/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * exec_smoke.c — end-to-end smoke test for exec_server v0.1.0 (#228).
 *
 * Looks up exec_server via name_server, asks it to load /hello_exec,
 * verifies the new task starts (mach_print output appears in console),
 * tears it down.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <servers/netname.h>
#include <servers/netname_defs.h>
#include <stdio.h>
#include <string.h>

/* MIG user stub generated from exec.defs (subsystem 3100). */
extern kern_return_t exec_load(
    mach_port_t exec_port,
    mach_port_t client_task,
    char *path,
    pointer_t argv_blob, mach_msg_type_number_t argv_blobCnt,
    pointer_t envp_blob, mach_msg_type_number_t envp_blobCnt,
    mach_port_t *new_task,
    mach_port_t *new_thread,
    int *result_code);

void
bench_exec_smoke(void)
{
    mach_port_t exec_port = MACH_PORT_NULL;
    mach_port_t new_task   = MACH_PORT_NULL;
    mach_port_t new_thread = MACH_PORT_NULL;
    kern_return_t kr;
    int rc = 0;

    /* argv = ["hello_exec"], envp = empty.
     * Format: NUL-separated strings + double-NUL terminator. */
    static char argv[] = "hello_exec\0";
    static char envp[] = "\0";

    printf("\n--- exec_server smoke test (#228 v0.1.0) ---\n");

    kr = netname_look_up(name_server_port, (char *)"",
                         (char *)"exec_server", &exec_port);
    if (kr != NETNAME_SUCCESS) {
        printf("  exec: exec_server not registered (kr=%d) — skipped\n",
               kr);
        return;
    }

    kr = exec_load(exec_port, mach_task_self(),
                   (char *)"/hello_exec",
                   (pointer_t)argv, sizeof(argv),
                   (pointer_t)envp, sizeof(envp),
                   &new_task, &new_thread, &rc);
    if (kr != KERN_SUCCESS) {
        printf("  exec: exec_load RPC failed kr=%d — FAIL\n", kr);
        return;
    }

    if (rc != 0) {
        printf("  exec: exec_load returned EXEC_ERR=%d — FAIL\n", rc);
        return;
    }

    printf("  exec: hello_exec spawned (task=0x%x thread=0x%x) — PASS\n",
           (unsigned)new_task, (unsigned)new_thread);

    /* Give hello_exec a moment to print, then tear it down so the
     * boot timeline doesn't accumulate runaway tasks. */
    {
        mach_port_t waste;
        (void)mach_port_allocate(mach_task_self(),
                                 MACH_PORT_RIGHT_RECEIVE, &waste);
        mach_msg_header_t hdr;
        (void)mach_msg(&hdr,
                       MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                       0, sizeof(hdr), waste,
                       /* timeout ms */ 200,
                       MACH_PORT_NULL);
        (void)mach_port_deallocate(mach_task_self(), waste);
    }

    (void)task_terminate(new_task);
    (void)mach_port_deallocate(mach_task_self(), new_thread);
}
