/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

/*
 * cap_server.c — Uros capability server (v1).
 *
 * Serves the cap_server.defs MIG interface: clients call cap_acquire /
 * cap_derive / cap_revoke / cap_verify to manage HMAC-signed uros_cap
 * tokens.  The emitted tokens are opaque on the wire (cap_token_t blob)
 * and can be verified by any server (Issue B will replace the cap_verify
 * RPC with the UrMach kernel fast-path trap urmach_cap_verify).
 *
 * v1 scope (issue #176):
 *   - RESOURCE_BLK_DEVICE only
 *   - policy_allows_v1 is permissive (no real policy yet — arrives in
 *     a later issue with manifest / user / runtime layers)
 *   - single-shard cap_table protected by a single implicit mutex
 *     (Issue A is single-threaded)
 *   - revocation is cascaded via explicit stack walk over the
 *     delegation tree (CAP_MAX_DEPTH)
 */

#include <stddef.h>
#include <string.h>

#include <mach.h>
#include <mach/bootstrap.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <mach/cap_types.h>
#include <mach_init.h>
#include <sa_mach.h>
#include <stdio.h>
#include <stdlib.h>

#include "cap_table.h"
#include "cap_server_server.h"   /* MIG-generated demux prototype */
#include "cap_revoke.h"          /* MIG-generated cap_revoke_notify user stub */
#include "gpu_console.h"         /* #209: async printf mirror */

/*
 * netname_check_in: publish our service port under "cap_server".
 */
extern kern_return_t netname_check_in(mach_port_t server_port,
                                      char *service_name,
                                      mach_port_t signature,
                                      mach_port_t port);

/*
 * Bootstrap ports (standard for any Mach server).
 */
static mach_port_t host_port;
static mach_port_t device_port;
static mach_port_t security_port;
static mach_port_t root_ledger_wired;
static mach_port_t root_ledger_paged;

/*
 * Our receive port — netname publishes a send right to this.
 */
static mach_port_t cap_port;

/*
 * Dispatcher: the MIG-generated cap_server_server() handles every
 * subsystem 3500 message.  Anything else is unknown and gets a
 * MIG_BAD_ID reply.
 */
static boolean_t
cap_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
    if (cap_server_server(in, out))
        return TRUE;
    return FALSE;
}

/* ============================================================
 * Revocation subscribers (Issue #183).
 *
 * Resource servers (e.g. block_device_server) call cap_subscribe_revoke
 * passing a send right to their notification port.  On every successful
 * cap_revoke we fan out cap_revoke_notify(port, cap_id) to every entry.
 * A send error means the subscriber died — we drop the entry and keep
 * walking.  Single-threaded cap_server, no locking yet.
 * ============================================================ */

#define CAP_MAX_REVOKE_SUBSCRIBERS	8

static mach_port_t revoke_subscribers[CAP_MAX_REVOKE_SUBSCRIBERS];
static int         n_revoke_subscribers = 0;

static int
revoke_subscribers_contains(mach_port_t p)
{
    for (int i = 0; i < n_revoke_subscribers; i++)
        if (revoke_subscribers[i] == p)
            return 1;
    return 0;
}

static void
revoke_subscribers_remove_at(int i)
{
    (void)mach_port_deallocate(mach_task_self(), revoke_subscribers[i]);
    revoke_subscribers[i] = revoke_subscribers[--n_revoke_subscribers];
    revoke_subscribers[n_revoke_subscribers] = MACH_PORT_NULL;
}

/*
 * Fan out a revocation event.  Iterates with index because the array
 * is compacted in place when a subscriber goes away.
 */
static void
fanout_revoke_notify(uint64_t cap_id)
{
    int i = 0;
    while (i < n_revoke_subscribers) {
        kern_return_t kr = cap_revoke_notify(revoke_subscribers[i], cap_id);
        if (kr != KERN_SUCCESS) {
            printf("cap: subscriber 0x%x dropped on revoke notify (kr=%d)\n",
                   (unsigned)revoke_subscribers[i], (int)kr);
            revoke_subscribers_remove_at(i);
            continue;
        }
        i++;
    }
}

/* ============================================================
 * v1 policy (hardcoded, permissive).
 * ============================================================ */

