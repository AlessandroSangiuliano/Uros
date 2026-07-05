/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * virtual_terminal_server — Uros virtual-terminal shell supervisor (#365).
 *
 * The distributor of the terminal gearbox: it does not itself drive any
 * tty, it only mounts a shell onto each virtual terminal and re-engages a
 * fresh one whenever a shell throws its belt.  char_server owns the
 * console-TTY devices (one per VT, #363/#364); this server owns the shells
 * that run on them.
 *
 * Boot role: launched as the last stage-2 task in place of the old direct
 * `ush ush` line.  Each ush it forks calls setsid() + acquires the first
 * *free* VT console as its controlling tty (#365 phase 2a), so the shells
 * fan out across the virtual terminals with no per-shell argv needed.
 *
 * Reaping the shells needs a wait-any, but libposix's waitpid only serves
 * one specific pid at a time (no pid==-1, no WNOHANG).  So instead of
 * waitpid we lean on the primitive underneath it: proc_subscribe_exit
 * (#238) fires a one-shot proc_exit_msg_t to a notify port when a pid dies.
 * Subscribing every shell to ONE shared port turns a single mach_msg(RCV)
 * into a wait-any — the message carries the pid that exited — with no
 * threads and no change to proc_server.
 *
 * Incremental bring-up (#365 phase 2b):
 *   step 2 — spawn ONE ush, verify boot reaches `ush$`   [done]
 *   step 3 — spawn a second shell on the next VT          [done]
 *   step 4 — respawn a shell when it exits                [done]
 * Phase 3 (lazy spawn on Ctrl+Alt+Fn) and phase 5 (login/auth) come later.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <mach.h>
#include <mach/mach_port.h>
#include <mach/message.h>

#include "proc.h"          /* MIG: proc_subscribe_exit user stub */
#include "proc_types.h"    /* proc_exit_msg_t, PROC_EXIT_MSGID, PROC_OK */

extern mach_port_t __uros_proc_port;

/* mach_print is libmach_core's direct serial write — bypasses musl stdio
 * buffering so bring-up traces show up even when printf hasn't flushed. */
extern void mach_print(const char *);

#define VTSLOG(s) mach_print("virtual_terminal_server: " s "\n")

/*
 * Absolute path of the shell binary in the boot filesystem.  ush lands in
 * /mach_servers/ush (make-disk-image.sh writes it after `cd mach_servers`);
 * userspace "/" is the boot-fs root, the same namespace in which ush itself
 * execs "/hello_dyn".
 */
#define VT_SHELL_PATH  "/mach_servers/ush"

/*
 * How many shells to bring up at boot.  char_server exposes three VT
 * consoles (surfaces 1/2/3, #365 phase 1); each ush claims the first free
 * one, so two shells land on two distinct VTs.  Phase 3 will make this
 * lazy — a shell only when a VT is first switched to — so this eager count
 * is a stepping stone, deliberately below the console count.
 */
#define VT_MAX_SHELLS      3
#define VT_BOOT_SHELLS     2

/* Cool-off before respawning an exited shell, so a shell that dies on
 * startup can't spin the supervisor into a fork bomb. */
#define VT_RESPAWN_PAUSE_US  200000

/* Shared receive port every shell's exit is routed to (see wait_any). */
static mach_port_t exit_port = MACH_PORT_NULL;

/* ------------------------------------------------------------------ */
/*  Exit plumbing                                                      */
/* ------------------------------------------------------------------ */

static int
exit_port_init(void)
{
	kern_return_t kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	                        &exit_port);
	if (kr != KERN_SUCCESS) {
		printf("virtual_terminal_server: exit port alloc failed kr=%d\n",
		       (int)kr);
		return -1;
	}
	/* A send right to name the port; proc_subscribe_exit mints its
	 * one-shot send-once from our receive right on each call. */
	kr = mach_port_insert_right(mach_task_self(), exit_port, exit_port,
	                            MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("virtual_terminal_server: exit port insert failed kr=%d\n",
		       (int)kr);
		return -1;
	}
	return 0;
}

/* Route pid's exit notification to the shared port.  Already-zombie pids
 * fire immediately, so there is no lost-exit race with the spawn. */
static int
subscribe_exit(pid_t pid)
{
	int           rc = PROC_ERR_INVAL;
	kern_return_t kr;

	if (__uros_proc_port == MACH_PORT_NULL)
		return -1;

	kr = proc_subscribe_exit(__uros_proc_port, (proc_pid_t)pid,
	                         exit_port, &rc);
	if (kr != KERN_SUCCESS || rc != PROC_OK) {
		printf("virtual_terminal_server: subscribe_exit(pid=%d) "
		       "kr=%d rc=%d\n", (int)pid, (int)kr, rc);
		return -1;
	}
	return 0;
}

/* Block until any subscribed shell exits; returns its pid, -1 on error. */
static pid_t
wait_any(int *exit_code)
{
	proc_exit_msg_t msg;
	kern_return_t   kr;

	kr = mach_msg(&msg.head, MACH_RCV_MSG, 0, sizeof msg, exit_port,
	              MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (kr != KERN_SUCCESS || msg.head.msgh_id != PROC_EXIT_MSGID)
		return -1;

	if (exit_code)
		*exit_code = (int)msg.exit_code;
	return (pid_t)msg.pid;
}

/* ------------------------------------------------------------------ */
/*  Shell spawn                                                        */
/* ------------------------------------------------------------------ */

/*
 * spawn_shell — fork a child that becomes a shell on the next free VT and
 * subscribe its exit to the shared port.  Returns the child pid, or -1.
 */
static pid_t
spawn_shell(void)
{
	pid_t pid = fork();

	if (pid < 0) {
		printf("virtual_terminal_server: fork failed errno=%d\n", errno);
		return -1;
	}
	if (pid == 0) {
		char *const sh_argv[] = { (char *)"ush", NULL };
		char *const sh_env[]  = { NULL };

		execve(VT_SHELL_PATH, sh_argv, sh_env);
		printf("virtual_terminal_server: execve(%s) failed errno=%d\n",
		       VT_SHELL_PATH, errno);
		_exit(127);
	}

	(void)subscribe_exit(pid);   /* respawn is best-effort if this fails */
	return pid;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
	pid_t shells[VT_MAX_SHELLS];
	int   live = 0;
	int   i;

	(void)argc;
	(void)argv;

	VTSLOG("starting up");

	if (exit_port_init() < 0) {
		VTSLOG("no exit port — shells will run but won't be respawned");
		exit_port = MACH_PORT_NULL;
	}

	/* Bring up the boot shells; each lands on the next free VT. */
	for (i = 0; i < VT_BOOT_SHELLS; i++) {
		shells[i] = spawn_shell();
		if (shells[i] < 0)
			continue;
		printf("virtual_terminal_server: VT %d shell pid=%d\n",
		       i, (int)shells[i]);
		live++;
	}
	if (live == 0) {
		VTSLOG("could not spawn any shell, exiting");
		return 1;
	}

	/*
	 * Supervise: reap whichever shell exits and re-engage a fresh one on
	 * the same VT slot.  ush only exits on an explicit "exit", so this is
	 * user-driven, not a spin — the cool-off is just belt-and-braces
	 * against a shell that dies during startup.
	 */
	for (;;) {
		int   exit_code = 0;
		pid_t r         = wait_any(&exit_code);

		if (r < 0) {
			VTSLOG("wait_any failed, exiting");
			break;
		}

		for (i = 0; i < VT_BOOT_SHELLS; i++)
			if (shells[i] == r)
				break;
		if (i == VT_BOOT_SHELLS)
			continue;		/* not one of ours */

		printf("virtual_terminal_server: VT %d shell pid=%d exited "
		       "(code=%d), respawning\n", i, (int)r, exit_code);

		usleep(VT_RESPAWN_PAUSE_US);
		shells[i] = spawn_shell();
		if (shells[i] < 0) {
			printf("virtual_terminal_server: VT %d respawn failed\n",
			       i);
			if (--live == 0) {
				VTSLOG("no live shells left, exiting");
				break;
			}
		}
	}

	return 0;
}
