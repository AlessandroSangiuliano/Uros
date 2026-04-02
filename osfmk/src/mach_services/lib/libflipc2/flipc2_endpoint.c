/*
 * FLIPC v2 — Endpoint registry (named channel discovery + connection).
 *
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
 * libflipc2/flipc2_endpoint.c
 *
 * Three-layer architecture:
 *   L3: Endpoint registry (name -> channel, discovery via Mach name server)
 *   L2: Connection manager (accept/connect, 1 SPSC pair per client)
 *   L1: flipc2_channel (SPSC ring, ~5 ns/desc)   ← existing
 *
 * Server side:
 *   - flipc2_endpoint_create() registers a contact port via netname
 *   - flipc2_endpoint_accept() blocks until a client connects
 *   - ds_flipc2_endpoint_connect() handles the MIG connect request
 *
 * Client side:
 *   - flipc2_endpoint_connect() looks up the name and calls connect_port
 *   - flipc2_endpoint_connect_port() does the MIG handshake
 *
 * Dead peer detection:
 *   - Server requests MACH_NOTIFY_DEAD_NAME on client_task
 *   - On notification, the connection slot is cleaned up
 */

#include <flipc2.h>
#include <mach/mach_interface.h>
#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <mach/mig_errors.h>
#include <mach/notify.h>
#include <mach_init.h>
#include <servers/netname.h>
#include <string.h>

#include "flipc2_endpoint.h"
#include "flipc2_endpoint_server.h"

/* Not yet in our exported headers */
extern kern_return_t mach_port_set_protected_payload(
    mach_port_t task, mach_port_t port, unsigned long payload);

extern kern_return_t mach_msg_server_once(
    boolean_t (*demux)(mach_msg_header_t *, mach_msg_header_t *),
    mach_msg_size_t max_size, mach_port_t rcv_name,
    mach_msg_option_t options);

/* ------------------------------------------------------------------ */
/*  Internal structures                                                */
/* ------------------------------------------------------------------ */

#define FLIPC2_EP_MAX_CLIENTS_MAX   64
#define FLIPC2_EP_NAME_MAX          128

struct flipc2_ep_conn {
    flipc2_channel_t    fwd_ch;         /* server→client */
    flipc2_channel_t    rev_ch;         /* client→server */
    mach_port_t         client_task;    /* send right to client task */
    int                 active;         /* 1 = connected */
};

struct flipc2_endpoint {
    char                name[FLIPC2_EP_NAME_MAX];
    mach_port_t         recv_port;      /* contact port (PP-tagged) */
    mach_port_t         port_set;       /* receives connect + notifications */
    uint32_t            max_clients;
    uint32_t            n_clients;
    uint32_t            channel_size;   /* default channel params */
    uint32_t            ring_entries;
    uint32_t            flags;          /* FLIPC2_CREATE_ISOLATED etc. */
    int                 pending_conn_idx; /* set by MIG stub, read by accept */
    struct flipc2_ep_conn conns[FLIPC2_EP_MAX_CLIENTS_MAX];
};

/* ------------------------------------------------------------------ */
/*  Global storage for the endpoint pointer used by the MIG stubs     */
/*                                                                     */
/*  The MIG-generated server stubs receive the first argument from     */
/*  msgh_local_port.  When protected payloads are enabled, this is     */
/*  the payload value, not the port name.  We cast it to get back      */
/*  the struct flipc2_endpoint pointer.                                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Dead-name notification handler                                     */
/* ------------------------------------------------------------------ */

static void
flipc2_ep_cleanup_dead(struct flipc2_endpoint *ep,
                       mach_msg_header_t *msg)
{
    mach_dead_name_notification_t *notif =
        (mach_dead_name_notification_t *)msg;
    mach_port_t dead_port = notif->not_port;
    uint32_t i;

    for (i = 0; i < ep->max_clients; i++) {
        if (!ep->conns[i].active)
            continue;
        if (ep->conns[i].client_task != dead_port)
            continue;

        /* Found the dead client — destroy its channels */
        flipc2_channel_destroy(ep->conns[i].fwd_ch);
        flipc2_channel_destroy(ep->conns[i].rev_ch);

        /* Release the dead name */
        mach_port_deallocate(mach_task_self(), dead_port);

        ep->conns[i].active = 0;
        ep->conns[i].client_task = MACH_PORT_NULL;
        ep->n_clients--;
        return;
    }

    /* Unknown dead name — just deallocate */
    mach_port_deallocate(mach_task_self(), dead_port);
}

