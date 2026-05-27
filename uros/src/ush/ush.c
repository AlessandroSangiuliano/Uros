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
extern void mach_print(const char *);

#define USHLOG(s) mach_print("ush: " s "\n")

#include "char_server.h"        /* MIG: char_* user stubs */
#include <char/char_types.h>    /* struct char_device_info + CHAR_CLASS_* */

/* From libposix-uros — we want the canonical pid. */
extern unsigned int __uros_my_pid;

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

static int
find_tty_device(uint32_t *out_dev_id)
{
    struct char_device_info *devs;
    vm_offset_t buf      = 0;
    mach_msg_type_number_t bcnt = 0;
    uint32_t n           = 0;
    kern_return_t kr;
    unsigned int i;

    kr = char_query_devices(char_port, &buf, &bcnt, &n);
    if (kr != KERN_SUCCESS || n == 0)
        return -1;
    devs = (void *)buf;
    for (i = 0; i < n; i++) {
        if (devs[i].class == CHAR_CLASS_TTY) {
            *out_dev_id = devs[i].id;
            (void)vm_deallocate(mach_task_self(), buf, bcnt);
            return 0;
        }
    }
    (void)vm_deallocate(mach_task_self(), buf, bcnt);
    return -1;
}

static int
ush_setup(void)
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

    /* v0.1: prefer the first CHAR_CLASS_TTY device reported by
     * char_query_devices.  Fall back to UART (dev_id=2 in current
     * boot order) if discovery fails. */
    if (find_tty_device(&dev_id) < 0) {
        printf("ush: no TTY device found, falling back to dev_id=2\n");
        dev_id = 2;
    }

    kr = char_tty_acquire_ctty(char_port, cap, 0, dev_id, (int)s, &rc);
    if (kr != KERN_SUCCESS || rc != 0) {
        printf("ush: tty_acquire_ctty kr=%d rc=%d\n", (int)kr, rc);
        return -1;
    }
    printf("ush: bound ctty dev_id=%u to sid=%d\n", dev_id, (int)s);

    tty_fd = open("/dev/tty", O_RDWR);
    if (tty_fd < 0) {
        printf("ush: open(/dev/tty) failed errno=%d\n", errno);
        return -1;
    }
    printf("ush: opened /dev/tty -> fd=%d\n", tty_fd);
    return 0;
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
        /* No backspace handling — kept minimal for v0.1. */
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
        execve(argv[0], argv, (char *const[]){ NULL });
        printf("ush: execve(%s) failed errno=%d\n", argv[0], errno);
        _exit(127);
    }

    /* Parent: race-safe duplicate of setpgid; tcsetpgrp before wait. */
    (void)setpgid(pid, pid);
    if (bg) {
        printf("[bg] pid=%d\n", (int)pid);
        return;
    }
    (void)tcsetpgrp(tty_fd, pid);
    (void)waitpid(pid, &status, 0);
    (void)tcsetpgrp(tty_fd, pgid_self);
    printf("ush: pid=%d exit=0x%x\n", (int)pid, status);
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

    (void)argc; (void)argv;

    if (ush_setup() < 0) {
        printf("ush: setup failed, exiting\n");
        return 1;
    }
    (void)write(tty_fd, USH_BANNER, strlen(USH_BANNER));

    for (;;) {
        (void)write(tty_fd, USH_PROMPT, strlen(USH_PROMPT));
        ssize_t r = read_line(line, sizeof line);
        if (r < 0)  { printf("ush: read error\n"); break; }
        if (r == 0) continue;

        n = parse_line(line, parts, &bg);
        if (n == 0) continue;

        if (strcmp(parts[0], "exit") == 0 || strcmp(parts[0], "quit") == 0)
            break;

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
