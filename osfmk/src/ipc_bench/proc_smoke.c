/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * proc_smoke.c — end-to-end smoke for proc_server v0.1.0 (#237).
 *
 * Reuses exec_server (#228) to spawn /hello_exec, then registers the
 * resulting task with proc_server, exercises:
 *   - proc_register   → returns a pid
 *   - proc_list       → must include the new pid
 *   - libvfs.vfs_open + vfs_read on /proc/<pid>/stat
 *   - proc_subscribe_exit + task_terminate + mach_msg_receive on
 *     the notify port — must arrive within a short timeout
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <servers/netname.h>
#include <servers/netname_defs.h>
#include <libvfs.h>
#include <proc_types.h>
#include <stdio.h>
#include <string.h>

/* MIG user stubs from exec.defs / proc.defs (subsystems 3100 / 3200). */
extern kern_return_t exec_load(
    mach_port_t exec_port, mach_port_t client_task, char *path,
    pointer_t argv_blob, mach_msg_type_number_t argv_blobCnt,
    pointer_t envp_blob, mach_msg_type_number_t envp_blobCnt,
    mach_port_t *new_task, mach_port_t *new_thread, int *result_code);

extern kern_return_t proc_register(
    mach_port_t proc_port, proc_pid_t parent_pid,
    mach_port_t task_port, char *cmdline,
    proc_pid_t *new_pid, int *result);

extern kern_return_t proc_subscribe_exit(
    mach_port_t proc_port, proc_pid_t pid,
    mach_port_t notify_port, int *result);

extern kern_return_t proc_list(
    mach_port_t proc_port, pointer_t *entries,
    mach_msg_type_number_t *entriesCnt, unsigned *count, int *result);

void
bench_proc_smoke(void)
{
    mach_port_t exec_port = MACH_PORT_NULL;
    mach_port_t proc_port = MACH_PORT_NULL;
    mach_port_t new_task = MACH_PORT_NULL, new_thread = MACH_PORT_NULL;
    mach_port_t notify_recv = MACH_PORT_NULL;
    mach_port_t notify_send = MACH_PORT_NULL;
    proc_pid_t pid = 0;
    int rc = 0;
    kern_return_t kr;
    static char argv[] = "hello_exec\0";
    static char envp[] = "\0";

    printf("\n--- proc_server smoke test (#237 v0.1.0) ---\n");

    if (netname_look_up(name_server_port, (char *)"",
                        (char *)"exec_server", &exec_port)
        != NETNAME_SUCCESS) {
        printf("  proc: exec_server not registered — skipped\n");
        return;
    }
    if (netname_look_up(name_server_port, (char *)"",
                        (char *)"proc_server", &proc_port)
        != NETNAME_SUCCESS) {
        printf("  proc: proc_server not registered — skipped\n");
        return;
    }

    /* 1. Spawn hello_exec via exec_server. */
    kr = exec_load(exec_port, mach_task_self(), (char *)"/hello_exec",
                   (pointer_t)argv, sizeof(argv),
                   (pointer_t)envp, sizeof(envp),
                   &new_task, &new_thread, &rc);
    if (kr != KERN_SUCCESS || rc != 0) {
        printf("  proc: exec_load failed kr=%d rc=%d — FAIL\n",
               kr, rc);
        return;
    }

    /* 2. Register with proc_server.  proc_server installs a
     *    DEAD_NAME notify so it can mark the pid zombie when we
     *    task_terminate later. */
    rc = 0;
    kr = proc_register(proc_port, /*parent*/ 0, new_task,
                       (char *)"hello_exec", &pid, &rc);
    if (kr != KERN_SUCCESS || rc != 0 || pid == 0) {
        printf("  proc: proc_register failed kr=%d rc=%d pid=%u "
               "— FAIL\n", kr, rc, pid);
        (void)task_terminate(new_task);
        return;
    }
    printf("  proc: proc_register -> pid=%u\n", pid);

    /* 3. proc_list must contain our pid. */
    {
        pointer_t entries = 0;
        mach_msg_type_number_t entriesCnt = 0;
        unsigned n = 0;
        int found = 0;

        rc = 0;
        kr = proc_list(proc_port, &entries, &entriesCnt, &n, &rc);
        if (kr != KERN_SUCCESS || rc != 0) {
            printf("  proc: proc_list failed kr=%d rc=%d — FAIL\n",
                   kr, rc);
        } else {
            unsigned i;
            proc_entry_t *arr = (proc_entry_t *)entries;
            for (i = 0; i < n; i++)
                if (arr[i].pid == pid) {
                    found = 1;
                    printf("  proc: list contains pid=%u state=%c "
                           "cmd=\"%s\"\n",
                           arr[i].pid, arr[i].state, arr[i].cmdline);
                    break;
                }
            if (entries)
                vm_deallocate(mach_task_self(),
                              (vm_address_t)entries, entriesCnt);
        }
        if (!found)
            printf("  proc: list MISSED pid=%u — FAIL\n", pid);
    }

    /* 4. Read /proc/<pid>/stat via libvfs. */
    {
        char path[64];
        char buf[256];
        ssize_t got;
        vfs_fd_t fd;

        snprintf(path, sizeof(path), "/proc/%u/stat", (unsigned)pid);
        fd = vfs_open(path, VFS_O_RDONLY, 0);
        if (fd == VFS_FD_INVALID) {
            printf("  proc: vfs_open(%s) failed — FAIL\n", path);
        } else {
            got = vfs_read(fd, buf, sizeof(buf) - 1);
            if (got > 0) {
                buf[got] = '\0';
                /* Strip trailing newline for clean printout. */
                if (got > 0 && buf[got - 1] == '\n') buf[got - 1] = '\0';
                printf("  proc: %s -> \"%s\"\n", path, buf);
            } else {
                printf("  proc: vfs_read(%s) = %ld — FAIL\n",
                       path, (long)got);
            }
            vfs_close(fd);
        }
    }

    /* 5. Subscribe to exit, then terminate, then receive. */
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                            &notify_recv);
    if (kr) { printf("  proc: notify port_allocate kr=%d\n", kr); goto out; }
    /* MIG generates the user stub with mach_port_make_send_once_t for
     * notify_port — it does the receive→send-once disposition switch
     * on the wire, no manual mach_port_insert_right needed. */
    notify_send = notify_recv;

    rc = 0;
    kr = proc_subscribe_exit(proc_port, pid, notify_send, &rc);
    if (kr != KERN_SUCCESS || rc != 0) {
        printf("  proc: subscribe_exit failed kr=%d rc=%d — FAIL\n",
               kr, rc);
        goto out;
    }

    (void)task_terminate(new_task);

    {
        proc_exit_msg_t reply;
        memset(&reply, 0, sizeof(reply));
        reply.head.msgh_local_port = notify_recv;
        kr = mach_msg(&reply.head,
                      MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                      0, sizeof(reply), notify_recv,
                      /* timeout ms */ 1000, MACH_PORT_NULL);
        if (kr == KERN_SUCCESS) {
            printf("  proc: exit notification: pid=%u code=%d\n",
                   reply.pid, reply.exit_code);
            printf("  proc: PASS\n");
        } else {
            printf("  proc: notify timeout kr=%d — FAIL\n", kr);
        }
    }

out:
    if (notify_recv != MACH_PORT_NULL)
        (void)mach_port_destroy(mach_task_self(), notify_recv);
    if (new_thread != MACH_PORT_NULL)
        (void)mach_port_deallocate(mach_task_self(), new_thread);
}
