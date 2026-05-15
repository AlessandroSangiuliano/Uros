/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * exec_server.c — main + Mach msg loop + MIG handler glue for
 * exec_server v0.1.0 (#228).
 *
 * The heavy lifting (read binary, parse, install segments, build
 * stack, start thread) lives in exec_load.c / exec_stack.c; this
 * file is the front door.
 */

#include "exec_types.h"
#include "exec_internal.h"

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/bootstrap.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <sa_mach.h>
#include <servers/netname.h>
#include <servers/netname_defs.h>
#include <libvfs.h>
#include <stdio.h>
#include <string.h>

/* MIG-generated server demux + impl prototypes. */
#include "exec_server.h"

/* Globals from bootstrap_ports — we keep them around for printf init
 * and possible future use even though v0.1.0 doesn't need most. */
static mach_port_t      host_port;
static mach_port_t      device_port;
static mach_port_t      security_port;
static mach_port_t      root_ledger_wired;
static mach_port_t      root_ledger_paged;

static mach_port_t      exec_port;      /* our service port */

/* ------------------------------------------------------------------ */
/*  MIG handler — called from the generated exec_server() dispatcher  */
/* ------------------------------------------------------------------ */

kern_return_t
exec_S_load(
    mach_port_t                 server_port,
    mach_port_t                      client_task,
    exec_path_t                 path,
    pointer_t                   argv_blob,
    mach_msg_type_number_t      argv_blobCnt,
    pointer_t                   envp_blob,
    mach_msg_type_number_t      envp_blobCnt,
    mach_port_t                      *new_task,
    mach_port_t                    *new_thread,
    int                         *result_code)
{
    int rc;

    (void)server_port;

    *new_task   = MACH_PORT_NULL;
    *new_thread = MACH_PORT_NULL;

    rc = exec_do_load(client_task, path,
                      (const void *)argv_blob, argv_blobCnt,
                      (const void *)envp_blob, envp_blobCnt,
                      new_task, new_thread);
    *result_code = rc;

    /* MIG OOL: deallocate the OOL buffers it handed us. */
    if (argv_blob)
        vm_deallocate(mach_task_self(), (vm_address_t)argv_blob,
                      argv_blobCnt);
    if (envp_blob)
        vm_deallocate(mach_task_self(), (vm_address_t)envp_blob,
                      envp_blobCnt);

    /* MIG demands KERN_SUCCESS on the message-level return; the
     * application-level result lives in result_code. */
    return KERN_SUCCESS;
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

    printf("\n=== exec_server " EXEC_SERVER_VERSION_STRING " ===\n");

    /* libvfs uses name_server lookups internally; init is idempotent. */
    if (vfs_init() != 0) {
        printf("exec: vfs_init failed\n");
        return 1;
    }

    /* Allocate and publish the service port. */
    kr = mach_port_allocate(mach_task_self(),
                            MACH_PORT_RIGHT_RECEIVE, &exec_port);
    if (kr != KERN_SUCCESS) {
        printf("exec: port_allocate failed kr=%d\n", kr);
        return 1;
    }
    kr = mach_port_insert_right(mach_task_self(), exec_port, exec_port,
                                MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        printf("exec: insert_right failed kr=%d\n", kr);
        return 1;
    }
    kr = netname_check_in(name_server_port, (char *)"exec_server",
                          MACH_PORT_NULL, exec_port);
    if (kr != NETNAME_SUCCESS) {
        printf("exec: netname_check_in failed kr=%d\n", kr);
        return 1;
    }
    printf("exec: registered as \"exec_server\"\n");

    /* Tell bootstrap we are ready. */
    bootstrap_completed(bootstrap_port, mach_task_self());

    printf("exec: ready, entering message loop\n");

    /* MIG-generated dispatcher does the heavy lifting. */
    mach_msg_server(exec_server, 8192, exec_port, MACH_MSG_OPTION_NONE);

    printf("exec: mach_msg_server exited unexpectedly\n");
    return 1;
}