/* ------------------------------------------------------------------ */
/*  Demux: MIG connect/disconnect + dead-name notifications            */
/* ------------------------------------------------------------------ */

static boolean_t
flipc2_ep_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
    /* Try MIG endpoint server demux first */
    if (flipc2_endpoint_server(in, out))
        return TRUE;

    /* Handle dead-name notifications */
    if (in->msgh_id == MACH_NOTIFY_DEAD_NAME) {
        struct flipc2_endpoint *ep =
            (struct flipc2_endpoint *)(unsigned long)in->msgh_local_port;
        flipc2_ep_cleanup_dead(ep, in);
        /* Mark as no-reply notification */
        ((mig_reply_error_t *)out)->RetCode = MIG_NO_REPLY;
        out->msgh_size = sizeof(mig_reply_error_t);
        return TRUE;
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  MIG server stubs (called by flipc2_endpoint_server)                */
/* ------------------------------------------------------------------ */

/*
 * ds_flipc2_endpoint_connect — MIG server stub for client connect.
 *
 * The first argument (server_port) is the protected payload, which
 * is the struct flipc2_endpoint pointer.
 */
kern_return_t
ds_flipc2_endpoint_connect_rpc(
    mach_port_t         server_port,
    mach_port_t         client_task,
    unsigned int        channel_size,
    unsigned int        ring_entries,
    unsigned int       *fwd_addr,
    unsigned int       *rev_addr,
    mach_port_t        *fwd_sem,
    mach_port_t        *fwd_sem_prod,
    mach_port_t        *rev_sem,
    mach_port_t        *rev_sem_prod,
    unsigned int       *size_out,
    unsigned int       *entries_out)
{
    struct flipc2_endpoint *ep =
        (struct flipc2_endpoint *)(unsigned long)server_port;
    flipc2_return_t     ret;
    flipc2_channel_t    fwd_ch = (flipc2_channel_t)0;
    flipc2_channel_t    rev_ch = (flipc2_channel_t)0;
    mach_port_t         fwd_sem_port, rev_sem_port;
    vm_address_t        fwd_client_addr, rev_client_addr;
    kern_return_t       kr;
    mach_port_t         prev;
    uint32_t            i;
    uint32_t            cs, re;
    int                 slot;

    /* Check client limit */
    if (ep->n_clients >= ep->max_clients)
        return KERN_RESOURCE_SHORTAGE;

    /* Use server defaults if client passed 0 */
    cs = (channel_size != 0) ? channel_size : ep->channel_size;
    re = (ring_entries != 0) ? ring_entries : ep->ring_entries;

    /* Clamp to valid range */
    if (cs < FLIPC2_CHANNEL_SIZE_MIN)
        cs = FLIPC2_CHANNEL_SIZE_MIN;
    if (cs > FLIPC2_CHANNEL_SIZE_MAX)
        cs = FLIPC2_CHANNEL_SIZE_MAX;
    if (re < FLIPC2_RING_ENTRIES_MIN)
        re = FLIPC2_RING_ENTRIES_MIN;
    if (re > FLIPC2_RING_ENTRIES_MAX)
        re = FLIPC2_RING_ENTRIES_MAX;

    /* Find a free slot */
    slot = -1;
    for (i = 0; i < ep->max_clients; i++) {
        if (!ep->conns[i].active) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0)
        return KERN_RESOURCE_SHORTAGE;

    /* Create forward channel (server→client) */
    ret = flipc2_channel_create_ex(cs, re, ep->flags, &fwd_ch, &fwd_sem_port);
    if (ret != FLIPC2_SUCCESS)
        return KERN_RESOURCE_SHORTAGE;

    /* Create reverse channel (client→server) */
    ret = flipc2_channel_create_ex(cs, re, ep->flags, &rev_ch, &rev_sem_port);
    if (ret != FLIPC2_SUCCESS) {
        flipc2_channel_destroy(fwd_ch);
        return KERN_RESOURCE_SHORTAGE;
    }

    /* Map forward channel into client (server=producer, client=consumer) */
    if (ep->flags & FLIPC2_CREATE_ISOLATED) {
        ret = flipc2_channel_share_isolated(fwd_ch, client_task,
                                            FLIPC2_ROLE_CONSUMER,
                                            &fwd_client_addr);
    } else {
        ret = flipc2_channel_share(fwd_ch, client_task, &fwd_client_addr);
    }
    if (ret != FLIPC2_SUCCESS) {
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return KERN_FAILURE;
    }

    /* Map reverse channel into client (client=producer, server=consumer) */
    if (ep->flags & FLIPC2_CREATE_ISOLATED) {
        ret = flipc2_channel_share_isolated(rev_ch, client_task,
                                            FLIPC2_ROLE_PRODUCER,
                                            &rev_client_addr);
    } else {
        ret = flipc2_channel_share(rev_ch, client_task, &rev_client_addr);
    }
    if (ret != FLIPC2_SUCCESS) {
        vm_deallocate(client_task, fwd_client_addr, cs);
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return KERN_FAILURE;
    }

    /* Request dead-name notification on client task port */
    prev = MACH_PORT_NULL;
    kr = mach_port_request_notification(mach_task_self(), client_task,
                                        MACH_NOTIFY_DEAD_NAME, 0,
                                        ep->recv_port,
                                        MACH_MSG_TYPE_MAKE_SEND_ONCE,
                                        &prev);
    if (kr != KERN_SUCCESS) {
        vm_deallocate(client_task, rev_client_addr, cs);
        vm_deallocate(client_task, fwd_client_addr, cs);
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return KERN_FAILURE;
    }
    if (prev != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), prev);

    /* Fill in output parameters */
    *fwd_addr     = (unsigned int)fwd_client_addr;
    *rev_addr     = (unsigned int)rev_client_addr;
    *fwd_sem      = fwd_ch->hdr->wakeup_sem;
    *fwd_sem_prod = fwd_ch->hdr->wakeup_sem_prod;
    *rev_sem      = rev_ch->hdr->wakeup_sem;
    *rev_sem_prod = rev_ch->hdr->wakeup_sem_prod;
    *size_out     = cs;
    *entries_out  = re;

    /* Store connection */
    ep->conns[slot].fwd_ch      = fwd_ch;
    ep->conns[slot].rev_ch      = rev_ch;
    ep->conns[slot].client_task = client_task;
    ep->conns[slot].active      = 1;
    ep->n_clients++;
    ep->pending_conn_idx = slot;

    return KERN_SUCCESS;
}

