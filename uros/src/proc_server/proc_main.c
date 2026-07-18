/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * proc_main.c — main + Mach msg loop + MIG handlers + pid table for
 * proc_server v0.1.0 (#237).
 *
 * Single translation unit for v0.1.0; split when it crosses ~1k lines.
 *
 * What lives here:
 *   - pid_table: PROC_MAX_TASKS-slot fixed array, mutex-protected
 *   - DEAD_NAME notify subscription per registered task
 *   - subscribe_exit waiter slot per pid (single waiter v0.1.0)
 *   - MIG handlers proc_S_register / _subscribe_exit / _list
 *   - vfs.defs handlers (vfs_open / vfs_read / vfs_close /
 *                        vfs_fstat / vfs_stat) for /proc/N/stat
 *   - Multi-subsystem demux: proc + vfs + notify
 */

#include "proc_types.h"

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/bootstrap.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <mach/notify.h>
#include <mach/mach_interface.h>
#include <mach/task_info.h>
#include <mach/host_reboot.h>
#include <mach/mach_host.h>    /* host_reboot user stub */
#include <sa_mach.h>
#include <pthread.h>
#include <servers/netname.h>
#include <servers/netname_defs.h>
#include <vfs_types.h>
#include <char/char_module_abi.h>  /* CHAR_CTTY_RELEASE_MSGH_ID (#365) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* MIG-generated headers — proc + vfs + notify. */
#include "proc_server.h"
#include "vfs_server.h"
#include "vfs.h"             /* fs_sync() client stub (#284 shutdown drain) */
#include "notify_server.h"

/* ------------------------------------------------------------------ */
/*  Globals from bootstrap_ports                                       */
/* ------------------------------------------------------------------ */

static mach_port_t      host_port;
static mach_port_t      device_port;
static mach_port_t      security_port;
static mach_port_t      root_ledger_wired;
static mach_port_t      root_ledger_paged;

static mach_port_t      proc_port;        /* proc.defs RPCs */
static mach_port_t      mount_port;       /* /proc fs.defs RPCs */
static mach_port_t      notify_port;      /* DEAD_NAME notifications */
static mach_port_t      port_set;         /* covers all three above */

/* ------------------------------------------------------------------ */
/*  pid_table                                                           */
/* ------------------------------------------------------------------ */

struct pid_entry {
    int             in_use;
    proc_pid_t      pid;
    proc_pid_t      ppid;
    proc_pid_t      pgrp_id;         /* POSIX process group (v0.3.0) */
    proc_pid_t      sid;             /* POSIX session id    (v0.3.0) */

    /* Controlling-tty / job control (v0.4.0 / #247).  Meaningful only on
     * a session-leader entry (pid == sid); other members reach them by
     * looking up their sid's leader.  ctty_port == MACH_PORT_NULL means
     * the session has no controlling terminal. */
    mach_port_t     ctty_port;       /* leader-only: send right to tty */
    proc_pid_t      fg_pgrp_id;      /* leader-only: foreground pgrp */

    /* Job-control stop state: set when proc_server suspends the task for
     * SIGSTOP, cleared on SIGCONT, so a continue can durably resume the
     * whole stopped process group (#247). */
    uint8_t         stopped;
    mach_port_t     task_port;       /* receive-side perspective */
    char            cmdline[PROC_CMDLINE_MAX];
    uint8_t         state;           /* PROC_STATE_* */
    int32_t         exit_code;

    /* Single subscriber for v0.1.0; v0.x.0 widens to a list. */
    mach_port_t     exit_notify;     /* send-once right, MACH_PORT_NULL if none */

    /* Signal port (v0.2.0 / #238) — send right held by proc_server;
     * catchable signals are delivered here, and SIGCHLD goes to the
     * parent's slot when this pid dies. */
    mach_port_t     signal_port;

    /* Cached resource accounting (v0.4.0 / #240) — refreshed on
     * proc_getrusage() and on /proc/N/stat reads.  Zombies keep the
     * last snapshot taken before task termination. */
    proc_rusage_t   last_rusage;

    /* Open-handle table for /proc/N/stat — one slot per pid_entry. */
    int             stat_handle_in_use;
    vfs_u64_t       stat_handle_id;
    vfs_u64_t       stat_offset;     /* read offset client-side mirror */
};

static struct pid_entry  pid_table[PROC_MAX_TASKS];
static proc_pid_t        next_pid = PROC_PID_INIT + 1;
static pthread_mutex_t   pid_lock = PTHREAD_MUTEX_INITIALIZER;
static vfs_u64_t         next_stat_handle = 1;

static struct pid_entry *
find_by_pid_locked(proc_pid_t pid)
{
    int i;
    for (i = 0; i < PROC_MAX_TASKS; i++)
        if (pid_table[i].in_use && pid_table[i].pid == pid)
            return &pid_table[i];
    return NULL;
}

static struct pid_entry *
find_by_task_locked(mach_port_t task)
{
    int i;
    for (i = 0; i < PROC_MAX_TASKS; i++)
        if (pid_table[i].in_use && pid_table[i].task_port == task)
            return &pid_table[i];
    return NULL;
}

/*
 * Find a session's leader entry — the in-use pid whose pid == sid (a
 * session id is the leader's pid by construction, see proc_setsid).
 * The controlling-tty state (#247) lives there.  Returns NULL if the
 * leader is gone (e.g. the session leader already exited).
 */
static struct pid_entry *
find_session_leader_locked(proc_pid_t sid)
{
    struct pid_entry *e;

    if (sid == 0)
        return NULL;
    e = find_by_pid_locked(sid);
    if (e && e->sid == sid)
        return e;
    return NULL;
}

/*
 * Send the one-shot exit notification to a subscriber.  Caller has
 * dropped the lock so we don't hold it across a mach_msg.  Consumes
 * the send-once right regardless of success.
 */
static void
fire_exit_notify(mach_port_t notify, proc_pid_t pid, int32_t exit_code)
{
    proc_exit_msg_t msg;

    if (notify == MACH_PORT_NULL)
        return;

    memset(&msg, 0, sizeof(msg));
    msg.head.msgh_bits        = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    msg.head.msgh_size        = sizeof(msg) - sizeof(mach_msg_trailer_t);
    msg.head.msgh_remote_port = notify;
    msg.head.msgh_local_port  = MACH_PORT_NULL;
    msg.head.msgh_id          = PROC_EXIT_MSGID;
    msg.exit_code             = exit_code;
    msg.pid                   = pid;

    (void)mach_msg(&msg.head, MACH_SEND_MSG,
                   sizeof(msg) - sizeof(mach_msg_trailer_t),
                   0, MACH_PORT_NULL,
                   MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

/* ------------------------------------------------------------------ */
/*  proc.defs MIG handlers                                              */
/* ------------------------------------------------------------------ */

kern_return_t
proc_S_register(
    mach_port_t                 server_port,
    proc_pid_t                  parent_pid,
    mach_port_t                 task_port,
    proc_cmdline_t              cmdline,
    proc_pid_t                  *new_pid,
    int                         *result)
{
    int i;
    struct pid_entry *e = NULL;
    mach_port_t prev = MACH_PORT_NULL;
    kern_return_t kr;
    proc_pid_t pid;

    (void)server_port;

    pthread_mutex_lock(&pid_lock);

    /* #287: idempotent registration by task port.  execve on Uros is
     * "spawn + suicide": the exec path already registered the new task
     * (inheriting the parent's pgrp + session) before the fresh image
     * runs.  When that image's libposix init then self-registers with
     * its own task port, return the identity the exec path already
     * minted instead of allocating a new self-leader pid — otherwise the
     * exec'd child lands in a brand-new session (sid = its own pid) and
     * loses the controlling tty / job-control context (its /dev/tty
     * lookup fails and stdout silently falls back to the mach_print
     * console, racing the shell's own output).  Mach coalesces send
     * rights to the same port, so the self-register's task_port name
     * matches the stored one. */
    for (i = 0; i < PROC_MAX_TASKS; i++) {
        if (pid_table[i].in_use && pid_table[i].task_port == task_port) {
            proc_pid_t existing = pid_table[i].pid;
            pthread_mutex_unlock(&pid_lock);
            /* Drop the extra uref MIG just handed us; the existing entry
             * already holds its own reference to the same port. */
            (void)mach_port_deallocate(mach_task_self(), task_port);
            *new_pid = existing;
            *result  = PROC_OK;
            return KERN_SUCCESS;
        }
    }

    for (i = 0; i < PROC_MAX_TASKS; i++) {
        if (!pid_table[i].in_use) {
            e = &pid_table[i];
            break;
        }
    }
    if (!e) {
        pthread_mutex_unlock(&pid_lock);
        *new_pid = 0;
        *result  = PROC_ERR_NO_SLOT;
        return KERN_SUCCESS;
    }

    pid = next_pid++;
    e->in_use     = 1;
    e->pid        = pid;
    e->ppid       = parent_pid;
    /* POSIX fork inherits pgrp + session from parent (v0.3.0).  If
     * the parent isn't tracked (parent_pid == 0 or unknown), the new
     * pid is a self-leader: pgrp = sid = pid. */
    {
        struct pid_entry *parent_entry = (parent_pid != PROC_PID_NONE)
            ? find_by_pid_locked(parent_pid) : NULL;
        if (parent_entry) {
            e->pgrp_id = parent_entry->pgrp_id;
            e->sid     = parent_entry->sid;
        } else {
            e->pgrp_id = pid;
            e->sid     = pid;
        }
    }
    e->task_port  = task_port;
    e->state      = PROC_STATE_RUNNING;
    e->exit_code  = 0;
    e->exit_notify = MACH_PORT_NULL;
    e->signal_port = MACH_PORT_NULL;
    memset(&e->last_rusage, 0, sizeof(e->last_rusage));
    e->stat_handle_in_use = 0;
    (void)strncpy(e->cmdline, cmdline, sizeof(e->cmdline) - 1);
    e->cmdline[sizeof(e->cmdline) - 1] = '\0';
    pthread_mutex_unlock(&pid_lock);

    /* Subscribe to DEAD_NAME on the task port — fires when the task
     * is destroyed, lets us mark zombie + fire notifications.
     * The notify request returns the previous notification port for
     * this name; v0.1.0 doesn't track it. */
    kr = mach_port_request_notification(mach_task_self(), task_port,
                                        MACH_NOTIFY_DEAD_NAME, 0,
                                        notify_port,
                                        MACH_MSG_TYPE_MAKE_SEND_ONCE,
                                        &prev);
    if (prev != MACH_PORT_NULL)
        (void)mach_port_deallocate(mach_task_self(), prev);
    if (kr != KERN_SUCCESS) {
        printf("proc: dead-name request kr=%d (pid=%u)\n", kr, pid);
        /* Not fatal — pid stays alive in the table, just no automatic
         * zombie marking.  Tests that race here will see the pid
         * stuck at RUNNING. */
    }

    printf("proc: registered pid=%u ppid=%u task=0x%x cmd=\"%s\"\n",
           pid, parent_pid, (unsigned)task_port, e->cmdline);

    *new_pid = pid;
    *result  = PROC_OK;
    return KERN_SUCCESS;
}

/*
 * #287: reassign an existing pid to a freshly-spawned task so execve
 * preserves the caller's pid (POSIX in-place-exec semantics emulated on
 * the "spawn + suicide" model).  Swap the entry's task_port to new_task,
 * re-arm the dead-name watch on it, refresh the cmdline, and keep
 * pid/ppid/pgrp/sid.  The caller's old task then suicides; its dead-name
 * no longer matches any entry (we dropped our ref) and is ignored.
 */
kern_return_t
proc_S_exec_handoff(
    mach_port_t                 server_port,
    proc_pid_t                  old_pid,
    mach_port_t                 new_task,
    proc_cmdline_t              cmdline,
    int                         *result)
{
    struct pid_entry *e;
    mach_port_t old_port = MACH_PORT_NULL;
    mach_port_t prev = MACH_PORT_NULL;
    kern_return_t kr;

    (void)server_port;

    pthread_mutex_lock(&pid_lock);
    e = find_by_pid_locked(old_pid);
    if (!e) {
        pthread_mutex_unlock(&pid_lock);
        if (new_task != MACH_PORT_NULL)
            (void)mach_port_deallocate(mach_task_self(), new_task);
        *result = PROC_ERR_NOT_FOUND;
        return KERN_SUCCESS;
    }
    old_port      = e->task_port;
    e->task_port  = new_task;
    e->state      = PROC_STATE_RUNNING;
    (void)strncpy(e->cmdline, cmdline, sizeof(e->cmdline) - 1);
    e->cmdline[sizeof(e->cmdline) - 1] = '\0';
    pthread_mutex_unlock(&pid_lock);

    /* Watch the new task for death so waitpid still reaps old_pid. */
    kr = mach_port_request_notification(mach_task_self(), new_task,
                                        MACH_NOTIFY_DEAD_NAME, 0,
                                        notify_port,
                                        MACH_MSG_TYPE_MAKE_SEND_ONCE,
                                        &prev);
    if (prev != MACH_PORT_NULL)
        (void)mach_port_deallocate(mach_task_self(), prev);
    if (kr != KERN_SUCCESS)
        printf("proc: exec_handoff dead-name request kr=%d (pid=%u)\n",
               kr, old_pid);

    /* Drop our ref to the old (about-to-suicide) task port.  Its pending
     * dead-name notification, when it fires, won't match any entry. */
    if (old_port != MACH_PORT_NULL && old_port != new_task)
        (void)mach_port_deallocate(mach_task_self(), old_port);

    printf("proc: exec_handoff pid=%u -> task=0x%x cmd=\"%s\"\n",
           old_pid, (unsigned)new_task, e->cmdline);

    *result = PROC_OK;
    return KERN_SUCCESS;
}

kern_return_t
proc_S_subscribe_exit(
    mach_port_t                 server_port,
    proc_pid_t                  pid,
    mach_port_t                 notify,
    int                         *result)
{
    struct pid_entry *e;
    int fire_now = 0;
    int32_t code = 0;
    mach_port_t reaped_sigport = MACH_PORT_NULL;

    (void)server_port;

    pthread_mutex_lock(&pid_lock);
    e = find_by_pid_locked(pid);
    if (!e) {
        pthread_mutex_unlock(&pid_lock);
        if (notify != MACH_PORT_NULL)
            (void)mach_port_deallocate(mach_task_self(), notify);
        *result = PROC_ERR_NOT_FOUND;
        return KERN_SUCCESS;
    }
    if (e->state == PROC_STATE_ZOMBIE) {
        fire_now = 1;
        code = e->exit_code;
        /*
         * #378: the caller is reaping this zombie now, so release its
         * pid-table slot — otherwise the fixed-size table (PROC_MAX_TASKS)
         * leaks one entry per process ever run.  task_port was already
         * dropped when the dead-name notification deallocated it; only
         * signal_port may still hold a (now dead-name) send right, freed
         * below the lock.  memset clears in_use so the allocator reuses it.
         */
        reaped_sigport = e->signal_port;
        memset(e, 0, sizeof(*e));
    } else {
        /* Replace any previous waiter (v0.1.0 single-slot). */
        if (e->exit_notify != MACH_PORT_NULL) {
            mach_port_t old = e->exit_notify;
            e->exit_notify = MACH_PORT_NULL;
            pthread_mutex_unlock(&pid_lock);
            (void)mach_port_deallocate(mach_task_self(), old);
            pthread_mutex_lock(&pid_lock);
            e = find_by_pid_locked(pid);
            if (!e) {
                pthread_mutex_unlock(&pid_lock);
                if (notify != MACH_PORT_NULL)
                    (void)mach_port_deallocate(mach_task_self(),
                                               notify);
                *result = PROC_ERR_NOT_FOUND;
                return KERN_SUCCESS;
            }
        }
        e->exit_notify = notify;
    }
    pthread_mutex_unlock(&pid_lock);

    if (reaped_sigport != MACH_PORT_NULL)
        (void)mach_port_deallocate(mach_task_self(), reaped_sigport);

    if (fire_now)
        fire_exit_notify(notify, pid, code);

    *result = PROC_OK;
    return KERN_SUCCESS;
}

/*
 * proc_reap_zombie(parent_pid) — non-blocking any-child reap (#389).
 *
 * Backs POSIX waitpid(-1, st, WNOHANG): find one zombie child of
 * parent_pid, release its slot (the same #378 reap subscribe_exit does
 * on an already-zombie pid) and hand back its pid + exit code.  Without
 * this a shell can never sweep its background jobs, and every "cmd &"
 * ever spawned holds a pid-table slot forever (the 256 wall the kill x
 * fork storm hit at iteration ~254).
 */
kern_return_t
proc_S_reap_zombie(
    mach_port_t   server_port,
    proc_pid_t    parent_pid,
    proc_pid_t   *pid_out,
    int          *exit_code_out,
    int          *result)
{
    int i, children = 0;

    (void)server_port;

    *pid_out       = 0;
    *exit_code_out = 0;

    pthread_mutex_lock(&pid_lock);
    for (i = 0; i < PROC_MAX_TASKS; i++) {
        struct pid_entry *e = &pid_table[i];
        mach_port_t reaped_sigport, reaped_notify;

        if (!e->in_use || e->ppid != parent_pid)
            continue;
        children++;
        if (e->state != PROC_STATE_ZOMBIE)
            continue;

        *pid_out       = e->pid;
        *exit_code_out = e->exit_code;
        /* #378-style reap: task_port was already dropped by the
         * dead-name notification; only signal_port (and a stale
         * exit_notify, defensively) may still hold rights — freed
         * below the lock. */
        reaped_sigport = e->signal_port;
        reaped_notify  = e->exit_notify;
        memset(e, 0, sizeof(*e));
        pthread_mutex_unlock(&pid_lock);

        if (reaped_sigport != MACH_PORT_NULL)
            (void)mach_port_deallocate(mach_task_self(), reaped_sigport);
        if (reaped_notify != MACH_PORT_NULL)
            (void)mach_port_deallocate(mach_task_self(), reaped_notify);
        *result = PROC_OK;
        return KERN_SUCCESS;
    }
    pthread_mutex_unlock(&pid_lock);

    *result = children ? PROC_ERR_NOT_FOUND : PROC_ERR_NO_CHILD;
    return KERN_SUCCESS;
}

kern_return_t
proc_S_list(
    mach_port_t                 server_port,
    pointer_t                   *entries,
    mach_msg_type_number_t      *entriesCnt,
    unsigned                    *count,
    int                         *result)
{
    proc_entry_t *out;
    vm_size_t alloc_sz;
    unsigned n = 0;
    int i;
    kern_return_t kr;
    vm_address_t buf = 0;

    (void)server_port;

    /* Worst case: all slots populated. */
    alloc_sz = sizeof(proc_entry_t) * PROC_MAX_TASKS;
    kr = vm_allocate(mach_task_self(), &buf, alloc_sz, TRUE);
    if (kr != KERN_SUCCESS) {
        *entries    = (pointer_t)0;
        *entriesCnt = 0;
        *count      = 0;
        *result     = PROC_ERR_KERNEL;
        return KERN_SUCCESS;
    }
    out = (proc_entry_t *)buf;

    pthread_mutex_lock(&pid_lock);
    for (i = 0; i < PROC_MAX_TASKS && n < PROC_MAX_TASKS; i++) {
        if (!pid_table[i].in_use)
            continue;
        out[n].pid       = pid_table[i].pid;
        out[n].ppid      = pid_table[i].ppid;
        out[n].state     = pid_table[i].state;
        out[n].exit_code = pid_table[i].exit_code;
        memset(out[n]._pad, 0, sizeof(out[n]._pad));
        (void)strncpy(out[n].cmdline, pid_table[i].cmdline,
                      sizeof(out[n].cmdline) - 1);
        out[n].cmdline[sizeof(out[n].cmdline) - 1] = '\0';
        n++;
    }
    pthread_mutex_unlock(&pid_lock);

    *entries    = (pointer_t)buf;
    *entriesCnt = (mach_msg_type_number_t)(sizeof(proc_entry_t) * n);
    *count      = n;
    *result     = PROC_OK;
    return KERN_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Signal delivery (v0.2.0 / #238)                                     */
/* ------------------------------------------------------------------ */

/*
 * Send proc_signal_msg_t to a signal_port.  COPY_SEND so the server
 * keeps the right for subsequent kills.  Returns the mach_msg kr —
 * caller decides whether to clear the slot on dead-name errors.
 */
static kern_return_t
send_signal_msg(mach_port_t sigport, int signo, proc_pid_t sender)
{
    proc_signal_msg_t msg;

    memset(&msg, 0, sizeof(msg));
    msg.head.msgh_bits        = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg.head.msgh_size        = sizeof(msg) - sizeof(mach_msg_trailer_t);
    msg.head.msgh_remote_port = sigport;
    msg.head.msgh_local_port  = MACH_PORT_NULL;
    msg.head.msgh_id          = PROC_SIGNAL_MSGID;
    msg.signo                 = signo;
    msg.sender_pid            = sender;

    return mach_msg(&msg.head, MACH_SEND_MSG,
                    sizeof(msg) - sizeof(mach_msg_trailer_t),
                    0, MACH_PORT_NULL,
                    MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

static int
signo_valid(int signo)
{
    return signo > 0 && signo < PROC_NSIG;
}

kern_return_t
proc_S_set_signal_port(
    mach_port_t                 server_port,
    proc_pid_t                  pid,
    mach_port_t                 signal_port,
    int                         *result)
{
    struct pid_entry *e;
    mach_port_t old = MACH_PORT_NULL;

    (void)server_port;

    pthread_mutex_lock(&pid_lock);
    e = find_by_pid_locked(pid);
    if (!e) {
        pthread_mutex_unlock(&pid_lock);
        if (signal_port != MACH_PORT_NULL)
            (void)mach_port_deallocate(mach_task_self(), signal_port);
        *result = PROC_ERR_NOT_FOUND;
        return KERN_SUCCESS;
    }
    old = e->signal_port;
    e->signal_port = signal_port;
    pthread_mutex_unlock(&pid_lock);

    if (old != MACH_PORT_NULL)
        (void)mach_port_deallocate(mach_task_self(), old);

    *result = PROC_OK;
    return KERN_SUCCESS;
}

kern_return_t
proc_S_kill(
    mach_port_t                 server_port,
    proc_pid_t                  pid,
    int                         signo,
    int                         *result)
{
    struct pid_entry *e;
    mach_port_t task = MACH_PORT_NULL;
    mach_port_t sigport = MACH_PORT_NULL;
    kern_return_t kr;

    (void)server_port;

    if (!signo_valid(signo)) {
        *result = PROC_ERR_BAD_SIGNO;
        return KERN_SUCCESS;
    }

    pthread_mutex_lock(&pid_lock);
    e = find_by_pid_locked(pid);
    if (!e || e->state == PROC_STATE_ZOMBIE) {
        pthread_mutex_unlock(&pid_lock);
        *result = PROC_ERR_NOT_FOUND;
        return KERN_SUCCESS;
    }
    /*
     * SIGCONT is special: durably continue the whole stopped process
     * group (#247), not just wake the one target.  Snapshot the tasks
     * proc_server itself suspended (stopped flag) under the lock, clear
     * the flag, then task_resume outside the lock.  SIGCONT to a group
     * with nothing stopped is a successful no-op (POSIX).
     */
    if (signo == PROC_SIGCONT) {
        mach_port_t cont_tasks[PROC_MAX_TASKS];
        unsigned n_cont = 0, j;
        proc_pid_t grp = e->pgrp_id;
        int k;

        for (k = 0; k < PROC_MAX_TASKS; k++) {
            if (pid_table[k].in_use &&
                pid_table[k].state != PROC_STATE_ZOMBIE &&
                pid_table[k].pgrp_id == grp &&
                pid_table[k].stopped) {
                cont_tasks[n_cont++] = pid_table[k].task_port;
                pid_table[k].stopped = 0;
            }
        }
        pthread_mutex_unlock(&pid_lock);

        for (j = 0; j < n_cont; j++)
            (void)task_resume(cont_tasks[j]);
        *result = PROC_OK;
        return KERN_SUCCESS;
    }

    /* Uncatchable signals act on the task port; catchable ones go to
     * the signal_port via mach_msg.  Snapshot the rights under the
     * lock, release the lock before issuing the kernel/mach call. */
    if (signo == PROC_SIGKILL || signo == PROC_SIGSTOP) {
        task = e->task_port;
    } else {
        sigport = e->signal_port;
    }
    pthread_mutex_unlock(&pid_lock);

    switch (signo) {
    case PROC_SIGKILL:
        kr = task_terminate(task);
        *result = (kr == KERN_SUCCESS) ? PROC_OK : PROC_ERR_KERNEL;
        return KERN_SUCCESS;
    case PROC_SIGSTOP:
        kr = task_suspend(task);
        if (kr == KERN_SUCCESS) {
            /* Mark the pid stopped so a later SIGCONT resumes it. */
            pthread_mutex_lock(&pid_lock);
            e = find_by_pid_locked(pid);
            if (e)
                e->stopped = 1;
            pthread_mutex_unlock(&pid_lock);
        }
        *result = (kr == KERN_SUCCESS) ? PROC_OK : PROC_ERR_KERNEL;
        return KERN_SUCCESS;
    default:
        break;
    }

    if (sigport == MACH_PORT_NULL) {
        *result = PROC_ERR_NO_SIGPORT;
        return KERN_SUCCESS;
    }

    kr = send_signal_msg(sigport, signo, PROC_PID_NONE);
    if (kr != KERN_SUCCESS) {
        /* Signal port is dead or gone — clear the slot so future kills
         * skip it.  Re-lock and clear only if no one swapped it in. */
        pthread_mutex_lock(&pid_lock);
        e = find_by_pid_locked(pid);
        if (e && e->signal_port == sigport) {
            e->signal_port = MACH_PORT_NULL;
            pthread_mutex_unlock(&pid_lock);
            (void)mach_port_deallocate(mach_task_self(), sigport);
        } else {
            pthread_mutex_unlock(&pid_lock);
        }
        *result = PROC_ERR_KERNEL;
        return KERN_SUCCESS;
    }

    *result = PROC_OK;
    return KERN_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  POSIX session + process group (v0.3.0 / #239)                      */
/* ------------------------------------------------------------------ */

/*
 * setsid: caller's pid becomes leader of a new session and a new
 * process group, both numerically equal to the pid.  POSIX forbids
 * this if the caller is already a pgrp leader (would create a
 * collision with the existing pgrp_id == pid).
 */
kern_return_t
proc_S_setsid(
    mach_port_t                 server_port,
    proc_pid_t                  pid,
    proc_pid_t                  *new_sid,
    int                         *result)
{
    struct pid_entry *e;
    int i, is_leader = 0;

    (void)server_port;
    *new_sid = 0;

    pthread_mutex_lock(&pid_lock);
    e = find_by_pid_locked(pid);
    if (!e) {
        pthread_mutex_unlock(&pid_lock);
        *result = PROC_ERR_NOT_FOUND;
        return KERN_SUCCESS;
    }
    /* POSIX: setsid() fails with EPERM if the calling pid is already
     * a pgrp leader, i.e. some entry (including itself) already has
     * pgrp_id == pid. */
    for (i = 0; i < PROC_MAX_TASKS; i++) {
        if (pid_table[i].in_use && pid_table[i].pgrp_id == pid) {
            is_leader = 1;
            break;
        }
    }
    if (is_leader) {
        pthread_mutex_unlock(&pid_lock);
        *result = PROC_ERR_PERM;
        return KERN_SUCCESS;
    }
    e->sid     = pid;
    e->pgrp_id = pid;
    /* A new session starts with no controlling terminal (#247): this is
     * what makes the fork()+setsid() daemonize idiom detach from the
     * tty.  The leader claims one explicitly via proc_set_ctty. */
    e->ctty_port  = MACH_PORT_NULL;
    e->fg_pgrp_id = 0;
    pthread_mutex_unlock(&pid_lock);

    *new_sid = pid;
    *result  = PROC_OK;
    return KERN_SUCCESS;
}

/*
 * setpgid(pid, pgrp): move pid into pgrp.  pgrp == 0 means "use pid"
 * (POSIX: create a new pgrp with id == pid).  The target pgrp, if it
 * already exists, must be in the same session as pid.
 */
kern_return_t
proc_S_setpgid(
    mach_port_t                 server_port,
    proc_pid_t                  pid,
    proc_pid_t                  pgrp,
    int                         *result)
{
    struct pid_entry *e;
    int i;
    proc_pid_t target_pgrp;
    int pgrp_exists = 0;
    proc_pid_t pgrp_sid = 0;

    (void)server_port;

    pthread_mutex_lock(&pid_lock);
    e = find_by_pid_locked(pid);
    if (!e) {
        pthread_mutex_unlock(&pid_lock);
        *result = PROC_ERR_NOT_FOUND;
        return KERN_SUCCESS;
    }
    target_pgrp = (pgrp == 0) ? pid : pgrp;

    /* If target_pgrp already labels another running pid, snapshot
     * that pid's session so we can enforce the same-session rule. */
    for (i = 0; i < PROC_MAX_TASKS; i++) {
        if (pid_table[i].in_use &&
            pid_table[i].pgrp_id == target_pgrp) {
            pgrp_exists = 1;
            pgrp_sid    = pid_table[i].sid;
            break;
        }
    }
    if (pgrp_exists && pgrp_sid != e->sid) {
        pthread_mutex_unlock(&pid_lock);
        *result = PROC_ERR_DIFF_SESS;
        return KERN_SUCCESS;
    }
    e->pgrp_id = target_pgrp;
    pthread_mutex_unlock(&pid_lock);

    *result = PROC_OK;
    return KERN_SUCCESS;
}

kern_return_t
proc_S_getsid(
    mach_port_t                 server_port,
    proc_pid_t                  pid,
    proc_pid_t                  *sid,
    int                         *result)
{
    struct pid_entry *e;
    (void)server_port;
    *sid = 0;

    pthread_mutex_lock(&pid_lock);
    e = find_by_pid_locked(pid);
    if (e)
        *sid = e->sid;
    pthread_mutex_unlock(&pid_lock);

    *result = e ? PROC_OK : PROC_ERR_NOT_FOUND;
    return KERN_SUCCESS;
}

kern_return_t
proc_S_getpgid(
    mach_port_t                 server_port,
    proc_pid_t                  pid,
    proc_pid_t                  *pgrp,
    int                         *result)
{
    struct pid_entry *e;
    (void)server_port;
    *pgrp = 0;

    pthread_mutex_lock(&pid_lock);
    e = find_by_pid_locked(pid);
    if (e)
        *pgrp = e->pgrp_id;
    pthread_mutex_unlock(&pid_lock);

    *result = e ? PROC_OK : PROC_ERR_NOT_FOUND;
    return KERN_SUCCESS;
}

/*
 * killpg: deliver signo to every running pid in pgrp.  We can't hold
 * pid_lock across task_terminate / mach_msg, so we snapshot the
 * matching ports into a local array under the lock, then dispatch
 * outside.  For catchable signals we use signal_port (COPY_SEND);
 * pids without a signal_port are skipped silently (POSIX killpg
 * doesn't require every recipient to be reachable).  n_sent counts
 * how many dispatches actually fired.
 */
kern_return_t
proc_S_killpg(
    mach_port_t                 server_port,
    proc_pid_t                  pgrp,
    int                         signo,
    unsigned                    *n_sent,
    int                         *result)
{
    /* Snapshot: per-match port + classification of the dispatch.
     * We size the array to PROC_MAX_TASKS so it always fits. */
    struct killpg_target {
        mach_port_t port;       /* task_port for uncatchable, signal_port otherwise */
        proc_pid_t  pid;        /* for cleanup-on-error of signal_port */
    };
    struct killpg_target uncatch[PROC_MAX_TASKS];
    struct killpg_target catchable[PROC_MAX_TASKS];
    unsigned n_uncatch = 0, n_catch = 0;
    unsigned i;
    unsigned sent = 0;
    int is_uncatch;

    (void)server_port;
    *n_sent = 0;

    if (!signo_valid(signo)) {
        *result = PROC_ERR_BAD_SIGNO;
        return KERN_SUCCESS;
    }
    is_uncatch = (signo == PROC_SIGKILL || signo == PROC_SIGSTOP ||
                  signo == PROC_SIGCONT);

    pthread_mutex_lock(&pid_lock);
    for (i = 0; i < PROC_MAX_TASKS; i++) {
        if (!pid_table[i].in_use) continue;
        if (pid_table[i].state == PROC_STATE_ZOMBIE) continue;
        if (pid_table[i].pgrp_id != pgrp) continue;
        if (is_uncatch) {
            uncatch[n_uncatch].port = pid_table[i].task_port;
            uncatch[n_uncatch].pid  = pid_table[i].pid;
            n_uncatch++;
        } else if (pid_table[i].signal_port != MACH_PORT_NULL) {
            catchable[n_catch].port = pid_table[i].signal_port;
            catchable[n_catch].pid  = pid_table[i].pid;
            n_catch++;
        }
    }
    pthread_mutex_unlock(&pid_lock);

    if (is_uncatch) {
        for (i = 0; i < n_uncatch; i++) {
            kern_return_t kr;
            switch (signo) {
            case PROC_SIGKILL: kr = task_terminate(uncatch[i].port); break;
            case PROC_SIGSTOP: kr = task_suspend  (uncatch[i].port); break;
            case PROC_SIGCONT: kr = task_resume   (uncatch[i].port); break;
            default:           kr = KERN_INVALID_ARGUMENT;           break;
            }
            if (kr == KERN_SUCCESS) sent++;
        }
    } else {
        for (i = 0; i < n_catch; i++) {
            kern_return_t kr = send_signal_msg(catchable[i].port, signo,
                                               PROC_PID_NONE);
            if (kr == KERN_SUCCESS) {
                sent++;
            } else {
                /* Clear dead signal_port slot, same policy as proc_kill. */
                struct pid_entry *e;
                pthread_mutex_lock(&pid_lock);
                e = find_by_pid_locked(catchable[i].pid);
                if (e && e->signal_port == catchable[i].port) {
                    e->signal_port = MACH_PORT_NULL;
                    pthread_mutex_unlock(&pid_lock);
                    (void)mach_port_deallocate(mach_task_self(),
                                               catchable[i].port);
                } else {
                    pthread_mutex_unlock(&pid_lock);
                }
            }
        }
    }

    *n_sent = sent;
    *result = (sent > 0) ? PROC_OK : PROC_ERR_NOT_FOUND;
    return KERN_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Controlling terminal / job control (v0.4.0 / #247)                  */
/* ------------------------------------------------------------------ */

/*
 * proc_set_ctty(sid, tty_port): the session leader claims tty_port as
 * the session's controlling terminal.  POSIX allows this only when the
 * session has no controlling tty yet; the foreground process group is
 * initialised to the leader's own pgrp.  Consumes the passed send right.
 */
kern_return_t
proc_S_set_ctty(
    mach_port_t                 server_port,
    proc_pid_t                  sid,
    mach_port_t                 tty_port,
    int                         *result)
{
    struct pid_entry *e;

    (void)server_port;

    pthread_mutex_lock(&pid_lock);
    e = find_session_leader_locked(sid);
    if (!e) {
        pthread_mutex_unlock(&pid_lock);
        if (tty_port != MACH_PORT_NULL)
            (void)mach_port_deallocate(mach_task_self(), tty_port);
        *result = PROC_ERR_NOT_FOUND;
        return KERN_SUCCESS;
    }
    if (e->ctty_port != MACH_PORT_NULL) {
        pthread_mutex_unlock(&pid_lock);
        if (tty_port != MACH_PORT_NULL)
            (void)mach_port_deallocate(mach_task_self(), tty_port);
        *result = PROC_ERR_PERM;        /* session already has a ctty */
        return KERN_SUCCESS;
    }
    e->ctty_port  = tty_port;
    e->fg_pgrp_id = e->pgrp_id;         /* leader's pgrp is foreground */
    pthread_mutex_unlock(&pid_lock);

    *result = PROC_OK;
    return KERN_SUCCESS;
}

/*
 * Tell the tty owner (char_server) that a session's controlling terminal
 * has been released, so it can drop the stale binding and free the VT for
 * a fresh shell (#365).  proc_server holds the send right to the tty port
 * (handed over at tty_acquire_ctty), so it is the one that knows the exact
 * moment the ctty goes away — on explicit clear or on leader death.
 * One-way, best-effort: a dead port just means char_server is gone too.
 */
static void
notify_ctty_release(mach_port_t ctty, proc_pid_t sid)
{
    char_ctty_release_msg_t m;

    if (ctty == MACH_PORT_NULL)
        return;
    memset(&m, 0, sizeof m);
    m.head.msgh_bits        = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    m.head.msgh_size        = sizeof m;
    m.head.msgh_remote_port = ctty;
    m.head.msgh_local_port  = MACH_PORT_NULL;
    m.head.msgh_id          = CHAR_CTTY_RELEASE_MSGH_ID;
    m.sid                   = (int32_t)sid;
    (void)mach_msg(&m.head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m,
                   0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
}

/*
 * proc_clear_ctty(sid): release the session's controlling terminal.
 * Idempotent — clearing a session that has none simply succeeds.
 */
kern_return_t
proc_S_clear_ctty(
    mach_port_t                 server_port,
    proc_pid_t                  sid,
    int                         *result)
{
    struct pid_entry *e;
    mach_port_t old = MACH_PORT_NULL;

    (void)server_port;

    pthread_mutex_lock(&pid_lock);
    e = find_session_leader_locked(sid);
    if (!e) {
        pthread_mutex_unlock(&pid_lock);
        *result = PROC_ERR_NOT_FOUND;
        return KERN_SUCCESS;
    }
    old           = e->ctty_port;
    e->ctty_port  = MACH_PORT_NULL;
    e->fg_pgrp_id = 0;
    pthread_mutex_unlock(&pid_lock);

    if (old != MACH_PORT_NULL) {
        notify_ctty_release(old, sid);
        (void)mach_port_deallocate(mach_task_self(), old);
    }

    *result = PROC_OK;
    return KERN_SUCCESS;
}

/*
 * proc_tcsetpgrp(sid, pgrp): set the foreground process group of the
 * session's controlling tty.  PROC_ERR_NO_CTTY if the session has none.
 */
kern_return_t
proc_S_tcsetpgrp(
    mach_port_t                 server_port,
    proc_pid_t                  sid,
    proc_pid_t                  pgrp,
    int                         *result)
{
    struct pid_entry *e;

    (void)server_port;

    pthread_mutex_lock(&pid_lock);
    e = find_session_leader_locked(sid);
    if (!e) {
        pthread_mutex_unlock(&pid_lock);
        *result = PROC_ERR_NOT_FOUND;
        return KERN_SUCCESS;
    }
    if (e->ctty_port == MACH_PORT_NULL) {
        pthread_mutex_unlock(&pid_lock);
        *result = PROC_ERR_NO_CTTY;
        return KERN_SUCCESS;
    }
    e->fg_pgrp_id = pgrp;
    pthread_mutex_unlock(&pid_lock);

    *result = PROC_OK;
    return KERN_SUCCESS;
}

/*
 * proc_tcgetpgrp(sid): read the foreground process group.
 * PROC_ERR_NO_CTTY if the session has no controlling tty.
 */
kern_return_t
proc_S_tcgetpgrp(
    mach_port_t                 server_port,
    proc_pid_t                  sid,
    proc_pid_t                  *pgrp,
    int                         *result)
{
    struct pid_entry *e;

    (void)server_port;
    *pgrp = 0;

    pthread_mutex_lock(&pid_lock);
    e = find_session_leader_locked(sid);
    if (!e) {
        pthread_mutex_unlock(&pid_lock);
        *result = PROC_ERR_NOT_FOUND;
        return KERN_SUCCESS;
    }
    if (e->ctty_port == MACH_PORT_NULL) {
        pthread_mutex_unlock(&pid_lock);
        *result = PROC_ERR_NO_CTTY;
        return KERN_SUCCESS;
    }
    *pgrp = e->fg_pgrp_id;
    pthread_mutex_unlock(&pid_lock);

    *result = PROC_OK;
    return KERN_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* proc_S_shutdown — bring the system down via host_reboot (#281).    */
/* ------------------------------------------------------------------ */

/* SIGTERM grace window before we escalate to task_terminate (#284). */
#define PROC_DRAIN_GRACE_US     800000      /* 0.8 s */

/*
 * #284 phase 1 — SIGTERM sweep.  Send PROC_SIGTERM to every userland
 * app, give them a short grace window to flush and exit, then SIGKILL
 * (task_terminate) anything still alive.  We target only pids that own
 * a signal_port: those are the userland apps (ush and its children).
 * The core servers — ext_server, name_server, block_device, char — set
 * no signal_port, so the sweep never touches them, which is exactly what
 * we want: ext_server must stay alive for the fs sync in phase 2.
 */
static void
proc_drain_sigterm_sweep(void)
{
    struct { mach_port_t sigport; proc_pid_t pid; } term[PROC_MAX_TASKS];
    unsigned n_term = 0, n_kill = 0, i;
    int k;

    pthread_mutex_lock(&pid_lock);
    for (k = 0; k < PROC_MAX_TASKS; k++) {
        struct pid_entry *e = &pid_table[k];
        if (e->in_use && e->state != PROC_STATE_ZOMBIE &&
            e->signal_port != MACH_PORT_NULL) {
            term[n_term].sigport = e->signal_port;
            term[n_term].pid     = e->pid;
            n_term++;
        }
    }
    pthread_mutex_unlock(&pid_lock);

    if (n_term == 0)
        return;

    printf("proc: shutdown — SIGTERM to %u task(s)\n", n_term);
    for (i = 0; i < n_term; i++)
        (void)send_signal_msg(term[i].sigport, PROC_SIGTERM, PROC_PID_NONE);

    usleep(PROC_DRAIN_GRACE_US);

    /*
     * #379: count whoever ignored SIGTERM, but do NOT task_terminate()
     * them here.  On SMP, terminating a task whose thread is still active
     * on another CPU can hang task_terminate() intermittently, wedging the
     * whole shutdown before the fs sync + host_reboot below (the log then
     * stops dead at this line).  We don't need to force-kill them: the
     * host_reboot in phase 3 resets the machine and wipes them, and the fs
     * sync (phase 2) needs only the fs servers — which own no signal_port
     * and are never in this sweep.  A survivor that ignored SIGTERM loses
     * its unflushed buffers whether we kill it or reset it, so integrity is
     * unchanged.  Leaving them alone keeps shutdown bounded — the original
     * intent of the force-kill, minus the hang.
     */
    pthread_mutex_lock(&pid_lock);
    for (i = 0; i < n_term; i++) {
        struct pid_entry *e = find_by_pid_locked(term[i].pid);
        if (e && e->state != PROC_STATE_ZOMBIE)
            n_kill++;
    }
    pthread_mutex_unlock(&pid_lock);

    if (n_kill)
        printf("proc: shutdown — %u task(s) unresponsive to SIGTERM; "
               "leaving them for host_reboot (#379)\n", n_kill);
}

/*
 * #284 phase 2 — flush every mounted filesystem.  Walk the mount
 * registry (netname_list_mounts), then fs_sync each mount's fs_port.
 * fs_sync is fs-wide ("flush all dirty cached state to the device") and
 * a no-op on read-only servers, so this is safe for every mount.  We
 * skip our own /proc mount — it is virtual and an RPC to ourselves from
 * this server thread would deadlock.  Failures are logged, never fatal.
 */
static void
proc_drain_fs_sync(void)
{
    netname_path_t prefixes;
    int count = 0;
    kern_return_t kr;
    char *line;

    kr = netname_list_mounts(name_server_port, prefixes, &count);
    if (kr != NETNAME_SUCCESS) {
        printf("proc: shutdown — list_mounts failed kr=%d (skip sync)\n", kr);
        return;
    }
    printf("proc: shutdown — syncing %d mount(s)\n", count);

    /* prefixes is a newline-separated list; walk it in place. */
    line = prefixes;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';

        if (*line && strcmp(line, PROC_MOUNT_PATH) != 0) {
            mach_port_t fs_port = MACH_PORT_NULL;
            netname_name_t matched;

            kr = netname_look_up_mount(name_server_port, line,
                                       &fs_port, matched);
            if (kr == NETNAME_SUCCESS && fs_port != MACH_PORT_NULL) {
                kern_return_t sk = fs_unmount(fs_port);
                printf("proc: shutdown — unmount %s %s\n", line,
                       (sk == KERN_SUCCESS) ? "ok" : "FAILED");
                (void)mach_port_deallocate(mach_task_self(), fs_port);
            }
        }

        line = nl ? nl + 1 : NULL;
    }
}

/*
 * Graceful drain (#284) then host_reboot.  The drain SIGTERMs userland
 * apps (grace + SIGKILL fallback) and fs_syncs every mount so no dirty
 * page is lost across the reset.  On success host_reboot does not return:
 * it halts/resets the machine before the reply marshalls, so any reply
 * the caller sees is a failure.
 */
kern_return_t
proc_S_shutdown(
    mach_port_t                 server_port,
    int                         mode,
    int                         *result)
{
    int options;
    kern_return_t kr;

    (void)server_port;

    switch (mode) {
    case PROC_SHUTDOWN_HALT:
        options = HOST_REBOOT_HALT;
        printf("proc: shutdown — HALT (draining first)\n");
        break;
    case PROC_SHUTDOWN_REBOOT:
        options = 0;
        printf("proc: shutdown — REBOOT (draining first)\n");
        break;
    default:
        *result = PROC_ERR_INVAL;
        return KERN_SUCCESS;
    }

    proc_drain_sigterm_sweep();
    proc_drain_fs_sync();

    printf("proc: shutdown — host_reboot(0x%x)\n", options);
    kr = host_reboot(host_port, options);
    /* host_reboot does not return on success.  Report failure if it
     * somehow does. */
    *result = (kr == KERN_SUCCESS) ? PROC_OK : PROC_ERR_KERNEL;
    return KERN_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Resource accounting (v0.4.0 / #240)                                */
/* ------------------------------------------------------------------ */

/*
 * Query the kernel for the rusage of `task_port` and fill `out`.
 * Combines TASK_BASIC_INFO (times + sizes + suspend count) with
 * TASK_EVENTS_INFO (faults + Mach msg counters).  BASIC carries the
 * load-bearing fields; EVENTS is best-effort (the kernel can return
 * KERN_FAILURE for it without breaking the snapshot), so we only
 * propagate failure when BASIC itself fails.
 */
static kern_return_t
query_kernel_rusage(mach_port_t task_port, proc_rusage_t *out)
{
    task_basic_info_data_t  basic;
    task_events_info_data_t events;
    mach_msg_type_number_t  cnt;
    kern_return_t           kr_basic, kr_events;

    memset(out, 0, sizeof(*out));

    cnt = TASK_BASIC_INFO_COUNT;
    kr_basic = task_info(task_port, TASK_BASIC_INFO,
                         (task_info_t)&basic, &cnt);
    if (kr_basic != KERN_SUCCESS)
        return kr_basic;

    out->utime_sec   = (uint32_t)basic.user_time.seconds;
    out->utime_usec  = (uint32_t)basic.user_time.microseconds;
    out->stime_sec   = (uint32_t)basic.system_time.seconds;
    out->stime_usec  = (uint32_t)basic.system_time.microseconds;
    out->virtual_kb  = (uint32_t)(basic.virtual_size  / 1024);
    out->maxrss_kb   = (uint32_t)(basic.resident_size / 1024);

    cnt = TASK_EVENTS_INFO_COUNT;
    kr_events = task_info(task_port, TASK_EVENTS_INFO,
                          (task_info_t)&events, &cnt);
    if (kr_events == KERN_SUCCESS) {
        /* Linux ru_minflt = zero_fill + COW (no I/O); ru_majflt = pageins. */
        out->minflt = (uint32_t)(events.zero_fills + events.cow_faults);
        out->majflt = (uint32_t)events.pageins;
        out->msgsnd = (uint32_t)events.messages_sent;
        out->msgrcv = (uint32_t)events.messages_received;
    }
    /* EVENTS failure leaves those fields at zero — still a valid
     * snapshot of times + memory sizes from BASIC. */
    return KERN_SUCCESS;
}

/*
 * Refresh the cached rusage for `pid` from the kernel.  Zombies
 * skip the refresh (the kernel task is gone) but the cached
 * snapshot remains valid.  Returns KERN_SUCCESS even when we
 * couldn't refresh — callers fall back to whatever's in the cache.
 */
static void
refresh_rusage_for_pid(proc_pid_t pid)
{
    struct pid_entry *e;
    mach_port_t task = MACH_PORT_NULL;
    int is_zombie = 0;
    proc_rusage_t fresh;

    pthread_mutex_lock(&pid_lock);
    e = find_by_pid_locked(pid);
    if (e) {
        task = e->task_port;
        is_zombie = (e->state == PROC_STATE_ZOMBIE);
    }
    pthread_mutex_unlock(&pid_lock);

    if (!e || is_zombie || task == MACH_PORT_NULL)
        return;

    if (query_kernel_rusage(task, &fresh) != KERN_SUCCESS)
        return;

    pthread_mutex_lock(&pid_lock);
    /* Re-find: the slot is never recycled in v0.x.0, so the pointer
     * stays valid, but the state may have flipped to ZOMBIE while we
     * were unlocked.  In that case the kernel rusage we just got is
     * still the most recent snapshot, so commit it anyway. */
    e = find_by_pid_locked(pid);
    if (e)
        e->last_rusage = fresh;
    pthread_mutex_unlock(&pid_lock);
}

kern_return_t
proc_S_getrusage(
    mach_port_t                 server_port,
    proc_pid_t                  pid,
    proc_rusage_t               *usage,
    int                         *result)
{
    struct pid_entry *e;

    (void)server_port;
    memset(usage, 0, sizeof(*usage));

    /* Best-effort refresh from the kernel before snapshotting the
     * cache — keeps the returned values as close to "now" as a
     * single RPC can manage. */
    refresh_rusage_for_pid(pid);

    pthread_mutex_lock(&pid_lock);
    e = find_by_pid_locked(pid);
    if (e)
        *usage = e->last_rusage;
    pthread_mutex_unlock(&pid_lock);

    *result = e ? PROC_OK : PROC_ERR_NOT_FOUND;
    return KERN_SUCCESS;
}

/*
 * proc_set_exit_code(pid, code) — Phase 5b (#255) hook for the
 * libposix-uros SYS_exit handler.  Stash the value so the impending
 * dead-name notification's proc_exit_msg_t carries the real exit code
 * instead of the default 0.  Last writer wins; safe if the pid is
 * already gone (no-op + PROC_ERR_NOT_FOUND).
 */
kern_return_t
proc_S_set_exit_code(
    mach_port_t                 server_port,
    proc_pid_t                  pid,
    int                         code,
    int                         *result)
{
    struct pid_entry *e;

    (void)server_port;

    pthread_mutex_lock(&pid_lock);
    e = find_by_pid_locked(pid);
    if (e)
        e->exit_code = code;
    pthread_mutex_unlock(&pid_lock);

    *result = e ? PROC_OK : PROC_ERR_NOT_FOUND;
    return KERN_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  vfs.defs adapter for /proc/N/stat                                   */
/* ------------------------------------------------------------------ */

/*
 * Parse a path of the form "/proc/<pid>/stat" or "/<pid>/stat" (the
 * latter when libvfs has stripped the matched mount prefix).  Sets
 * *out_pid and returns the trailing component (e.g. "stat") or NULL
 * if the path is malformed.
 */
static const char *
parse_proc_path(const char *path, proc_pid_t *out_pid)
{
    const char *p = path;
    proc_pid_t pid = 0;

    if (!p)
        return NULL;
    if (*p == '/')
        p++;
    /* Skip optional leading "proc/" — depending on whether libvfs
     * strips the mount prefix.  Both forms are accepted. */
    if (strncmp(p, "proc/", 5) == 0)
        p += 5;
    if (*p < '0' || *p > '9')
        return NULL;
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10 + (proc_pid_t)(*p - '0');
        p++;
    }
    if (*p == '\0') {
        *out_pid = pid;
        return "";
    }
    if (*p != '/')
        return NULL;
    p++;
    *out_pid = pid;
    return p;
}

kern_return_t
vfs_open(
    mach_port_t fs_port,
    mach_port_t client_task,
    char *path,
    int flags,
    int mode,
    vfs_u64_t *handle_out,
    vfs_u32_t *type_out)
{
    proc_pid_t pid = 0;
    const char *tail;
    struct pid_entry *e;

    (void)fs_port; (void)flags; (void)mode;
    /* #385: /proc handles are virtual (no fs_server fid pool), so we don't
     * need the client-task reclaim hook — just drop the send right. */
    (void)mach_port_deallocate(mach_task_self(), client_task);

    *handle_out = 0;
    *type_out   = VFS_FT_UNKNOWN;

    tail = parse_proc_path(path, &pid);
    if (!tail || strcmp(tail, "stat") != 0)
        return KERN_FAILURE;     /* v0.1.0: only /proc/<pid>/stat */

    pthread_mutex_lock(&pid_lock);
    e = find_by_pid_locked(pid);
    if (!e || e->stat_handle_in_use) {
        pthread_mutex_unlock(&pid_lock);
        return KERN_FAILURE;
    }
    e->stat_handle_in_use = 1;
    e->stat_handle_id     = next_stat_handle++;
    e->stat_offset        = 0;
    *handle_out = e->stat_handle_id;
    *type_out   = VFS_FT_REG;
    pthread_mutex_unlock(&pid_lock);
    return KERN_SUCCESS;
}

static struct pid_entry *
find_by_handle_locked(vfs_u64_t handle)
{
    int i;
    for (i = 0; i < PROC_MAX_TASKS; i++)
        if (pid_table[i].in_use && pid_table[i].stat_handle_in_use
            && pid_table[i].stat_handle_id == handle)
            return &pid_table[i];
    return NULL;
}

kern_return_t
vfs_close(mach_port_t fs_port, vfs_u64_t handle)
{
    struct pid_entry *e;
    (void)fs_port;

    pthread_mutex_lock(&pid_lock);
    e = find_by_handle_locked(handle);
    if (e) {
        e->stat_handle_in_use = 0;
        e->stat_handle_id     = 0;
    }
    pthread_mutex_unlock(&pid_lock);
    return e ? KERN_SUCCESS : KERN_INVALID_ARGUMENT;
}

/*
 * Format /proc/N/stat line — Linux-compatible field ordering.
 * Fields filled:
 *   1 pid       2 (cmdline)    3 state     4 ppid
 *   5 pgrp      6 session      7 tty_nr    8 tpgid
 *   9 flags    10 minflt      11 cminflt  12 majflt
 *  13 cmajflt  14 utime       15 stime    16 cutime
 *  17 cstime   ... (priority/nice/threads not tracked yet)
 *  22 starttime  23 vsize     24 rss
 *
 * utime/stime are expressed in clock ticks; we report seconds * 100
 * (i.e. assume HZ == 100) since the kernel hands us time_value_t and
 * Linux convention for /proc is "USER_HZ ticks".  rss is reported in
 * pages (PAGE_SIZE bytes), so we divide bytes by 4096.  tty_nr stays
 * 0 until #247 wires controlling-tty.
 */
static int
format_stat(const struct pid_entry *e, char *buf, int max)
{
    int n;
    char ch[2] = { (char)e->state, '\0' };
    const proc_rusage_t *r = &e->last_rusage;
    uint32_t utime_ticks = r->utime_sec * 100u + r->utime_usec / 10000u;
    uint32_t stime_ticks = r->stime_sec * 100u + r->stime_usec / 10000u;
    /* virtual_kb / maxrss_kb are KiB; Linux vsize is bytes, rss is pages. */
    uint32_t vsize_bytes = r->virtual_kb * 1024u;
    uint32_t rss_pages   = r->maxrss_kb / 4u;  /* KiB / 4 == pages (PAGE_SIZE=4096) */
    uint32_t minflt = r->minflt;
    uint32_t majflt = r->majflt;

    n = snprintf(buf, max,
                 "%u (%s) %s %u %u %u 0 -1 0 %u 0 %u 0 %u %u 0 0 0 0 0 0 0 %u %u 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                 (unsigned)e->pid, e->cmdline, ch,
                 (unsigned)e->ppid,
                 (unsigned)e->pgrp_id,
                 (unsigned)e->sid,
                 (unsigned)minflt,
                 (unsigned)majflt,
                 (unsigned)utime_ticks,
                 (unsigned)stime_ticks,
                 (unsigned)vsize_bytes,
                 (unsigned)rss_pages);
    if (n < 0)
        n = 0;
    if (n >= max)
        n = max - 1;
    return n;
}

kern_return_t
vfs_read(
    mach_port_t                fs_port,
    vfs_u64_t                  handle,
    vfs_u64_t                  offset,
    vfs_u32_t                  count,
    pointer_t                  *data_out,
    mach_msg_type_number_t     *data_count_out)
{
    struct pid_entry *e;
    char tmp[512];
    int len;
    vm_address_t buf = 0;
    kern_return_t kr;
    vm_size_t to_copy;

    (void)fs_port;

    *data_out       = (pointer_t)0;
    *data_count_out = 0;

    /* Snapshot pid under lock, refresh rusage outside lock, then
     * format from the (potentially-updated) cache. */
    {
        proc_pid_t target_pid = 0;
        pthread_mutex_lock(&pid_lock);
        e = find_by_handle_locked(handle);
        if (e) target_pid = e->pid;
        pthread_mutex_unlock(&pid_lock);
        if (target_pid != 0)
            refresh_rusage_for_pid(target_pid);
    }

    pthread_mutex_lock(&pid_lock);
    e = find_by_handle_locked(handle);
    if (!e) {
        pthread_mutex_unlock(&pid_lock);
        return KERN_INVALID_ARGUMENT;
    }
    len = format_stat(e, tmp, sizeof(tmp));
    pthread_mutex_unlock(&pid_lock);

    if (offset >= (vfs_u64_t)len)
        return KERN_SUCCESS;     /* EOF, return 0 bytes */

    to_copy = (vm_size_t)len - (vm_size_t)offset;
    if (to_copy > count)
        to_copy = count;

    kr = vm_allocate(mach_task_self(), &buf, to_copy, TRUE);
    if (kr != KERN_SUCCESS)
        return kr;
    memcpy((void *)buf, tmp + offset, to_copy);
    *data_out       = (pointer_t)buf;
    *data_count_out = (mach_msg_type_number_t)to_copy;
    return KERN_SUCCESS;
}

kern_return_t
vfs_fstat(mach_port_t fs_port, vfs_u64_t handle, vfs_stat_t *st)
{
    struct pid_entry *e;
    char tmp[512];
    int len;
    proc_pid_t target_pid = 0;
    (void)fs_port;

    pthread_mutex_lock(&pid_lock);
    e = find_by_handle_locked(handle);
    if (e) target_pid = e->pid;
    pthread_mutex_unlock(&pid_lock);
    if (target_pid != 0)
        refresh_rusage_for_pid(target_pid);

    pthread_mutex_lock(&pid_lock);
    e = find_by_handle_locked(handle);
    if (!e) {
        pthread_mutex_unlock(&pid_lock);
        return KERN_INVALID_ARGUMENT;
    }
    len = format_stat(e, tmp, sizeof(tmp));
    pthread_mutex_unlock(&pid_lock);

    memset(st, 0, sizeof(*st));
    st->st_size    = (vfs_u64_t)len;
    st->st_blksize = 4096;
    st->st_nlink   = 1;
    st->st_mode    = 0444;
    st->st_type    = VFS_FT_REG;
    return KERN_SUCCESS;
}

kern_return_t
vfs_stat(mach_port_t fs_port, char *path, vfs_stat_t *st)
{
    proc_pid_t pid = 0;
    const char *tail;
    struct pid_entry *e;
    char tmp[512];
    int len;
    (void)fs_port;

    tail = parse_proc_path(path, &pid);
    if (!tail || strcmp(tail, "stat") != 0)
        return KERN_FAILURE;
    refresh_rusage_for_pid(pid);
    pthread_mutex_lock(&pid_lock);
    e = find_by_pid_locked(pid);
    if (!e) {
        pthread_mutex_unlock(&pid_lock);
        return KERN_FAILURE;
    }
    len = format_stat(e, tmp, sizeof(tmp));
    pthread_mutex_unlock(&pid_lock);

    memset(st, 0, sizeof(*st));
    st->st_size    = (vfs_u64_t)len;
    st->st_blksize = 4096;
    st->st_nlink   = 1;
    st->st_mode    = 0444;
    st->st_type    = VFS_FT_REG;
    return KERN_SUCCESS;
}

/* Stub the rest of vfs.defs — proc_server is a read-only synthetic. */

kern_return_t vfs_write(mach_port_t fp, vfs_u64_t h, vfs_u64_t o,
                        pointer_t d, mach_msg_type_number_t dc,
                        vfs_u32_t *w)
{ (void)fp;(void)h;(void)o;(void)d;(void)dc; *w=0; return KERN_FAILURE; }

kern_return_t vfs_truncate(mach_port_t fp, vfs_u64_t h, vfs_u64_t l)
{ (void)fp;(void)h;(void)l; return KERN_FAILURE; }

kern_return_t vfs_readdir(mach_port_t fp, vfs_u64_t h, vfs_u64_t c,
                          pointer_t *e, mach_msg_type_number_t *ec,
                          vfs_u64_t *nc)
{ (void)fp;(void)h;(void)c; *e=(pointer_t)0; *ec=0; *nc=0;
  return KERN_FAILURE; }

kern_return_t vfs_unlink(mach_port_t fp, char *p) { (void)fp;(void)p;
  return KERN_FAILURE; }
kern_return_t vfs_mkdir(mach_port_t fp, char *p, int m){ (void)fp;(void)p;
  (void)m; return KERN_FAILURE; }
kern_return_t vfs_rmdir(mach_port_t fp, char *p) { (void)fp;(void)p;
  return KERN_FAILURE; }
kern_return_t vfs_rename(mach_port_t fp, char *o, char *n)
{ (void)fp;(void)o;(void)n; return KERN_FAILURE; }
kern_return_t vfs_sync(mach_port_t fp) { (void)fp; return KERN_SUCCESS; }
kern_return_t vfs_unmount(mach_port_t fp) { (void)fp; return KERN_SUCCESS; }

/* /proc files are synthetic — no mmap path.  Stub introduced when
 * fs_mmap landed in vfs.defs via #276; proc_server otherwise fails to
 * link.  Returns KERN_FAILURE → libposix-uros maps to EINVAL. */
kern_return_t vfs_mmap(mach_port_t fp, vfs_u64_t h, vfs_u32_t prot,
                       vfs_u32_t flags, mach_port_t *mem_obj)
{ (void)fp;(void)h;(void)prot;(void)flags;
  *mem_obj = MACH_PORT_NULL; return KERN_FAILURE; }

/* /proc is a synthetic fs with no bulk data path — no FLIPC fast-path
 * (#232).  Report unavailable so libvfs stays on the Mach data path. */
kern_return_t
vfs_flipc_endpoint(mach_port_t fp, vfs_path_t endpoint, int *result)
{
	(void)fp;
	endpoint[0] = '\0';
	*result = -1;
	return KERN_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  notify.defs handler — DEAD_NAME on a registered task               */
/* ------------------------------------------------------------------ */

kern_return_t
do_mach_notify_dead_name(mach_port_t notify, mach_port_t name)
{
    struct pid_entry *e, *parent;
    mach_port_t subscriber = MACH_PORT_NULL;
    mach_port_t parent_sigport = MACH_PORT_NULL;
    mach_port_t ctty = MACH_PORT_NULL;
    mach_port_t dead_sigport = MACH_PORT_NULL;
    proc_pid_t pid = 0;
    proc_pid_t ppid = 0;
    int32_t code = 0;

    (void)notify;

    pthread_mutex_lock(&pid_lock);
    e = find_by_task_locked(name);
    if (e) {
        e->state      = PROC_STATE_ZOMBIE;
        /* exit_code was set by proc_S_set_exit_code (#255 / Phase 5b)
         * just before the dying task issued task_terminate; leave it
         * alone here — overwriting with 0 throws away the value
         * waitpid is about to read. */
        subscriber    = e->exit_notify;
        e->exit_notify = MACH_PORT_NULL;
        pid           = e->pid;
        ppid          = e->ppid;
        code          = e->exit_code;
        /* If a session leader dies, release its controlling tty so the
         * send right isn't leaked (#247). */
        if (e->ctty_port != MACH_PORT_NULL) {
            ctty          = e->ctty_port;
            e->ctty_port  = MACH_PORT_NULL;
            e->fg_pgrp_id = 0;
        }
        printf("proc: pid=%u zombied (task=0x%x)\n",
               pid, (unsigned)name);
        /* SIGCHLD wiring (v0.2.0 / #238): if the parent has a signal
         * port registered, snapshot it under the lock — the COPY_SEND
         * happens later, after we drop the lock. */
        if (ppid != PROC_PID_NONE) {
            parent = find_by_pid_locked(ppid);
            if (parent && parent->signal_port != MACH_PORT_NULL)
                parent_sigport = parent->signal_port;
        }
        /*
         * #378: a waiter already blocked on this pid reaps it the moment
         * we fire_exit_notify below, so release its pid-table slot now —
         * otherwise the fixed-size table leaks one entry per process.  With
         * no waiter we keep the zombie for a later waitpid, which reaps and
         * frees it through proc_S_subscribe_exit's already-zombie path.
         * (Unwaited zombies — background jobs, orphans — still linger until
         * a reaper exists; tracked separately.)  signal_port is freed below
         * the lock; everything else is already snapshotted or cleared.
         */
        if (subscriber != MACH_PORT_NULL) {
            dead_sigport = e->signal_port;
            memset(e, 0, sizeof(*e));
        }
    }
    pthread_mutex_unlock(&pid_lock);

    /* Drop the dead-name reference the kernel handed us. */
    (void)mach_port_deallocate(mach_task_self(), name);
    if (dead_sigport != MACH_PORT_NULL)
        (void)mach_port_deallocate(mach_task_self(), dead_sigport);

    /* Release a dead session leader's controlling-tty send right, and tell
     * the tty owner first so it frees the VT (leader → sid == pid). */
    if (ctty != MACH_PORT_NULL) {
        notify_ctty_release(ctty, pid);
        (void)mach_port_deallocate(mach_task_self(), ctty);
    }

    if (subscriber != MACH_PORT_NULL)
        fire_exit_notify(subscriber, pid, code);

    /* Deliver SIGCHLD to parent.  sender_pid carries the dead child's
     * pid so the parent can waitpid() it.  Best-effort — if the port
     * is dead, proc_kill will clean it up the next time it's used. */
    if (parent_sigport != MACH_PORT_NULL)
        (void)send_signal_msg(parent_sigport, PROC_SIGCHLD, pid);

    return KERN_SUCCESS;
}

kern_return_t do_mach_notify_no_senders(mach_port_t n, mach_port_mscount_t c)
{ (void)n; (void)c; return KERN_SUCCESS; }
kern_return_t do_mach_notify_port_destroyed(mach_port_t n, mach_port_t p)
{ (void)n; (void)p; return KERN_SUCCESS; }
kern_return_t do_mach_notify_port_deleted(mach_port_t n, mach_port_t p)
{ (void)n; (void)p; return KERN_SUCCESS; }
kern_return_t do_mach_notify_send_once(mach_port_t n)
{ (void)n; return KERN_SUCCESS; }

/* ------------------------------------------------------------------ */
/*  Multi-subsystem demux                                              */
/* ------------------------------------------------------------------ */

static boolean_t
proc_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
    if (proc_server(in, out))
        return TRUE;
    if (vfs_server(in, out))
        return TRUE;
    if (notify_server(in, out))
        return TRUE;
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  Bring-up                                                           */
/* ------------------------------------------------------------------ */

static int
proc_bringup_fatal(kern_return_t kr)
{
    printf("proc: bring-up failed kr=%d\n", kr);
    return 1;
}

int
main(int argc, char **argv)
{
    kern_return_t kr;
    (void)argc; (void)argv;

    kr = bootstrap_ports(bootstrap_port,
                         &host_port, &device_port,
                         &root_ledger_wired, &root_ledger_paged,
                         &security_port);
    if (kr != KERN_SUCCESS)
        return 1;

    printf_init(device_port);
    panic_init(host_port);

    printf("\n=== proc_server " PROC_SERVER_VERSION_STRING " ===\n");

    /* Three receive ports + one shared port set. */
    kr = mach_port_allocate(mach_task_self(),
                            MACH_PORT_RIGHT_RECEIVE, &proc_port);
    if (kr)
        return proc_bringup_fatal(kr);
    kr = mach_port_allocate(mach_task_self(),
                            MACH_PORT_RIGHT_RECEIVE, &mount_port);
    if (kr)
        return proc_bringup_fatal(kr);
    kr = mach_port_allocate(mach_task_self(),
                            MACH_PORT_RIGHT_RECEIVE, &notify_port);
    if (kr)
        return proc_bringup_fatal(kr);
    kr = mach_port_allocate(mach_task_self(),
                            MACH_PORT_RIGHT_PORT_SET, &port_set);
    if (kr)
        return proc_bringup_fatal(kr);

    (void)mach_port_move_member(mach_task_self(), proc_port,   port_set);
    (void)mach_port_move_member(mach_task_self(), mount_port,  port_set);
    (void)mach_port_move_member(mach_task_self(), notify_port, port_set);

    /* Send rights for proc_port and mount_port (need to register
     * with name_server). */
    (void)mach_port_insert_right(mach_task_self(), proc_port, proc_port,
                                 MACH_MSG_TYPE_MAKE_SEND);
    (void)mach_port_insert_right(mach_task_self(), mount_port, mount_port,
                                 MACH_MSG_TYPE_MAKE_SEND);

    kr = netname_check_in(name_server_port, (char *)"proc_server",
                          MACH_PORT_NULL, proc_port);
    if (kr != NETNAME_SUCCESS) {
        printf("proc: netname_check_in failed kr=%d\n", kr);
        return proc_bringup_fatal(kr);
    }

    /* Register /proc as a vfs mount point — libvfs in any task will
     * route open("/proc/...") to mount_port. */
    kr = netname_check_in_mount(name_server_port,
                                (char *)PROC_MOUNT_PATH,
                                MACH_PORT_NULL, mount_port);
    if (kr != NETNAME_SUCCESS) {
        printf("proc: netname_check_in_mount(/proc) failed kr=%d\n",
               kr);
        /* Not fatal — proc.defs RPCs still work, /proc just won't
         * be reachable through libvfs. */
    } else {
        printf("proc: mount registered at \"%s\"\n", PROC_MOUNT_PATH);
    }

    bootstrap_completed(bootstrap_port, mach_task_self());
    printf("proc: ready, entering message loop\n");

    mach_msg_server(proc_demux, 8192, port_set, MACH_MSG_OPTION_NONE);

    printf("proc: mach_msg_server exited unexpectedly\n");
    return 1;
}
