/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * ush — Uros shell, v0.1.0 (#275.5).
 *
 * Minimal POSIX shell that demonstrates the controlling-tty + job-
 * control pipeline built in #275.1 .. #275.4-bis.  Boot sequence:
 *
 *   1. setsid()                                                # new session, leader
 *   2. netname_look_up("char") + query_devices                 # find a TTY device
 *   3. char_tty_acquire_ctty(dev_id, sid)                      # bind it as ctty
 *   4. open("/dev/tty", O_RDWR)                                # routed to char_server
 *   5. read line, parse "cmd args [&]", fork+execve            # job-control loop
 *      - foreground: setpgid(child,child), tcsetpgrp(fd,child),
 *                    waitpid(child), tcsetpgrp(fd, getpid())
 *      - background: setpgid(child,child), no tcsetpgrp
 *
 * v0.1.0 scope: absolute-path commands only ("/hello_exec"), single
 * "exit" builtin, no quoting, no globbing, no pipes/redirections.
 * Everything else is a future iteration once a real terminfo / line-
 * editing layer arrives.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mach.h>
#include <mach_init.h>
#include <mach/mach_port.h>
#include <servers/netname.h>

/* mach_print is libmach_core's direct serial write — bypasses musl stdio
 * buffering, useful for bring-up tracing where printf may not flush. */
#include <mach/mach_traps.h>	/* the traps, declared once (#426) */

#define USHLOG(s) mach_print("ush: " s "\n")

#include "char_server.h"        /* MIG: char_* user stubs */
#include <char/char_types.h>    /* struct char_device_info + CHAR_CLASS_* */
#include "proc.h"               /* MIG: proc_shutdown */
#include "proc_types.h"         /* PROC_SHUTDOWN_HALT / _REBOOT */
extern mach_port_t __uros_proc_port;

/* From libposix-uros — we want the canonical pid. */
extern unsigned int __uros_my_pid;

/*
 * The em-dash is fine again: #364 gave the gpu text renderer a
 * UTF-8 -> CP437 fold, so multibyte characters land as one legible glyph
 * instead of the garbage bytes that forced the ASCII workaround in #363.
 */
#define USH_BANNER     "\r\nush v0.1.0 — Uros shell (#275.5)\r\n"
#define USH_PROMPT     "ush$ "
#define USH_LINE_MAX   256

static mach_port_t char_port = MACH_PORT_NULL;
static int         tty_fd    = -1;
static pid_t       my_sid    = 0;

/* ------------------------------------------------------------------ */
/*  Setup                                                              */
/* ------------------------------------------------------------------ */

static int
lookup_char_server(void)
{
    char host[80] = "";
    char serv[80] = "char";
    kern_return_t kr;
    int spins;

    if (name_server_port == MACH_PORT_NULL) {
        printf("ush: name_server_port is NULL\n");
        return -1;
    }

    for (spins = 0; spins < 200; spins++) {
        kr = netname_look_up(name_server_port, host, serv, &char_port);
        if (kr == NETNAME_SUCCESS)
            return 0;
        usleep(50000);
    }
    printf("ush: netname_look_up(\"char\") timed out (kr=%d)\n", (int)kr);
    return -1;
}

/*
 * Collect candidate TTY dev_ids to try, best first: the on-screen
 * consoles (#363/#365 — one per VT) ahead of the UART fallback (serial /
 * headless / bench, when console.so is absent).  ush then claims the
 * first candidate it can acquire; since #365 gives every VT its own
 * console, a second ush skips the one already taken and lands on the
 * next VT.  Returns how many ids were written.
 */
static int
find_tty_devices(uint32_t *ids, unsigned int max)
{
    struct char_device_info *devs;
    vm_offset_t buf      = 0;
    mach_msg_type_number_t bcnt = 0;
    uint32_t n           = 0;
    kern_return_t kr;
    unsigned int i, cnt = 0;

    kr = char_query_devices(char_port, &buf, &bcnt, &n);
    if (kr != KERN_SUCCESS || n == 0)
        return 0;
    devs = (void *)buf;

    for (i = 0; i < n && cnt < max; i++)		/* consoles first */
        if (devs[i].class == CHAR_CLASS_TTY &&
            strcmp(devs[i].module_name, "console") == 0)
            ids[cnt++] = devs[i].id;
    for (i = 0; i < n && cnt < max; i++)		/* then UART etc. */
        if (devs[i].class == CHAR_CLASS_TTY &&
            strcmp(devs[i].module_name, "console") != 0)
            ids[cnt++] = devs[i].id;

    (void)vm_deallocate(mach_task_self(), buf, bcnt);
    return (int)cnt;
}

/*
 * ush_setup — become a session leader and bind a controlling tty.
 *
 * target_vt selects which VT console to claim (#365 phase 3): a positive
 * value N asks for the N-th on-screen console (surface N), so the
 * virtual_terminal_server can place a shell on a *specific* virtual
 * terminal — e.g. the one the user just switched to — instead of letting
 * every shell drift onto the next free VT.  target_vt == 0 keeps the
 * first-free walk used when ush is launched standalone (or headless, where
 * only the UART tty exists).
 */
static int
ush_setup(int target_vt)
{
    pid_t s;
    uint32_t dev_id = 0;
    char cap[256]   = { 0 };
    int rc          = 0;
    kern_return_t kr;

    s = setsid();
    if (s < 0) {
        /*
         * Bootstrap launches ush via exec_load, and proc_register makes
         * a fresh task its own pgrp leader; POSIX setsid then EPERMs.
         * Treat "already session leader of pid==sid" as success — the
         * preconditions for ctty_acquire are met either way.
         */
        if (errno == EPERM) {
            s = (pid_t)getsid(0);
            if (s == (pid_t)__uros_my_pid) {
                printf("ush: already session leader (sid=%d)\n", (int)s);
            } else {
                printf("ush: setsid failed errno=EPERM and sid=%d != pid=%u\n",
                       (int)s, __uros_my_pid);
                return -1;
            }
        } else {
            printf("ush: setsid failed errno=%d\n", errno);
            return -1;
        }
    } else {
        printf("ush: setsid -> sid=%d pid=%u\n", (int)s, __uros_my_pid);
    }
    my_sid = s;

    USHLOG("looking up char_server...");
    if (lookup_char_server() < 0)
        return -1;
    USHLOG("char_server resolved");

    /*
     * Bind a controlling tty.  A free console (or the UART) binds without
     * a cap (#275.5); a console already owned by another session fails the
     * acquire cap-check.
     *
     * find_tty_devices lists the consoles first, in surface order (the
     * console module hands out surfaces 1..N in that order, #365 phase 1),
     * so candidate index N-1 is the console for surface N.  A targeted
     * request therefore claims exactly cand[target_vt-1]; a standalone ush
     * (target_vt == 0) walks the list and takes the first it can get.
     */
    {
        uint32_t cand[8];
        int      ncand = find_tty_devices(cand, 8);
        int      c, bound = 0;

        if (ncand <= 0) {
            printf("ush: no TTY device found, falling back to dev_id=2\n");
            cand[0] = 2;
            ncand   = 1;
        }

        if (target_vt > 0 && target_vt <= ncand) {
            /* Targeted: claim exactly this VT, do not drift on failure. */
            c  = target_vt - 1;
            rc = 0;
            kr = char_tty_acquire_ctty(char_port, cap, 0,
                                       cand[c], (int)s, &rc);
            if (kr == KERN_SUCCESS && rc == 0) {
                dev_id = cand[c];
                bound  = 1;
            } else {
                printf("ush: VT %d (dev_id=%u) busy (kr=%d rc=%d)\n",
                       target_vt, cand[c], (int)kr, rc);
                return -1;
            }
        } else {
            /* First-free walk (standalone / headless UART). */
            for (c = 0; c < ncand; c++) {
                rc = 0;
                kr = char_tty_acquire_ctty(char_port, cap, 0,
                                           cand[c], (int)s, &rc);
                if (kr == KERN_SUCCESS && rc == 0) {
                    dev_id = cand[c];
                    bound  = 1;
                    break;
                }
            }
        }
        if (!bound) {
            printf("ush: could not acquire any tty (last kr=%d rc=%d)\n",
                   (int)kr, rc);
            return -1;
        }
    }
    printf("ush: bound ctty dev_id=%u to sid=%d\n", dev_id, (int)s);

    tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd < 0) {
        printf("ush: open(/dev/tty) failed errno=%d\n", errno);
        return -1;
    }
    printf("ush: opened /dev/tty -> fd=%d\n", tty_fd);

    /*
     * The tty line discipline sends the terminal signals to the whole
     * foreground process group (#397), and while no job is running that
     * group is the shell's own.  A shell must therefore not take the
     * default action for them: ^C at the prompt has to abandon the input
     * line, not kill the session.  Ignore them here and restore SIG_DFL
     * in the child before execve, so foreground jobs stay interruptible.
     */
    (void)signal(SIGINT,  SIG_IGN);
    (void)signal(SIGQUIT, SIG_IGN);
    (void)signal(SIGTSTP, SIG_IGN);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Terminal output                                                    */
/* ------------------------------------------------------------------ */

/*
 * tty_msg — user-facing status straight to the controlling tty.
 * The banner and prompt go through write(tty_fd); job-control
 * notifications must too, otherwise they sit in musl's full-buffered
 * stdout (fd 1 is not the tty) and never reach the user.
 */
static void
tty_msg(const char *fmt, ...)
{
    char    buf[128];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0)
        (void)write(tty_fd, buf, (size_t)n < sizeof buf ? (size_t)n
                                                        : sizeof buf - 1);
}

