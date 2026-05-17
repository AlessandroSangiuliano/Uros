/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef _PROC_TYPES_H_
#define _PROC_TYPES_H_

/*
 * proc_types.h — wire-stable types for the proc MIG subsystem
 * (#237 / proc_server v0.1.0; signals added in v0.2.0 / #238).
 */

#include <stdint.h>
#include <mach/vm_types.h>
#include <mach/message.h>

/* ------------------------------------------------------------------ */
/*  Server version (BSD-style major.minor.patch)                       */
/* ------------------------------------------------------------------ */

#define PROC_SERVER_VERSION_MAJOR    0
#define PROC_SERVER_VERSION_MINOR    2
#define PROC_SERVER_VERSION_PATCH    0
#define PROC_SERVER_VERSION_STRING   "0.2.0"

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define PROC_PID_NONE        0       /* no-parent / sentinel */
#define PROC_PID_INIT        1       /* reserved for future init */
#define PROC_MAX_TASKS      256      /* v0.1.0 fixed table size */
#define PROC_CMDLINE_MAX    128      /* truncated command name */

/* Process state — single ASCII char, mirrored in /proc/N/stat. */
#define PROC_STATE_RUNNING   'R'
#define PROC_STATE_ZOMBIE    'Z'

/* Mount path proc_server registers in name_server (libvfs routes
 * everything starting with this prefix to us). */
#define PROC_MOUNT_PATH      "/proc"

/* ------------------------------------------------------------------ */
/*  Wire types                                                          */
/* ------------------------------------------------------------------ */

typedef uint32_t            proc_pid_t;
typedef char                proc_cmdline_t[PROC_CMDLINE_MAX];
typedef pointer_t           proc_entry_array;

/*
 * proc_entry_t — one row in the proc_list reply.  Sent inline as
 * a fixed-size struct so the array marshalling stays simple.
 *
 *   pid       — server-assigned PID
 *   ppid      — parent PID, 0 if none
 *   state     — 'R' (running) or 'Z' (zombie)
 *   exit_code — meaningful only when state == 'Z'
 *   cmdline   — NUL-terminated command name
 *
 * sizeof(proc_entry_t) == 144 == 36 * uint32_t (matches proc.defs).
 */
typedef struct proc_entry {
    proc_pid_t  pid;                          /*   4 */
    proc_pid_t  ppid;                         /*   8 */
    uint8_t     state;                        /*   9 */
    uint8_t     _pad[3];                      /*  12 */
    int32_t     exit_code;                    /*  16 */
    char        cmdline[PROC_CMDLINE_MAX];    /* 144 */
} proc_entry_t;

/* ------------------------------------------------------------------ */
/*  Notification message — sent to subscribers when a pid exits        */
/* ------------------------------------------------------------------ */

#define PROC_EXIT_MSGID      3299

typedef struct proc_exit_msg {
    mach_msg_header_t   head;
    int32_t             exit_code;
    proc_pid_t          pid;
    mach_msg_trailer_t  trailer;
} proc_exit_msg_t;

/* ------------------------------------------------------------------ */
/*  Result codes                                                       */
/* ------------------------------------------------------------------ */

#define PROC_OK               0
#define PROC_ERR_INVAL       -1   /* bad arg */
#define PROC_ERR_NO_SLOT     -2   /* table full */
#define PROC_ERR_NOT_FOUND   -3   /* pid does not exist */
#define PROC_ERR_KERNEL      -4   /* underlying Mach call failed */
#define PROC_ERR_TOO_MANY    -5   /* list bigger than caller buffer */
#define PROC_ERR_NO_SIGPORT  -6   /* target has no signal_port registered */
#define PROC_ERR_BAD_SIGNO   -7   /* signo out of range / unknown */

/* ------------------------------------------------------------------ */
/*  Signals (v0.2.0 / #238)                                            */
/* ------------------------------------------------------------------ */

/*
 * Signal numbers — Linux/i386 layout for ease of porting libc later.
 * proc_server itself only special-cases SIGKILL/SIGSTOP/SIGCONT; the
 * other constants are just labels carried in the wire message and
 * interpreted by libposix-uros (#218).
 */
#define PROC_SIGHUP           1
#define PROC_SIGINT           2
#define PROC_SIGQUIT          3
#define PROC_SIGILL           4
#define PROC_SIGTRAP          5
#define PROC_SIGABRT          6
#define PROC_SIGBUS           7
#define PROC_SIGFPE           8
#define PROC_SIGKILL          9   /* cannot be caught/blocked */
#define PROC_SIGUSR1         10
#define PROC_SIGSEGV         11
#define PROC_SIGUSR2         12
#define PROC_SIGPIPE         13
#define PROC_SIGALRM         14
#define PROC_SIGTERM         15
#define PROC_SIGCHLD         17
#define PROC_SIGCONT         18
#define PROC_SIGSTOP         19   /* cannot be caught/blocked */
#define PROC_SIGTSTP         20
#define PROC_NSIG            32

#define PROC_SIGNAL_MSGID    3298

/*
 * Wire message proc_server sends on a target's signal_port for
 * catchable signals.  libposix-uros's handler thread receives this
 * and dispatches to sa_handler with sa_mask applied.
 *
 *   signo       — PROC_SIG* number
 *   sender_pid  — pid that issued proc_kill, or the dead child's pid
 *                 for SIGCHLD (so the parent can waitpid that pid)
 */
typedef struct proc_signal_msg {
    mach_msg_header_t   head;
    int32_t             signo;
    proc_pid_t          sender_pid;
    mach_msg_trailer_t  trailer;
} proc_signal_msg_t;

#endif /* _PROC_TYPES_H_ */