static int
policy_allows_v1(mach_port_t sender,
                 uint32_t    resource_type,
                 uint64_t    ops)
{
    (void)sender;
    (void)ops;

    /* Issue A: we only know how to sign RESOURCE_BLK_DEVICE tokens.
     * Everything else is rejected to keep the surface honest —
     * later issues add the other resource types as servers publish
     * them. */
    if (resource_type != RESOURCE_BLK_DEVICE)
        return 0;

    return 1;
}

/* ============================================================
 * MIG server handlers (called by the generated dispatcher).
 *
 * Signatures must match cap_server.defs:
 *   routine cap_acquire(cap_server, resource_type, resource_id,
 *                       ops, lifetime_ms; out token);
 *   routine cap_derive(cap_server, parent; reduced_ops,
 *                      lifetime_ms; out child);
 *   routine cap_revoke(cap_server, cap_id);
 *   routine cap_verify(cap_server, token, op, resource_id);
 *
 * MIG synthesizes the out/in array lengths as mach_msg_type_number_t.
 * ============================================================ */

kern_return_t
cap_acquire(mach_port_t             server,
            unsigned int            resource_type,
            uint64_t                resource_id,
            uint64_t                ops,
            unsigned int            lifetime_ms,
            char                   *token,
            mach_msg_type_number_t *token_len)
{
    (void)server;

    if (*token_len < sizeof(struct uros_cap))
        return CAP_ERR_NO_MEMORY;

    if (!policy_allows_v1(MACH_PORT_NULL, resource_type, ops))
        return CAP_ERR_POLICY_DENIED;

    struct cap_entry *e = (struct cap_entry *)malloc(sizeof(*e));
    if (!e) return CAP_ERR_NO_MEMORY;
    memset(e, 0, sizeof(*e));

    struct uros_cap *t = &e->token;
    t->cap_id        = cap_table_alloc_id();
    t->resource_type = resource_type;
    t->resource_id   = resource_id;
    t->allowed_ops   = ops;
    t->owner         = MACH_PORT_NULL;  /* audit trailer not consulted in v1 */
    t->owner_name[0] = '\0';
    t->parent_cap_id = 0;
    t->depth         = 0;
    t->delegable     = 1;
    t->expires       = 0;   /* v1: ignore lifetime_ms until clock wiring lands */
    (void)lifetime_ms;
    t->max_uses      = 0;
    t->current_uses  = 0;
    t->revoked       = 0;
    t->is_generic    = 0;
    t->first_child_cap_id = 0;
    t->next_sibling_cap_id = 0;

    cap_sign(t);

    e->sender = MACH_PORT_NULL;
    cap_table_insert(e);

    memcpy(token, t, sizeof(*t));
    *token_len = (mach_msg_type_number_t)sizeof(*t);

    printf("cap: acquired id=%llu rtype=%u rid=%llu ops=0x%llx\n",
           (unsigned long long)t->cap_id, resource_type,
           (unsigned long long)resource_id,
           (unsigned long long)ops);
    return KERN_SUCCESS;
}

kern_return_t
cap_derive(mach_port_t             server,
           char                   *parent_buf,
           mach_msg_type_number_t  parent_len,
           uint64_t                reduced_ops,
           unsigned int            lifetime_ms,
           char                   *child_buf,
           mach_msg_type_number_t *child_len)
{
    (void)server;
    (void)lifetime_ms;

    if (parent_len != sizeof(struct uros_cap))
        return CAP_ERR_INVALID_TOKEN;
    if (*child_len < sizeof(struct uros_cap))
        return CAP_ERR_NO_MEMORY;

    struct uros_cap parent;
    memcpy(&parent, parent_buf, sizeof(parent));

    if (!cap_verify_hmac(&parent)) return CAP_ERR_INVALID_TOKEN;
    if (parent.revoked)            return CAP_ERR_REVOKED;
    if (!parent.delegable)         return CAP_ERR_NOT_DELEGABLE;
    if (parent.depth + 1 > CAP_MAX_DEPTH) return CAP_ERR_DEPTH_EXCEEDED;
    if ((reduced_ops & ~parent.allowed_ops) != 0) return CAP_ERR_OP_NOT_ALLOWED;

    struct cap_entry *parent_entry = cap_table_find_by_id(parent.cap_id);
    if (!parent_entry || parent_entry->token.revoked) return CAP_ERR_REVOKED;

    struct cap_entry *e = (struct cap_entry *)malloc(sizeof(*e));
    if (!e) return CAP_ERR_NO_MEMORY;
    memset(e, 0, sizeof(*e));

    struct uros_cap *t = &e->token;
    *t = parent;                           /* copy identity + rights */
    t->cap_id        = cap_table_alloc_id();
    t->allowed_ops   = reduced_ops;
    t->parent_cap_id = parent.cap_id;
    t->depth         = parent.depth + 1;
    t->first_child_cap_id  = 0;
    t->next_sibling_cap_id = parent_entry->token.first_child_cap_id;

    cap_sign(t);
    e->sender = MACH_PORT_NULL;
    cap_table_insert(e);

    /* Link as new first child of parent (re-sign parent since
     * first_child_cap_id changed). */
    parent_entry->token.first_child_cap_id = t->cap_id;
    cap_sign(&parent_entry->token);

    memcpy(child_buf, t, sizeof(*t));
    *child_len = (mach_msg_type_number_t)sizeof(*t);

    printf("cap: derived id=%llu from parent=%llu ops=0x%llx depth=%u\n",
           (unsigned long long)t->cap_id,
           (unsigned long long)parent.cap_id,
           (unsigned long long)reduced_ops,
           t->depth);
    return KERN_SUCCESS;
}

