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
 * `ush ush` line.  Each shell is forked with the target VT surface as
 * argv[1], so ush binds that exact console (#365 phase 3) — deterministic
 * placement, and a respawn returns to the same terminal.
 *
 * Two event sources feed one port set:
 *
 *   shell exits — reaping needs a wait-any, but libposix's waitpid serves
 *     only a specific pid (no pid==-1, no WNOHANG).  So we use the
 *     primitive underneath it: proc_subscribe_exit (#238) fires a one-shot
 *     proc_exit_msg_t to a notify port when a pid dies.  Subscribing every
 *     shell to ONE shared port makes a single receive a wait-any — the
 *     message carries the pid — with no threads.
 *
 *   VT switches — char_server sends a vt_switch_msg_t (VT_SWITCH_MSGH_ID)
 *     whenever the user switches to a virtual terminal (#365 phase 3).
 *     A shell is started lazily on first switch-to if none runs there yet,
 *     so a terminal only costs a shell once it is actually looked at.
 *
 * Phase 5 (login/auth) is future — there is no authentication yet.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <mach.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <servers/netname.h>

#include <vt/vt_ipc.h>     /* vt_switch_msg_t, VT_SWITCH_MSGH_ID, name */
#include "proc.h"          /* MIG: proc_subscribe_exit user stub */
#include "proc_types.h"    /* proc_exit_msg_t, PROC_EXIT_MSGID, PROC_OK */

extern mach_port_t __uros_proc_port;

/* mach_print is libmach_core's direct serial write — bypasses musl stdio
 * buffering so bring-up traces show up even when printf hasn't flushed. */
#include <mach/mach_traps.h>	/* the traps, declared once (#426) */

#define VTSLOG(s) mach_print("virtual_terminal_server: " s "\n")

/*
 * Absolute path of the shell binary in the boot filesystem.  ush lands in
 * /mach_servers/ush (make-disk-image.sh writes it after `cd mach_servers`);
 * userspace "/" is the boot-fs root, the same namespace in which ush itself
 * execs "/hello_dyn".
 */
#define VT_SHELL_PATH  "/mach_servers/ush"

/*
 * char_server exposes three VT consoles on surfaces 1..3 (#365 phase 1).
 * VT_BOOT_SHELLS shells come up eagerly at boot (surface 1, the default
 * view); the rest are lazy — started the first time the user switches to
 * their terminal.
 */
#define VT_FIRST_SURFACE   1
#define VT_NSURFACES       3
#define VT_BOOT_SHELLS     1

/* Cool-off before respawning an exited shell, so a shell that dies on
 * startup can't spin the supervisor into a fork bomb. */
#define VT_RESPAWN_PAUSE_US  200000

/* Give up respawning a VT whose shell keeps dying, so a shell that cannot
 * come up (e.g. its console is wedged) does not spin forever.  A deliberate
 * switch back to that VT resets the count and gives it a fresh chance. */
#define VT_MAX_RESPAWNS      5

/* Ports.  vts_exit_port collects the shell-exit notifications; vts_service_port is
 * the public port char_server sends VT switches to; both sit in vts_port_set so
 * one receive multiplexes them. */
static mach_port_t vts_port_set     = MACH_PORT_NULL;
static mach_port_t vts_exit_port    = MACH_PORT_NULL;
static mach_port_t vts_service_port = MACH_PORT_NULL;

/* Shell table, indexed by surface 1..N.  Single-threaded server, no lock. */
static pid_t shell_pid[VT_NSURFACES + 1];
static int   respawns[VT_NSURFACES + 1];
static int   live_shells;

/* ------------------------------------------------------------------ */
/*  Port bring-up                                                      */
/* ------------------------------------------------------------------ */

static int
ports_init(void)
{
	kern_return_t kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET,
	                        &vts_port_set);
	if (kr != KERN_SUCCESS) {
		printf("virtual_terminal_server: port set alloc kr=%d\n", (int)kr);
		return -1;
	}

	/* vts_exit_port — proc_subscribe_exit mints one-shot send-onces from its
	 * receive right, so it also needs a send right to be nameable. */
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	                        &vts_exit_port);
	if (kr != KERN_SUCCESS) {
		printf("virtual_terminal_server: exit port alloc kr=%d\n", (int)kr);
		return -1;
	}
	(void)mach_port_move_member(mach_task_self(), vts_exit_port, vts_port_set);
	kr = mach_port_insert_right(mach_task_self(), vts_exit_port, vts_exit_port,
	                            MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("virtual_terminal_server: exit port insert kr=%d\n", (int)kr);
		return -1;
	}

	/* vts_service_port — published so char_server can send VT switches. */
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	                        &vts_service_port);
	if (kr != KERN_SUCCESS) {
		printf("virtual_terminal_server: service port alloc kr=%d\n",
		       (int)kr);
		return -1;
	}
	(void)mach_port_move_member(mach_task_self(), vts_service_port, vts_port_set);
	kr = mach_port_insert_right(mach_task_self(), vts_service_port,
	                            vts_service_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("virtual_terminal_server: service port insert kr=%d\n",
		       (int)kr);
		return -1;
	}
	kr = netname_check_in(name_server_port, VT_SERVICE_NAME,
	                      mach_task_self(), vts_service_port);
	if (kr != NETNAME_SUCCESS)
		printf("virtual_terminal_server: netname_check_in(\"%s\") "
		       "kr=%d — VT switches won't reach us\n",
		       VT_SERVICE_NAME, (int)kr);
	else
		printf("virtual_terminal_server: registered \"%s\"\n",
		       VT_SERVICE_NAME);
	return 0;
}