/*
 * ds_flipc2_endpoint_disconnect_rpc — MIG server stub for disconnect.
 */
kern_return_t
ds_flipc2_endpoint_disconnect_rpc(
    mach_port_t         server_port,
    mach_port_t         client_task)
{
    struct flipc2_endpoint *ep =
        (struct flipc2_endpoint *)(unsigned long)server_port;
    uint32_t i;

    for (i = 0; i < ep->max_clients; i++) {
        if (!ep->conns[i].active)
            continue;
        if (ep->conns[i].client_task != client_task)
            continue;

        flipc2_channel_destroy(ep->conns[i].fwd_ch);
        flipc2_channel_destroy(ep->conns[i].rev_ch);
        mach_port_deallocate(mach_task_self(), client_task);

        ep->conns[i].active = 0;
        ep->conns[i].client_task = MACH_PORT_NULL;
        ep->n_clients--;
        return KERN_SUCCESS;
    }

    return KERN_INVALID_ARGUMENT;
}

/* ================================================================== */
/*  Server-side public API                                             */
/* ================================================================== */

flipc2_return_t
flipc2_endpoint_create(
    const char         *name,
    uint32_t            max_clients,
    uint32_t            channel_size,
    uint32_t            ring_entries,
    uint32_t            flags,
    flipc2_endpoint_t  *ep_out)
{
    kern_return_t       kr;
    struct flipc2_endpoint *ep;
    vm_address_t        addr;
    uint32_t            i;

    if (!name || !ep_out)
        return FLIPC2_ERR_INVALID_ARGUMENT;
    if (max_clients == 0 || max_clients > FLIPC2_EP_MAX_CLIENTS_MAX)
        return FLIPC2_ERR_INVALID_ARGUMENT;
    if (channel_size < FLIPC2_CHANNEL_SIZE_MIN ||
        channel_size > FLIPC2_CHANNEL_SIZE_MAX)
        return FLIPC2_ERR_INVALID_ARGUMENT;
    if (ring_entries < FLIPC2_RING_ENTRIES_MIN ||
        ring_entries > FLIPC2_RING_ENTRIES_MAX)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    /* Allocate endpoint struct */
    addr = 0;
    kr = vm_allocate(mach_task_self(), &addr,
                     sizeof(struct flipc2_endpoint), TRUE);
    if (kr != KERN_SUCCESS)
        return FLIPC2_ERR_RESOURCE_SHORTAGE;

    ep = (struct flipc2_endpoint *)addr;
    memset(ep, 0, sizeof(struct flipc2_endpoint));

    strncpy(ep->name, name, FLIPC2_EP_NAME_MAX - 1);
    ep->name[FLIPC2_EP_NAME_MAX - 1] = '\0';
    ep->max_clients   = max_clients;
    ep->channel_size  = channel_size;
    ep->ring_entries  = ring_entries;
    ep->flags         = flags;
    ep->n_clients     = 0;
    ep->pending_conn_idx = -1;

    for (i = 0; i < FLIPC2_EP_MAX_CLIENTS_MAX; i++)
        ep->conns[i].active = 0;

    /* Allocate receive port */
    kr = mach_port_allocate(mach_task_self(),
                            MACH_PORT_RIGHT_RECEIVE,
                            &ep->recv_port);
    if (kr != KERN_SUCCESS) {
        vm_deallocate(mach_task_self(), addr,
                      sizeof(struct flipc2_endpoint));
        return FLIPC2_ERR_KERNEL;
    }

    /* Insert send right for netname registration */
    kr = mach_port_insert_right(mach_task_self(),
                                ep->recv_port, ep->recv_port,
                                MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        mach_port_mod_refs(mach_task_self(), ep->recv_port,
                           MACH_PORT_RIGHT_RECEIVE, -1);
        vm_deallocate(mach_task_self(), addr,
                      sizeof(struct flipc2_endpoint));
        return FLIPC2_ERR_KERNEL;
    }

    /* Set protected payload so MIG stubs receive the ep pointer */
    kr = mach_port_set_protected_payload(mach_task_self(),
                                         ep->recv_port,
                                         (unsigned long)ep);
    if (kr != KERN_SUCCESS) {
        mach_port_mod_refs(mach_task_self(), ep->recv_port,
                           MACH_PORT_RIGHT_RECEIVE, -1);
        vm_deallocate(mach_task_self(), addr,
                      sizeof(struct flipc2_endpoint));
        return FLIPC2_ERR_KERNEL;
    }

    /* Create port set */
    kr = mach_port_allocate(mach_task_self(),
                            MACH_PORT_RIGHT_PORT_SET,
                            &ep->port_set);
    if (kr != KERN_SUCCESS) {
        mach_port_mod_refs(mach_task_self(), ep->recv_port,
                           MACH_PORT_RIGHT_RECEIVE, -1);
        vm_deallocate(mach_task_self(), addr,
                      sizeof(struct flipc2_endpoint));
        return FLIPC2_ERR_KERNEL;
    }

    /* Add receive port to port set */
    kr = mach_port_move_member(mach_task_self(),
                               ep->recv_port, ep->port_set);
    if (kr != KERN_SUCCESS) {
        mach_port_mod_refs(mach_task_self(), ep->port_set,
                           MACH_PORT_RIGHT_PORT_SET, -1);
        mach_port_mod_refs(mach_task_self(), ep->recv_port,
                           MACH_PORT_RIGHT_RECEIVE, -1);
        vm_deallocate(mach_task_self(), addr,
                      sizeof(struct flipc2_endpoint));
        return FLIPC2_ERR_KERNEL;
    }

    /* Register with the Mach name server */
    kr = netname_check_in(name_server_port, (char *)name,
                          mach_task_self(), ep->recv_port);
    if (kr != KERN_SUCCESS) {
        mach_port_mod_refs(mach_task_self(), ep->port_set,
                           MACH_PORT_RIGHT_PORT_SET, -1);
        mach_port_mod_refs(mach_task_self(), ep->recv_port,
                           MACH_PORT_RIGHT_RECEIVE, -1);
        vm_deallocate(mach_task_self(), addr,
                      sizeof(struct flipc2_endpoint));
        return FLIPC2_ERR_NAME_EXISTS;
    }

    *ep_out = ep;
    return FLIPC2_SUCCESS;
}