/*
 * Cascade revocation with an explicit stack (no recursion).  The
 * delegation tree is left-child right-sibling so we walk via
 * first_child_cap_id then next_sibling_cap_id.
 */
kern_return_t
cap_revoke(mach_port_t server, uint64_t cap_id)
{
    (void)server;

    struct cap_entry *root = cap_table_find_by_id(cap_id);
    if (!root) return CAP_ERR_INVALID_TOKEN;

    uint64_t stack[CAP_MAX_DEPTH + 1];
    int top = 0;
    stack[top++] = cap_id;

    int revoked = 0;
    while (top > 0) {
        uint64_t id = stack[--top];
        struct cap_entry *e = cap_table_find_by_id(id);
        if (!e || e->token.revoked) continue;

        cap_table_mark_revoked(e);
        revoked++;

        /* Enqueue every child via the sibling chain.  Siblings are
         * distinct cap_ids so they fit linearly in the stack budget. */
        uint64_t child = e->token.first_child_cap_id;
        while (child != 0 && top <= CAP_MAX_DEPTH) {
            stack[top++] = child;
            struct cap_entry *c = cap_table_find_by_id(child);
            if (!c) break;
            child = c->token.next_sibling_cap_id;
        }
    }

    printf("cap: revoked %d cap(s) in cascade rooted at %llu\n",
           revoked, (unsigned long long)cap_id);

    /*
     * Issue #183: notify every subscribed resource server so they can
     * drop cap-gated state synchronously (e.g. block_device_server
     * marks any blk_handle whose cap_id matches as revoked).  We fire
     * one notification per cap in the cascade so a subscriber that
     * only knows about a specific child can still react.
     */
    if (n_revoke_subscribers > 0 && revoked > 0) {
        /* Re-walk the cascade to collect every revoked id.  Cheap:
         * the set is already marked, so we just enumerate it once. */
        uint64_t stack2[CAP_MAX_DEPTH + 1];
        int top2 = 0;
        stack2[top2++] = cap_id;
        while (top2 > 0) {
            uint64_t id = stack2[--top2];
            struct cap_entry *e = cap_table_find_by_id(id);
            if (!e) continue;
            fanout_revoke_notify(id);

            uint64_t child = e->token.first_child_cap_id;
            while (child != 0 && top2 <= CAP_MAX_DEPTH) {
                stack2[top2++] = child;
                struct cap_entry *c = cap_table_find_by_id(child);
                if (!c) break;
                child = c->token.next_sibling_cap_id;
            }
        }
    }

    return KERN_SUCCESS;
}

/*
 * cap_subscribe_revoke (Issue #183) — register a notify port.  We
 * keep the send right; subsequent cap_revoke calls fan out
 * cap_revoke_notify(notify_port, cap_id) on each subscriber.
 */
