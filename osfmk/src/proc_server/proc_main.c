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
#include <sa_mach.h>
#include <pthread.h>
#include <servers/netname.h>
#include <servers/netname_defs.h>
#include <vfs_types.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* MIG-generated headers — proc + vfs + notify. */
#include "proc_server.h"
#include "vfs_server.h"
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
    mach_port_t     task_port;       /* receive-side perspective */
    char            cmdline[PROC_CMDLINE_MAX];
    uint8_t         state;           /* PROC_STATE_* */
    int32_t         exit_code;

    /* Single subscriber for v0.1.0; v0.x.0 widens to a list. */
    mach_port_t     exit_notify;     /* send-once right, MACH_PORT_NULL if none */

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
    e->task_port  = task_port;
    e->state      = PROC_STATE_RUNNING;
    e->exit_code  = 0;
    e->exit_notify = MACH_PORT_NULL;
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

    if (fire_now)
        fire_exit_notify(notify, pid, code);

    *result = PROC_OK;
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
 * Format /proc/N/stat line — Linux-compatible-ish for v0.1.0.
 * Fields we fill (rest are placeholder zero):
 *   pid (cmdline) state ppid 0 0 0 -1 0 0 0 0 0 0 0 0 ...
 */
static int
format_stat(const struct pid_entry *e, char *buf, int max)
{
    int n;
    char ch[2] = { (char)e->state, '\0' };
    n = snprintf(buf, max,
                 "%u (%s) %s %u 0 0 0 -1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                 (unsigned)e->pid, e->cmdline, ch, (unsigned)e->ppid);
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
    (void)fs_port;

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

/* ------------------------------------------------------------------ */
/*  notify.defs handler — DEAD_NAME on a registered task               */
/* ------------------------------------------------------------------ */

kern_return_t
do_mach_notify_dead_name(mach_port_t notify, mach_port_t name)
{
    struct pid_entry *e;
    mach_port_t subscriber = MACH_PORT_NULL;
    proc_pid_t pid = 0;
    int32_t code = 0;

    (void)notify;

    pthread_mutex_lock(&pid_lock);
    e = find_by_task_locked(name);
    if (e) {
        e->state      = PROC_STATE_ZOMBIE;
        e->exit_code  = 0;     /* v0.1.0: no exit-code wire-up yet */
        subscriber    = e->exit_notify;
        e->exit_notify = MACH_PORT_NULL;
        pid           = e->pid;
        code          = e->exit_code;
        printf("proc: pid=%u zombied (task=0x%x)\n",
               pid, (unsigned)name);
    }
    pthread_mutex_unlock(&pid_lock);

    /* Drop the dead-name reference the kernel handed us. */
    (void)mach_port_deallocate(mach_task_self(), name);

    if (subscriber != MACH_PORT_NULL)
        fire_exit_notify(subscriber, pid, code);

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
    if (kr) goto fatal;
    kr = mach_port_allocate(mach_task_self(),
                            MACH_PORT_RIGHT_RECEIVE, &mount_port);
    if (kr) goto fatal;
    kr = mach_port_allocate(mach_task_self(),
                            MACH_PORT_RIGHT_RECEIVE, &notify_port);
    if (kr) goto fatal;
    kr = mach_port_allocate(mach_task_self(),
                            MACH_PORT_RIGHT_PORT_SET, &port_set);
    if (kr) goto fatal;

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
        goto fatal;
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

fatal:
    printf("proc: bring-up failed kr=%d\n", kr);
    return 1;
}