flipc2_return_t
flipc2_endpoint_accept(
    flipc2_endpoint_t   ep,
    flipc2_channel_t   *fwd_ch,
    flipc2_channel_t   *rev_ch)
{
    kern_return_t kr;

    if (!ep || !fwd_ch || !rev_ch)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    ep->pending_conn_idx = -1;

    for (;;) {
        kr = mach_msg_server_once(flipc2_ep_demux, 8192,
                                  ep->port_set,
                                  MACH_MSG_OPTION_NONE);
        if (kr != KERN_SUCCESS)
            return FLIPC2_ERR_KERNEL;

        /* Check if a connection was established */
        if (ep->pending_conn_idx >= 0) {
            int idx = ep->pending_conn_idx;
            *fwd_ch = ep->conns[idx].fwd_ch;
            *rev_ch = ep->conns[idx].rev_ch;
            ep->pending_conn_idx = -1;
            return FLIPC2_SUCCESS;
        }
        /* Was a dead-name notification or disconnect — loop again */
    }
}

flipc2_return_t
flipc2_endpoint_destroy(
    flipc2_endpoint_t   ep)
{
    uint32_t i;

    if (!ep)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    /* Disconnect all active clients */
    for (i = 0; i < ep->max_clients; i++) {
        if (!ep->conns[i].active)
            continue;
        flipc2_channel_destroy(ep->conns[i].fwd_ch);
        flipc2_channel_destroy(ep->conns[i].rev_ch);
        mach_port_deallocate(mach_task_self(), ep->conns[i].client_task);
        ep->conns[i].active = 0;
    }

    /* Unregister from name server */
    netname_check_out(name_server_port, ep->name, mach_task_self());

    /* Destroy ports */
    mach_port_mod_refs(mach_task_self(), ep->recv_port,
                       MACH_PORT_RIGHT_RECEIVE, -1);
    mach_port_mod_refs(mach_task_self(), ep->port_set,
                       MACH_PORT_RIGHT_PORT_SET, -1);

    /* Free the struct */
    vm_deallocate(mach_task_self(), (vm_address_t)ep,
                  sizeof(struct flipc2_endpoint));

    return FLIPC2_SUCCESS;
}