/* Route pid's exit notification to vts_exit_port.  Already-zombie pids fire
 * immediately, so there is no lost-exit race with the spawn. */
static int
subscribe_exit(pid_t pid)
{
	int           rc = PROC_ERR_INVAL;
	kern_return_t kr;

	if (__uros_proc_port == MACH_PORT_NULL)
		return -1;

	kr = proc_subscribe_exit(__uros_proc_port, (proc_pid_t)pid,
	                         vts_exit_port, &rc);
	if (kr != KERN_SUCCESS || rc != PROC_OK) {
		printf("virtual_terminal_server: subscribe_exit(pid=%d) "
		       "kr=%d rc=%d\n", (int)pid, (int)kr, rc);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Shell spawn                                                        */
/* ------------------------------------------------------------------ */

/*
 * spawn_shell — fork a child that becomes a shell bound to VT `surface`
 * and subscribe its exit to the shared port.  Returns the child pid, -1.
 */
static pid_t
spawn_shell(int surface)
{
	char  vtarg[8];
	pid_t pid;

	snprintf(vtarg, sizeof vtarg, "%d", surface);

	pid = fork();
	if (pid < 0) {
		printf("virtual_terminal_server: fork failed errno=%d\n", errno);
		return -1;
	}
	if (pid == 0) {
		char *const sh_argv[] = { (char *)"ush", vtarg, NULL };
		char *const sh_env[]  = { NULL };

		execve(VT_SHELL_PATH, sh_argv, sh_env);
		printf("virtual_terminal_server: execve(%s) failed errno=%d\n",
		       VT_SHELL_PATH, errno);
		_exit(127);
	}

	(void)subscribe_exit(pid);   /* respawn is best-effort if this fails */
	return pid;
}

/* Start a shell on `surface` and record it.  Returns 0 on success. */
static int
start_shell(int surface)
{
	pid_t pid = spawn_shell(surface);

	if (pid < 0) {
		shell_pid[surface] = 0;
		return -1;
	}
	shell_pid[surface] = pid;
	live_shells++;
	printf("virtual_terminal_server: VT surface %d shell pid=%d\n",
	       surface, (int)pid);
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Event handlers                                                     */
/* ------------------------------------------------------------------ */

/* A shell exited: re-engage a fresh one on the *same* VT so it returns to
 * the terminal it left, unless it keeps dying (fork-bomb guard). */
static void
handle_exit(pid_t r, int exit_code)
{
	int surface;

	for (surface = VT_FIRST_SURFACE; surface <= VT_NSURFACES; surface++)
		if (shell_pid[surface] == r)
			break;
	if (surface > VT_NSURFACES)
		return;			/* not one of ours */

	shell_pid[surface] = 0;
	live_shells--;

	if (++respawns[surface] > VT_MAX_RESPAWNS) {
		printf("virtual_terminal_server: VT surface %d shell pid=%d "
		       "keeps dying — giving up on it\n", surface, (int)r);
		return;
	}

	printf("virtual_terminal_server: VT surface %d shell pid=%d exited "
	       "(code=%d), respawning\n", surface, (int)r, exit_code);
	usleep(VT_RESPAWN_PAUSE_US);
	(void)start_shell(surface);
}

/* The user switched to a VT: start a shell there lazily if none runs yet.
 * Surface 0 is the system log — it hosts no shell. */
static void
handle_switch(int surface)
{
	if (surface < VT_FIRST_SURFACE || surface > VT_NSURFACES)
		return;
	if (shell_pid[surface] != 0)
		return;			/* already has a shell */

	printf("virtual_terminal_server: VT surface %d activated — starting "
	       "shell\n", surface);
	respawns[surface] = 0;		/* deliberate visit → fresh chance */
	(void)start_shell(surface);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
	int surface;

	(void)argc;
	(void)argv;

	VTSLOG("starting up");

	for (surface = 0; surface <= VT_NSURFACES; surface++) {
		shell_pid[surface] = 0;
		respawns[surface]  = 0;
	}

	if (ports_init() < 0) {
		VTSLOG("port bring-up failed, exiting");
		return 1;
	}

	/* Eager boot shells; the rest come up lazily on VT switch. */
	for (surface = VT_FIRST_SURFACE;
	     surface < VT_FIRST_SURFACE + VT_BOOT_SHELLS &&
	     surface <= VT_NSURFACES; surface++)
		(void)start_shell(surface);

	if (live_shells == 0)
		VTSLOG("no boot shell — waiting for a VT switch to start one");

	/*
	 * Multiplex shell exits and VT switches on the one port set.  The
	 * buffer must hold the larger of the two message types plus the
	 * trailer mach_msg appends.
	 */
	for (;;) {
		union {
			proc_exit_msg_t   exit;
			vt_switch_msg_t   sw;
			mach_msg_header_t hdr;
			char              pad[256];
		} m;
		kern_return_t kr;

		kr = mach_msg(&m.hdr, MACH_RCV_MSG, 0, sizeof m, vts_port_set,
		              MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		if (kr != KERN_SUCCESS) {
			printf("virtual_terminal_server: recv kr=%d\n", (int)kr);
			continue;
		}

		switch (m.hdr.msgh_id) {
		case PROC_EXIT_MSGID:
			handle_exit((pid_t)m.exit.pid, (int)m.exit.exit_code);
			break;
		case VT_SWITCH_MSGH_ID:
			handle_switch((int)m.sw.surface);
			break;
		default:
			break;		/* stray message — ignore */
		}
	}

	return 0;
}