/* ------------------------------------------------------------------ */
/*  Tiny line reader                                                   */
/* ------------------------------------------------------------------ */

static ssize_t
read_line(char *buf, size_t cap_)
{
    size_t i = 0;
    char ch;
    ssize_t n;

    while (i + 1 < cap_) {
        n = read(tty_fd, &ch, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return 0;
        if (ch == '\r' || ch == '\n') {
            (void)write(tty_fd, "\r\n", 2);
            break;
        }
        if (ch == 0x7f || ch == 0x08) {     /* DEL / BS — quick win */
            if (i > 0) {
                i--;
                (void)write(tty_fd, "\b \b", 3);
            }
            continue;
        }
        buf[i++] = ch;
        (void)write(tty_fd, &ch, 1);   /* local echo */
    }
    buf[i] = '\0';
    return (ssize_t)i;
}

/* ------------------------------------------------------------------ */
/*  Command parsing                                                    */
/* ------------------------------------------------------------------ */

#define USH_MAX_ARGS 16

static int
parse_line(char *line, char *argv[USH_MAX_ARGS + 1], int *bg)
{
    int argc = 0;
    char *p  = line;
    size_t len = strlen(line);

    *bg = 0;
    /* Trim trailing space + detect '&'. */
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t'))
        line[--len] = '\0';
    if (len > 0 && line[len - 1] == '&') {
        *bg = 1;
        line[--len] = '\0';
        while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t'))
            line[--len] = '\0';
    }

    while (*p && argc < USH_MAX_ARGS) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

/* ------------------------------------------------------------------ */
/*  Job-control spawn                                                  */
/* ------------------------------------------------------------------ */

static void
run_command(char *argv[], int bg)
{
    pid_t pid;
    pid_t pgid_self = getpid();
    int status      = 0;

    pid = fork();
    if (pid < 0) {
        printf("ush: fork failed errno=%d\n", errno);
        return;
    }
    if (pid == 0) {
        /* Child: own pgrp, foreground if requested, then exec. */
        (void)setpgid(0, 0);
        if (!bg)
            (void)tcsetpgrp(tty_fd, getpid());
        /* Undo the shell's own guard (#397): a job must be interruptible
         * even though the shell that spawned it ignores these. */
        (void)signal(SIGINT,  SIG_DFL);
        (void)signal(SIGQUIT, SIG_DFL);
        (void)signal(SIGTSTP, SIG_DFL);
        execve(argv[0], argv, (char *const[]){ NULL });
        printf("ush: execve(%s) failed errno=%d\n", argv[0], errno);
        _exit(127);
    }

    /* Parent: race-safe duplicate of setpgid; tcsetpgrp before wait. */
    (void)setpgid(pid, pid);
    if (bg) {
        tty_msg("[bg] pid=%d\r\n", (int)pid);
        return;
    }
    (void)tcsetpgrp(tty_fd, pid);
    (void)waitpid(pid, &status, 0);
    (void)tcsetpgrp(tty_fd, pgid_self);
    tty_msg("ush: pid=%d exit=0x%x\r\n", (int)pid, status);
}

/* ------------------------------------------------------------------ */
/*  Main loop                                                          */
/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    char  line[USH_LINE_MAX];
    char *parts[USH_MAX_ARGS + 1];
    int   n, bg;
    int   rstatus;
    int   target_vt = 0;

    /* argv[1], when present, is the VT surface to bind (#365 phase 3):
     * the virtual_terminal_server passes it so this shell lands on a
     * specific virtual terminal. */
    if (argc > 1 && argv[1] != NULL)
        target_vt = atoi(argv[1]);

    if (ush_setup(target_vt) < 0) {
        printf("ush: setup failed, exiting\n");
        return 1;
    }
    (void)write(tty_fd, USH_BANNER, strlen(USH_BANNER));

    for (;;) {
        /* Reap finished background jobs before prompting: without this
         * every "&" child that exited or was killed stays a zombie
         * holding its proc pid slot, and a long session walls the whole
         * system at PROC_MAX_TASKS spawns (the kill x fork storm hit
         * the 256 wall at iteration ~254 every run). */
        while (waitpid(-1, &rstatus, WNOHANG) > 0)
            ;

        (void)write(tty_fd, USH_PROMPT, strlen(USH_PROMPT));
        ssize_t r = read_line(line, sizeof line);
        if (r < 0)  { printf("ush: read error\n"); break; }
        if (r == 0) continue;

        n = parse_line(line, parts, &bg);
        if (n == 0) continue;

        if (strcmp(parts[0], "exit") == 0 || strcmp(parts[0], "quit") == 0)
            break;

        if (strcmp(parts[0], "shutdown") == 0 ||
            strcmp(parts[0], "halt")     == 0 ||
            strcmp(parts[0], "poweroff") == 0 ||
            strcmp(parts[0], "reboot")   == 0 ||
            strcmp(parts[0], "restart")  == 0) {
            int reboot = (strcmp(parts[0], "reboot") == 0 ||
                          strcmp(parts[0], "restart") == 0);
            int rc = 0;
            kern_return_t kr;

            printf("ush: requesting %s...\n", reboot ? "reboot" : "shutdown");
            if (__uros_proc_port == MACH_PORT_NULL) {
                printf("ush: proc_server unavailable\n");
                continue;
            }
            kr = proc_shutdown(__uros_proc_port,
                               reboot ? PROC_SHUTDOWN_REBOOT
                                      : PROC_SHUTDOWN_HALT,
                               &rc);
            /* On success host_reboot does not return; reaching here is
             * itself the error path. */
            printf("ush: proc_shutdown returned kr=%d rc=%d\n",
                   (int)kr, rc);
            continue;
        }

        /* "kill [-SIG] pid..." — send a signal by pid.  Default SIGTERM;
         * "kill -9 <pid>" is the direct trigger for proc's SIGKILL ->
         * task_terminate() path (used to exercise the #380 cross-CPU
         * stop of a task busy-spinning on another CPU). */
        if (strcmp(parts[0], "kill") == 0) {
            int sig = SIGTERM;
            int ai  = 1;

            if (n >= 2 && parts[1][0] == '-') {
                sig = atoi(parts[1] + 1);
                ai  = 2;
            }
            if (ai >= n) {
                printf("usage: kill [-SIG] pid...\n");
                continue;
            }
            for (; ai < n; ai++) {
                int pid = atoi(parts[ai]);
                if (kill(pid, sig) != 0)
                    printf("ush: kill %d (sig %d) failed errno=%d\n",
                           pid, sig, errno);
                else
                    printf("ush: sent sig %d to pid %d\n", sig, pid);
            }
            continue;
        }

        if (parts[0][0] != '/') {
            printf("ush: only absolute paths supported (got \"%s\")\n",
                   parts[0]);
            continue;
        }
        run_command(parts, bg);
    }

    printf("ush: bye\n");
    return 0;
}