mach_port_t
flipc2_endpoint_port(
    flipc2_endpoint_t   ep)
{
    if (!ep)
        return MACH_PORT_NULL;
    return ep->recv_port;
}

/* ================================================================== */
/*  Client-side public API                                             */
/* ================================================================== */

flipc2_return_t
flipc2_endpoint_connect_port(
    mach_port_t         server_port,
    uint32_t            channel_size,
    uint32_t            ring_entries,
    flipc2_channel_t   *fwd_ch_out,
    flipc2_channel_t   *rev_ch_out)
{
    kern_return_t       kr;
    unsigned int        fwd_addr, rev_addr;
    mach_port_t         fwd_sem, fwd_sem_prod;
    mach_port_t         rev_sem, rev_sem_prod;
    unsigned int        size_out, entries_out;
    flipc2_return_t     ret;
    flipc2_channel_t    fwd_ch, rev_ch;

    if (server_port == MACH_PORT_NULL || !fwd_ch_out || !rev_ch_out)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    /* Send connect request via MIG */
    kr = flipc2_endpoint_connect_rpc(server_port, mach_task_self(),
                                 channel_size, ring_entries,
                                 &fwd_addr, &rev_addr,
                                 &fwd_sem, &fwd_sem_prod,
                                 &rev_sem, &rev_sem_prod,
                                 &size_out, &entries_out);
    if (kr != KERN_SUCCESS) {
        if (kr == KERN_RESOURCE_SHORTAGE)
            return FLIPC2_ERR_MAX_CLIENTS;
        return FLIPC2_ERR_KERNEL;
    }

    /* Attach to forward channel (server→client, client consumes) */
    ret = flipc2_channel_attach_remote((vm_address_t)fwd_addr,
                                       size_out, &fwd_ch);
    if (ret != FLIPC2_SUCCESS) {
        vm_deallocate(mach_task_self(), (vm_address_t)fwd_addr, size_out);
        vm_deallocate(mach_task_self(), (vm_address_t)rev_addr, size_out);
        return ret;
    }
    flipc2_channel_set_semaphores(fwd_ch, fwd_sem, fwd_sem_prod);

    /* Attach to reverse channel (client→server, client produces) */
    ret = flipc2_channel_attach_remote((vm_address_t)rev_addr,
                                       size_out, &rev_ch);
    if (ret != FLIPC2_SUCCESS) {
        flipc2_channel_detach(fwd_ch);
        vm_deallocate(mach_task_self(), (vm_address_t)rev_addr, size_out);
        return ret;
    }
    flipc2_channel_set_semaphores(rev_ch, rev_sem, rev_sem_prod);

    *fwd_ch_out = fwd_ch;
    *rev_ch_out = rev_ch;
    return FLIPC2_SUCCESS;
}