kern_return_t
cap_subscribe_revoke(mach_port_t server, mach_port_t notify_port)
{
    (void)server;

    if (notify_port == MACH_PORT_NULL)
        return CAP_ERR_INVALID_TOKEN;

    if (revoke_subscribers_contains(notify_port)) {
        /* Idempotent: already subscribed.  Drop the extra send right
         * we just received so the refcount doesn't drift. */
        (void)mach_port_deallocate(mach_task_self(), notify_port);
        return KERN_SUCCESS;
    }

    if (n_revoke_subscribers >= CAP_MAX_REVOKE_SUBSCRIBERS) {
        printf("cap: subscribe_revoke FAIL — table full (%d)\n",
               n_revoke_subscribers);
        (void)mach_port_deallocate(mach_task_self(), notify_port);
        return CAP_ERR_NO_MEMORY;
    }

    revoke_subscribers[n_revoke_subscribers++] = notify_port;
    printf("cap: subscribe_revoke ok — %d subscriber(s)\n",
           n_revoke_subscribers);
    return KERN_SUCCESS;
}

kern_return_t
cap_verify(mach_port_t             server,
           char                   *token_buf,
           mach_msg_type_number_t  token_len,
           uint64_t                op,
           uint64_t                resource_id)
{
    (void)server;

    if (token_len != sizeof(struct uros_cap)) return CAP_ERR_INVALID_TOKEN;

    struct uros_cap token;
    memcpy(&token, token_buf, sizeof(token));

    if (!cap_verify_hmac(&token))           return CAP_ERR_INVALID_TOKEN;
    if (token.revoked)                      return CAP_ERR_REVOKED;
    if (token.resource_id != resource_id)   return CAP_ERR_RESOURCE_MISMATCH;
    if ((op & ~token.allowed_ops) != 0)     return CAP_ERR_OP_NOT_ALLOWED;

    /* Also consult the live table: a token may have been revoked
     * after issuance but the in-flight copy can still carry revoked=0. */
    struct cap_entry *live = cap_table_find_by_id(token.cap_id);
    if (live && live->token.revoked) return CAP_ERR_REVOKED;

    return KERN_SUCCESS;
}

/* ============================================================
 * Entry point.
 * ============================================================ */

int
main(int argc, char **argv)
{
    kern_return_t kr;

    kr = bootstrap_ports(bootstrap_port,
                         &host_port, &device_port,
                         &root_ledger_wired, &root_ledger_paged,
                         &security_port);
    if (kr != KERN_SUCCESS)
        _exit(1);

    printf_init(device_port);
    panic_init(host_port);

    printf("\n=== cap_server (UrMach capability) ===\n");
    printf("cap: argc=%d\n", argc);
    (void)argv;

    cap_key_init();
    cap_table_init();
    printf("cap: hmac key initialized, table ready\n");

    int ekr = cap_key_export_to_kernel();
    if (ekr != KERN_SUCCESS)
        printf("cap: urmach_cap_register (setup) failed (%d)\n", ekr);
    else
        printf("cap: kernel hmac key registered via urmach_cap_register\n");

    kr = mach_port_allocate(mach_task_self(),
                            MACH_PORT_RIGHT_RECEIVE,
                            &cap_port);
    if (kr != KERN_SUCCESS) {
        printf("cap: mach_port_allocate failed (%d)\n", kr);
        return 1;
    }

    kr = mach_port_insert_right(mach_task_self(),
                                cap_port, cap_port,
                                MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        printf("cap: insert send right failed (%d)\n", kr);
        return 1;
    }

    kr = netname_check_in(name_server_port, "cap_server",
                          MACH_PORT_NULL, cap_port);
    if (kr != KERN_SUCCESS)
        printf("cap: netname_check_in failed (%d)\n", kr);
    else
        printf("cap: registered as \"cap_server\"\n");

    /* #209: cap_server boots before gpu_server in bootstrap.conf
     * (gpu_server cap_subscribe_revoke's against us at startup), so
     * a sync gpu_console_init would just fail.  Use the async
     * variant: ~5 seconds of retries at 100 ms intervals — gpu_server
     * is up well before then.  Once it succeeds, our runtime printfs
     * (cap_revoke logs, policy hits, ...) appear on the VGA console
     * with the [cap] tag.  Startup chatter above stays serial-only.
     */
    (void)gpu_console_init_async("cap", 100u, 50u);

    bootstrap_completed(bootstrap_port, mach_task_self());

    printf("cap: entering message loop\n");
    mach_msg_server(cap_demux, 8192, cap_port, MACH_MSG_OPTION_NONE);

    printf("cap: mach_msg_server exited unexpectedly\n");
    return 1;
}
