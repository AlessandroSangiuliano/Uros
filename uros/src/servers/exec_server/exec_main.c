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
#include <pthread.h>
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

static mach_port_t      exec_port;      /* our service port: file loads */

/*
 * #964: the image port, "exec_image", served by a thread of its own: an image
 * load reads no file, and never waits behind one that does (exec.defs).  The
 * file port is served by EXEC_FILE_THREADS threads, so that one execve held on
 * a file does not hold the next.  The loader keeps no state outside its stack
 * and its allocations, and libmach's malloc takes its lock from the second
 * thread on (#540).
 */
static mach_port_t      exec_image_port;
#define EXEC_FILE_THREADS       4

/* ------------------------------------------------------------------ */
/*  MIG handler — called from the generated exec_server() dispatcher  */
/* ------------------------------------------------------------------ */

kern_return_t
exec_S_load(
    mach_port_t                 server_port,
    mach_port_t                 task,
    mach_port_t                 parent_caps,
    exec_path_t                 path,
    pointer_t                   argv_blob,
    mach_msg_type_number_t      argv_blobCnt,
    pointer_t                   envp_blob,
    mach_msg_type_number_t      envp_blobCnt,
    mach_port_t                *new_thread,
    int                        *result_code)
{
    int rc;

    *new_thread = MACH_PORT_NULL;

    /* #964: the image port loads images alone, so that it never waits. */
    if (server_port == exec_image_port) {
        printf("exec: a file load, %s, asked on the image port: refused "
               "(#964)\n", path);
        rc = EXEC_ERR_WRONG_PORT;
    } else
        rc = exec_do_load(task, parent_caps, path,
                          (const void *)argv_blob, argv_blobCnt,
                          (const void *)envp_blob, envp_blobCnt,
                          new_thread);
    *result_code = rc;

    /*
     * #926: the task is the caller's, lent for the length of this call, and
     * the send right to it is this server's to give back, whatever the
     * result: kept, it would be the caller's child in this server's hands
     * for good.  The first thread goes back MOVED (exec.defs), so nothing
     * of it stays here either.
     */
    if (MACH_PORT_VALID(task))
        (void)mach_port_deallocate(mach_task_self(), task);

    /*
     * #743: the caller's per-task port is ours to give back, whatever the
     * result: this routine answers KERN_SUCCESS, so the request is not
     * destroyed for us.  Kept, it would be a sender the caller's port never
     * loses, and its slot in cap_server would outlive the caller.
     */
    if (MACH_PORT_VALID(parent_caps))
        (void)mach_port_deallocate(mach_task_self(), parent_caps);

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

/*
 * #964: an image the caller hands, asked on the image port alone.  What the
 * request brought -- the task, the caller's port, the four buffers -- is this
 * server's to give back, whatever the result, as exec_S_load() gives back its.
 */
kern_return_t
exec_S_load_image(
    mach_port_t                 server_port,
    mach_port_t                 task,
    mach_port_t                 parent_caps,
    pointer_t                   image,
    mach_msg_type_number_t      imageCnt,
    pointer_t                   manifest,
    mach_msg_type_number_t      manifestCnt,
    pointer_t                   argv_blob,
    mach_msg_type_number_t      argv_blobCnt,
    pointer_t                   envp_blob,
    mach_msg_type_number_t      envp_blobCnt,
    mach_port_t                *new_thread,
    int                        *result_code)
{
    int rc;

    *new_thread = MACH_PORT_NULL;

    if (server_port != exec_image_port) {
        printf("exec: an image load asked on exec_server's port, where it "
               "could wait behind a file: refused (#964)\n");
        rc = EXEC_ERR_WRONG_PORT;
    } else
        rc = exec_do_load_image(task, parent_caps,
                                (void *)image, imageCnt,
                                manifestCnt != 0 ? (const void *)manifest
                                                 : NULL,
                                (uint32_t)manifestCnt,
                                (const void *)argv_blob, argv_blobCnt,
                                (const void *)envp_blob, envp_blobCnt,
                                new_thread);
    *result_code = rc;

    if (MACH_PORT_VALID(task))
        (void)mach_port_deallocate(mach_task_self(), task);
    if (MACH_PORT_VALID(parent_caps))
        (void)mach_port_deallocate(mach_task_self(), parent_caps);
    if (image)
        vm_deallocate(mach_task_self(), (vm_address_t)image, imageCnt);
    if (manifest)
        vm_deallocate(mach_task_self(), (vm_address_t)manifest, manifestCnt);
    if (argv_blob)
        vm_deallocate(mach_task_self(), (vm_address_t)argv_blob,
                      argv_blobCnt);
    if (envp_blob)
        vm_deallocate(mach_task_self(), (vm_address_t)envp_blob,
                      envp_blobCnt);
    return KERN_SUCCESS;
}

/* #964: one port's loop, for a thread of the pool or the image's own. */
static void *
serve(void *port)
{
    for (;;)
        (void)mach_msg_server(exec_server, 8192,
                              (mach_port_t)(uintptr_t)port,
                              MACH_MSG_OPTION_NONE);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Bring-up                                                           */
/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    kern_return_t kr;
    mach_port_t sig = MACH_PORT_NULL;

    (void)argc; (void)argv;

    kr = bootstrap_ports(bootstrap_port,
                         &host_port, &device_port,
                         &root_ledger_wired, &root_ledger_paged,
                         &security_port);
    if (kr != KERN_SUCCESS)
        return 1;

    printf_init(device_port);
    /*
     * 🔴 AND GIVEN BACK (#511).  That port is bus authority -- configuration
     * space, MMIO, DMA, interrupts -- and this task wanted it to open a
     * console.  See the note above printf_init() in libmach/printf.c.
     */
    (void) mach_port_deallocate(mach_task_self(), device_port);
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
    /* #744: signed with a port only this task holds, not MACH_PORT_NULL. */
    kr = urmach_netname_signature(&sig);
    if (kr == KERN_SUCCESS)
        kr = netname_check_in(name_server_port, (char *)"exec_server",
                              sig, exec_port);
    if (kr != NETNAME_SUCCESS) {
        printf("exec: netname_check_in failed kr=%d\n", kr);
        return 1;
    }
    printf("exec: registered as \"exec_server\"\n");

    /*
     * #964: the image port, on a thread of its own, and the pool on the file
     * port: both served before bootstrap is told, so that the HAL, which
     * comes after this server, finds them answering.
     */
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                            &exec_image_port);
    if (kr == KERN_SUCCESS)
        kr = mach_port_insert_right(mach_task_self(), exec_image_port,
                                    exec_image_port, MACH_MSG_TYPE_MAKE_SEND);
    if (kr == KERN_SUCCESS)
        kr = netname_check_in(name_server_port, (char *)"exec_image", sig,
                              exec_image_port);
    if (kr != KERN_SUCCESS) {
        printf("exec: the image port not published (kr=%d): no driver can "
               "be loaded (#964)\n", kr);
        return 1;
    }
    {
        pthread_t   t;
        int         i;

        if (pthread_create(&t, NULL, serve,
                           (void *)(uintptr_t)exec_image_port) != 0) {
            printf("exec: no thread for the image port (#964)\n");
            return 1;
        }
        for (i = 1; i < EXEC_FILE_THREADS; i++)
            if (pthread_create(&t, NULL, serve,
                               (void *)(uintptr_t)exec_port) != 0) {
                printf("exec: the file port has %d thread(s), not %d "
                       "(#964)\n", i, EXEC_FILE_THREADS);
                break;
            }
    }
    printf("exec: registered as \"exec_image\", on a thread of its own; "
           "file loads on %d threads (#964)\n", EXEC_FILE_THREADS);

    /* Tell bootstrap we are ready. */
    bootstrap_completed(bootstrap_port, mach_task_self());

    printf("exec: ready, entering message loop\n");

    /* This thread is one of the file port's. */
    (void)serve((void *)(uintptr_t)exec_port);

    printf("exec: mach_msg_server exited unexpectedly\n");
    return 1;
}