flipc2_return_t
flipc2_endpoint_connect(
    const char         *name,
    uint32_t            channel_size,
    uint32_t            ring_entries,
    flipc2_channel_t   *fwd_ch,
    flipc2_channel_t   *rev_ch)
{
    kern_return_t       kr;
    mach_port_t         server_port;
    flipc2_return_t     ret;

    if (!name || !fwd_ch || !rev_ch)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    /* Look up the endpoint port in the name server */
    kr = netname_look_up(name_server_port, "", (char *)name, &server_port);
    if (kr != KERN_SUCCESS)
        return FLIPC2_ERR_NAME_NOT_FOUND;

    ret = flipc2_endpoint_connect_port(server_port, channel_size,
                                       ring_entries, fwd_ch, rev_ch);

    /* Release the looked-up send right */
    mach_port_deallocate(mach_task_self(), server_port);

    return ret;
}

flipc2_return_t
flipc2_endpoint_disconnect(
    flipc2_channel_t    fwd_ch,
    flipc2_channel_t    rev_ch)
{
    if (!fwd_ch || !rev_ch)
        return FLIPC2_ERR_INVALID_ARGUMENT;

    /* Deallocate semaphore send rights before detaching */
    if (fwd_ch->sem_port != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), fwd_ch->sem_port);
    if (fwd_ch->sem_port_prod != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), fwd_ch->sem_port_prod);
    if (rev_ch->sem_port != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), rev_ch->sem_port);
    if (rev_ch->sem_port_prod != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), rev_ch->sem_port_prod);

    flipc2_channel_detach(fwd_ch);
    flipc2_channel_detach(rev_ch);

    return FLIPC2_SUCCESS;
}
