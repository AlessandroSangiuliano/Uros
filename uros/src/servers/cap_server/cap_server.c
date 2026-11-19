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
 *   - revocation is cascaded over the whole delegation tree, walked by
 *     its own links with no stack and no bound (#745)
 */

#include <stddef.h>
#include <string.h>

#include <mach.h>
#include <mach/bootstrap.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <mach/notify.h>	/* MACH_NOTIFY_NO_SENDERS (#743) */
#include <mach/cap_types.h>
#include <mach/mach_traps.h>	/* urmach_cap_revoke (#432) */
#include <mach/mach_host.h>	/* urmach_task_port_rights (#938) */
#include <mach/task_rights.h>	/* URMACH_TASK_RIGHTS_IDENTITY (#938) */
#include <mach/clock.h>		/* clock_get_time (#747) */
#include <mach/thread_switch.h>	/* the waiter's pause (#747) */
#include <mach/clock_types.h>	/* URMACH_CAP_CLOCK (#747) */
#include <mach/urmach_identity.h>	/* the requester's identity (#747) */
#include <mach_init.h>
#include <sa_mach.h>
#include <pthread.h>		/* a deadline's waiter (#747) */
#include <stdio.h>
#include <stdlib.h>

#include "cap_table.h"
#include "cap_manifest_table.h"  /* #216 v2.1: per-task manifests */
#include "cap_server_server.h"   /* MIG-generated demux prototype */
#include "cap_revoke.h"          /* MIG-generated cap_revoke_notify user stub */
#include "gpu_console.h"         /* #209: async printf mirror */

#include "service.h"			/* #744: service_checkin */

/*
 * Bootstrap ports (standard for any Mach server).
 */
static mach_port_t host_port;
static mach_port_t device_port;
static mach_port_t security_port;
static mach_port_t root_ledger_wired;
static mach_port_t root_ledger_paged;

/*
 * Our well-known receive port — netname publishes a send right to this.
 * Legacy callers (everyone before #216 v2.1) reach cap_server here and
 * get the permissive v1 policy.
 */
static mach_port_t cap_port;

/*
 * The provisioning port (#743): cap_provision_task answers on this port and
 * on no other.  Its one send right is bootstrap's, given at start-up on this
 * server's own bootstrap port (bootstrap_cap_provisioner()); the port every
 * task inherits and the per-task ports are refused.
 */
static mach_port_t provision_port;

/*
 * Port set that covers cap_port + every per-task cap_port allocated
 * by cap_provision_task.  mach_msg_server receives from the set;
 * the demux uses in->msgh_local_port to identify the caller.
 */
static mach_port_t cap_port_set;

/*
 * #747: the port in the set that a deadline's waiter rings when one comes.
 * No other task holds a right to it; see "A deadline is a revocation at that
 * time" below.
 */
static mach_port_t cap_sweep_port = MACH_PORT_NULL;
static void cap_expiry_sweep(void);

/*
 * Single-threaded snapshot of the receive port the current request
 * arrived on.  Set by cap_demux before dispatching the MIG server,
 * cleared after.  Used by the policy gate in cap_acquire to decide
 * whether to enforce a manifest (per-task port: yes, well-known
 * cap_port: no — legacy / permissive).
 */
static mach_port_t g_request_local_port;

/*
 * #747: and the task that sent it, by the identity the kernel stamped in the
 * request's trailer -- the one a capability is issued to, and the only one
 * that may derive from it.  Set and cleared with g_request_local_port; 0 for a
 * request received without its sender, which owns nothing.
 */
static uint64_t g_request_sender;

/*
 * Dispatcher: snapshot the receive port for the policy gate, then
 * hand off to the MIG-generated cap_server_server.  Anything that
 * doesn't match subsystem 3500 gets a MIG_BAD_ID reply.
 */
static void cap_task_port_gone(mach_port_t port);

static boolean_t
cap_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
    boolean_t handled;

    /*
     * A per-task port with no senders (#743).  Only the kernel can send
     * this: it comes through the send-once right this server made for the
     * request, and a task that holds a per-task port has a send right, not
     * a send-once one, so a message it forges with this id arrives with
     * other bits and goes to MIG, which refuses it.  No reply: the one
     * mach_msg_server() starts with is none.
     */
    if (in->msgh_id == MACH_NOTIFY_NO_SENDERS &&
        in->msgh_bits == MACH_MSGH_BITS(0, MACH_MSG_TYPE_PORT_SEND_ONCE)) {
        cap_task_port_gone(in->msgh_local_port);
        return TRUE;
    }

    /*
     * #747: the waiter's ring -- a deadline has come.  Only this task holds a
     * right to the port, so only the waiter rings it.  No reply.
     */
    if (cap_sweep_port != MACH_PORT_NULL &&
        in->msgh_local_port == cap_sweep_port) {
        cap_expiry_sweep();
        return TRUE;
    }

    g_request_local_port = in->msgh_local_port;
    g_request_sender = urmach_msg_sender(in);		/* #747 */
    handled = cap_server_server(in, out);
    g_request_local_port = MACH_PORT_NULL;
    g_request_sender = 0;
    return handled;
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
        /*
         * #744: a full queue costs that subscriber this notice and nothing
         * else -- it stays subscribed, and it sees the revocation anyway,
         * from the kernel's epoch, at the next use of what was revoked.  The
         * send does not wait (cap_revoke.defs), so this server never does.
         */
        if (kr == MACH_SEND_TIMED_OUT) {
            printf("cap: subscriber 0x%x missed the notice of %llu: its "
                   "queue is full (#744)\n",
                   (unsigned)revoke_subscribers[i], (unsigned long long)cap_id);
            i++;
            continue;
        }
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

    /*
     * v1 is permissive by design: every resource type cap_server can
     * be asked about is acked.  The real (manifest- / user- / quota-
     * aware) policy arrives in a later issue.  Until then, blocking
     * unknown types here breaks the libgpu_console mirror (#199), the
     * char_server cap path (#205-#207), and any future userspace
     * server that wants to gate access through cap_request — they all
     * use their own resource_type tag ('GPU\0', 'CHR\0', ...).
     *
     * RESOURCE_BLK_DEVICE remains the only fully-modeled type in this
     * enum, but tagging-only consumers (mirror caps, presence
     * tokens) pass through.
     */
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
 *   routine cap_verify(cap_server, token, presenter, resource_type, op,
 *                      resource_id);
 *
 * MIG synthesizes the out/in array lengths as mach_msg_type_number_t.
 * ============================================================ */

/*
 * ── A token's deadline (#747) ─────────────────────────────────────────────
 *
 * cap_acquire and cap_derive took a lifetime and threw it away, `(void)
 * lifetime_ms', writing every deadline 0: a caller that asked for 100 ms was
 * told success and given a token that never ended.  The deadline is written on
 * URMACH_CAP_CLOCK, nanoseconds since boot, the clock the kernel checks it
 * against in the trap; its service port is asked for once and kept.
 */
static mach_port_t cap_clock_port = MACH_PORT_NULL;

/* Now on the capability clock; 0 when it cannot be read. */
static int
cap_now_ns(uint64_t *now)
{
    tvalspec_t ts;

    if (cap_clock_port == MACH_PORT_NULL) {
        mach_port_t   host = mach_host_self();
        kern_return_t kr = host_get_clock_service(host, URMACH_CAP_CLOCK,
                                                  &cap_clock_port);

        (void)mach_port_deallocate(mach_task_self(), host);
        if (kr != KERN_SUCCESS) {
            cap_clock_port = MACH_PORT_NULL;
            return 0;
        }
    }
    if (clock_get_time(cap_clock_port, &ts) != KERN_SUCCESS)
        return 0;
    *now = (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
    return 1;
}

/*
 * #747: whether a deadline's waiter is running, and a deadline written, for it
 * -- "A deadline is a revocation at that time", below.
 */
static int  cap_expiry_running;
static void cap_expiry_note(uint64_t due);

/*
 * The deadline of a token asked for `lifetime_ms' from now, and never past
 * `ceiling', its parent's deadline (0: none).  0 is no deadline.  A lifetime
 * asked for when the clock cannot be read is refused, not dropped: -1.  So is
 * one asked for with no waiter running: a deadline revokes what it bought, and
 * nothing would.  Every deadline begins as a lifetime asked for, so without
 * the waiter there is none.
 */
static int
cap_deadline(unsigned int lifetime_ms, uint64_t ceiling, uint64_t *out)
{
    uint64_t now, d;

    if (lifetime_ms == 0) {
        *out = ceiling;
        return 0;
    }
    if (!cap_expiry_running || !cap_now_ns(&now))
        return -1;
    d = now + (uint64_t)lifetime_ms * 1000000ULL;
    *out = (ceiling != 0 && ceiling < d) ? ceiling : d;
    return 0;
}

/* Is the token past its deadline?  One that cannot be read against is. */
static kern_return_t
cap_expired(const struct uros_cap *t)
{
    uint64_t now;

    if (t->expires == 0)
        return KERN_SUCCESS;
    if (!cap_now_ns(&now) || now >= t->expires)
        return CAP_ERR_EXPIRED;
    return KERN_SUCCESS;
}

kern_return_t
cap_acquire(mach_port_t             server,
            unsigned int            resource_type,
            uint64_t                resource_id,
            uint64_t                ops,
            unsigned int            lifetime_ms,
            cap_token_t            token,
            mach_msg_type_number_t *token_len)
{
    (void)server;

    if (*token_len < sizeof(struct uros_cap))
        return CAP_ERR_NO_MEMORY;

    if (!policy_allows_v1(MACH_PORT_NULL, resource_type, ops))
        return CAP_ERR_POLICY_DENIED;

    /*
     * Manifest enforcement (#216 v2.1): if the request arrived on a
     * per-task cap_port (i.e. not the well-known cap_port reachable
     * via netname), look up the task's manifest and refuse the
     * request when type+ops aren't declared.  Legacy well-known
     * callers fall through to the permissive path.  In Phase 3 this
     * branch is dead — no client has been migrated yet, every
     * request still lands on the well-known port.
     */
    /*
     * ── The well-known port does not issue capabilities (#511) ─────────
     *
     * 🔴 IT USED TO, AND THAT WAS THE PERMISSIVE PATH ITSELF.  A request
     * arriving here has no per-task port behind it, so there is no manifest
     * to look up and nothing was consulted about whether the caller may have
     * what it asked for.  Worse, the port is reachable by ANY task that
     * looks CAP_SERVER_NETNAME up in the name server -- authority held by
     * name and not by right, which is the sentence this issue makes about
     * the master device port, in the server that exists to prevent it.
     *
     * 🔑 REMOVED AGAINST A COUNT AND NOT AGAINST A HOPE.  cap_server spent a
     * boot reporting every request the legacy path served: three, from two
     * tasks.  block_device_server was given a manifest, bootstrap was made to
     * provision itself, and the count went to zero.  Deleting a fallback that
     * nothing uses is a different act from deleting one that something might.
     *
     * ⚠️ Acquisition only.  cap_provision_task must still answer here,
     * because bootstrap provisions its children before it has a port of its
     * own -- that call is how per-task ports come to exist, so requiring one
     * to make one would have no first step.
     *
     * 🔴 AND ONLY FOR A MODELLED RESOURCE TYPE, which is not a softening.
     * This RPC is being used for two different things.  One is authority
     * over something the enum in <mach/cap_types.h> names.  The other is an
     * opaque TAG -- libgpu_console asks with 'GPU\0' and char_server with
     * 'CHR\0', using a token as a presence marker for a resource this server
     * models nothing about; policy_allows_v1 says as much a few lines above,
     * and warned that refusing unknown types breaks them.
     *
     * A tag is not a capability, and its travelling through this door is the
     * defect rather than the reason to leave the door open.  Refusing it here
     * would have stopped the i386 console -- measured, not guessed: the
     * blanket refusal took `ush' with it -- while proving nothing about
     * authority, so the taggers keep the legacy path until they stop asking
     * for capabilities they are not using as capabilities.
     *
     * ⚠️ A CENSUS IS OF ONE TARGET.  This branch's count was taken on
     * x86-64, where the tag path does not run, and it read zero; on i386 it
     * is fifteen requests in one boot.  The number was right and the
     * conclusion drawn from it was not.
     */
    /*
     * 🔑 ONE EXEMPTION, IN ONE PLACE.  This was first written as two -- the
     * well-known-port refusal skipped unmodelled types and the manifest rule
     * did not -- so a tag arriving on the legacy path passed the first gate
     * and was refused by the second.  An exemption spelt twice is an
     * exemption that will disagree with itself.
     *
     * ⚠️ The whole authority gate is inside this one condition, so an
     * unmodelled type does not reach any of it.  Written as an early jump
     * first, which this tree does not allow, and the condition is the better
     * shape anyway: it says what the block is for.
     */
    if (resource_type < RESOURCE_TYPE_MAX) {

    /*
     * #744: a permission, not a capability.  The manifest that grants
     * revocation notices says a task may be told of revocations; no token of
     * this type stands for anything any server checks, and issuing one would
     * hand out a signed claim to an authority that does not exist.
     */
    if (resource_type == RESOURCE_REVOKE_NOTICES) {
        printf("cap: DENY rtype=%u -- revocation notices are a permission, "
               "not a capability (#744)\n", resource_type);
        return CAP_ERR_POLICY_DENIED;
    }

    if (g_request_local_port == MACH_PORT_NULL ||
        g_request_local_port == cap_port) {
        printf("cap: DENY (well-known port issues no capabilities) rtype=%u "
               "id=0x%llx ops=0x%llx\n",
               resource_type, (unsigned long long)resource_id,
               (unsigned long long)ops);
        return CAP_ERR_NOT_IN_MANIFEST;
    }

    {
        const cap_manifest_header_t *m =
            cap_manifest_table_get(g_request_local_port);
        /*
         * 🔴 A DMA BUFFER'S INSTANCE IS THE KERNEL'S TO ANSWER, not this
         * server's (#432).  A manifest can name a device class or a mount
         * point because those exist before the policy is written; it cannot
         * name a buffer that will exist for four milliseconds.  So the
         * manifest says whether this task may hold buffer capabilities AT ALL
         * -- checked below with CAP_MANIFEST_ANY_ID, because that is the only
         * thing it can honestly say -- and the kernel says whether THIS buffer
         * is that task's to give away.
         *
         * ⚠️ Neither half is trusted about the other's question, which is what
         * makes it a check rather than a formality: this server cannot know
         * who allocated a region, and the kernel cannot read a manifest.
         */
        if (resource_type == RESOURCE_DMA_BUFFER) {
            mach_port_t owner = cap_manifest_table_task(g_request_local_port);
            kern_return_t okr;

            if (!cap_manifest_allows(m, resource_type,
                                     CAP_MANIFEST_ANY_ID, ops)) {
                printf("cap: DENY (manifest) port=0x%x dma buffer "
                       "ops=0x%llx\n", (unsigned)g_request_local_port,
                       (unsigned long long)ops);
                return CAP_ERR_NOT_IN_MANIFEST;
            }

            okr = urmach_dma_region_owner(resource_id, owner);
            if (okr != KERN_SUCCESS) {
                printf("cap: DENY (not the owner) port=0x%x region=%llu "
                       "kr=%d\n", (unsigned)g_request_local_port,
                       (unsigned long long)resource_id, (int)okr);
                return CAP_ERR_NOT_IN_MANIFEST;
            }
        } else if (!cap_manifest_allows(m, resource_type, resource_id, ops)) {
            printf("cap: DENY (%s) port=0x%x rtype=%u id=0x%llx "
                   "ops=0x%llx\n", m ? "manifest" : "no manifest, and "
                   "hardware needs one",
                   (unsigned)g_request_local_port, resource_type,
                   (unsigned long long)resource_id,
                   (unsigned long long)ops);
            return CAP_ERR_NOT_IN_MANIFEST;
        }
    }

    }	/* resource_type < RESOURCE_TYPE_MAX */

    uint64_t expires;

    /*
     * #747: issued to the task that asked, by the identity the kernel stamped
     * on its request.  The owner was a port name written MACH_PORT_NULL, "audit
     * trailer not consulted in v1", so a token was good for whoever held its
     * bytes.
     */
    if (g_request_sender == 0) {
        printf("cap: DENY port=0x%x: the request came without its sender's "
               "identity, so there is nobody to issue it to (#747)\n",
               (unsigned)g_request_local_port);
        return CAP_ERR_INTERNAL;
    }
    if (cap_deadline(lifetime_ms, 0, &expires) != 0) {
        printf("cap: DENY port=0x%x: a lifetime of %u ms was asked for, and "
               "%s (#747)\n", (unsigned)g_request_local_port, lifetime_ms,
               cap_expiry_running
               ? "the clock it would be kept on cannot be read"
               : "no waiter runs to revoke it at its deadline");
        return CAP_ERR_INTERNAL;
    }

    struct cap_entry *e = (struct cap_entry *)malloc(sizeof(*e));
    if (!e) return CAP_ERR_NO_MEMORY;
    memset(e, 0, sizeof(*e));

    struct uros_cap *t = &e->token;
    t->cap_id        = cap_table_alloc_id();
    t->resource_type = resource_type;
    t->resource_id   = resource_id;
    t->allowed_ops   = ops;
    t->owner         = g_request_sender;	/* #747 */
    t->parent_cap_id = 0;
    t->depth         = 0;
    t->delegable     = 1;
    t->expires       = expires;		/* #747 */
    t->revoked       = 0;
    t->is_generic    = 0;
    t->first_child_cap_id = 0;
    t->next_sibling_cap_id = 0;

    cap_sign(t);

    /*
     * #599: who it was issued to -- the per-task port the request came in
     * on -- so that only that task, or one its capability descends from,
     * may revoke it.  This was always written MACH_PORT_NULL, and any task
     * could revoke any capability.
     */
    e->sender = g_request_local_port;
    cap_table_insert(e);
    cap_expiry_note(t->expires);		/* #747 */

    memcpy(token, t, sizeof(*t));
    *token_len = (mach_msg_type_number_t)sizeof(*t);

    printf("cap: acquired id=%llu rtype=%u rid=%llu ops=0x%llx\n",
           (unsigned long long)t->cap_id, resource_type,
           (unsigned long long)resource_id,
           (unsigned long long)ops);
    return KERN_SUCCESS;
}

/*
 * A child of `parent_buf' with `reduced_ops', issued to `owner' (#747): the
 * requester itself for cap_derive, the task a port names for cap_derive_for.
 */
static kern_return_t
cap_derive_to(cap_token_t            parent_buf,
              mach_msg_type_number_t  parent_len,
              uint64_t                reduced_ops,
              unsigned int            lifetime_ms,
              uint64_t                owner,
              cap_token_t            child_buf,
              mach_msg_type_number_t *child_len)
{
    if (parent_len != sizeof(struct uros_cap))
        return CAP_ERR_INVALID_TOKEN;
    if (*child_len < sizeof(struct uros_cap))
        return CAP_ERR_NO_MEMORY;

    struct uros_cap parent;
    memcpy(&parent, parent_buf, sizeof(parent));

    if (!cap_verify_hmac(&parent)) return CAP_ERR_INVALID_TOKEN;
    /* #747: only the task it was issued to derives from it. */
    if (parent.owner != g_request_sender || g_request_sender == 0) {
        printf("cap: DENY derive from cap %llu: asked by 0x%llx, issued to "
               "0x%llx (#747)\n", (unsigned long long)parent.cap_id,
               (unsigned long long)g_request_sender,
               (unsigned long long)parent.owner);
        return CAP_ERR_NOT_OWNER;
    }
    if (parent.revoked)            return CAP_ERR_REVOKED;
    if (cap_expired(&parent) != KERN_SUCCESS) return CAP_ERR_EXPIRED; /* #747 */
    if (!parent.delegable)         return CAP_ERR_NOT_DELEGABLE;
    if (parent.depth + 1 > CAP_MAX_DEPTH) return CAP_ERR_DEPTH_EXCEEDED;
    if ((reduced_ops & ~parent.allowed_ops) != 0) return CAP_ERR_OP_NOT_ALLOWED;

    struct cap_entry *parent_entry = cap_table_find_by_id(parent.cap_id);
    if (!parent_entry || parent_entry->token.revoked) return CAP_ERR_REVOKED;

    /* #747: its own lifetime, and never past its parent's. */
    uint64_t expires;

    if (cap_deadline(lifetime_ms, parent.expires, &expires) != 0)
        return CAP_ERR_INTERNAL;

    struct cap_entry *e = (struct cap_entry *)malloc(sizeof(*e));
    if (!e) return CAP_ERR_NO_MEMORY;
    memset(e, 0, sizeof(*e));

    struct uros_cap *t = &e->token;
    *t = parent;                           /* copy identity + rights */
    t->cap_id        = cap_table_alloc_id();
    t->allowed_ops   = reduced_ops;
    t->parent_cap_id = parent.cap_id;
    t->depth         = parent.depth + 1;
    t->expires       = expires;		/* #747 */
    t->owner         = owner;		/* #747 */
    t->first_child_cap_id  = 0;
    t->next_sibling_cap_id = parent_entry->token.first_child_cap_id;

    cap_sign(t);
    e->sender = g_request_local_port;	/* #599: see cap_acquire */
    cap_table_insert(e);
    cap_expiry_note(t->expires);		/* #747 */

    /* Link as new first child of parent (re-sign parent since
     * first_child_cap_id changed). */
    parent_entry->token.first_child_cap_id = t->cap_id;
    cap_sign(&parent_entry->token);

    memcpy(child_buf, t, sizeof(*t));
    *child_len = (mach_msg_type_number_t)sizeof(*t);

    printf("cap: derived id=%llu from parent=%llu ops=0x%llx depth=%u "
           "for 0x%llx\n",
           (unsigned long long)t->cap_id,
           (unsigned long long)parent.cap_id,
           (unsigned long long)reduced_ops,
           t->depth, (unsigned long long)owner);
    return KERN_SUCCESS;
}

kern_return_t
cap_derive(mach_port_t             server,
           cap_token_t            parent_buf,
           mach_msg_type_number_t  parent_len,
           uint64_t                reduced_ops,
           unsigned int            lifetime_ms,
           cap_token_t            child_buf,
           mach_msg_type_number_t *child_len)
{
    (void)server;
    return cap_derive_to(parent_buf, parent_len, reduced_ops, lifetime_ms,
                         g_request_sender, child_buf, child_len);
}

/*
 * #747: a child for another task, named by a port it receives on.  The kernel
 * answers whose port it is; a port no task receives on -- the kernel's, one in
 * transit, a dead one -- names nobody to issue the child to.  The request's
 * right to the port is kept by nobody: given back here when the child is made,
 * and with the request by mach_msg_server() when it is refused.
 */
kern_return_t
cap_derive_for(mach_port_t             server,
               cap_token_t            parent_buf,
               mach_msg_type_number_t  parent_len,
               uint64_t                reduced_ops,
               unsigned int            lifetime_ms,
               mach_port_t             delegate,
               cap_token_t            child_buf,
               mach_msg_type_number_t *child_len)
{
    mach_port_t       host = mach_host_self();
    urmach_identity_t id = 0;
    kern_return_t     kr;

    (void)server;
    kr = urmach_port_receiver(host, delegate, &id);
    (void)mach_port_deallocate(mach_task_self(), host);
    if (kr != KERN_SUCCESS) {
        printf("cap: DENY derive_for: no task receives on port 0x%x "
               "(kr=%d) (#747)\n", (unsigned)delegate, (int)kr);
        return CAP_ERR_NO_DELEGATE;
    }
    kr = cap_derive_to(parent_buf, parent_len, reduced_ops, lifetime_ms, id,
                       child_buf, child_len);
    if (kr == KERN_SUCCESS)
        (void)mach_port_deallocate(mach_task_self(), delegate);
    return kr;
}

/*
 * Cascade revocation.  The delegation tree is left-child right-sibling, so it
 * is walked by first_child_cap_id, next_sibling_cap_id and, on the way back
 * up, parent_cap_id (cap_subtree_next() below).
 */
/*
 * #599: may the task asking revoke this capability?  Only the task it was
 * issued to, or one a capability it descends from was issued to -- the
 * delegation tree, walked up by parent_cap_id.  A well-known port or an
 * unknown sender authorises nothing.
 */
static int
cap_revoke_authorised(const struct cap_entry *e)
{
    unsigned int depth = 0;

    if (g_request_local_port == MACH_PORT_NULL ||
        g_request_local_port == cap_port)
        return 0;
    while (e != NULL && depth++ <= CAP_MAX_DEPTH) {
        if (e->sender == g_request_local_port)
            return 1;
        e = e->token.parent_cap_id != 0
          ? cap_table_find_by_id(e->token.parent_cap_id) : NULL;
    }
    return 0;
}

/*
 * The capability after `e' in a pre-order walk of the tree rooted at `root',
 * or NULL once `e' was the last (#745).  The tree's own links are the walk:
 * down to the first child unless told not to, else across to the next sibling,
 * else up to the parent and across from there, never past the root.  So the
 * walk needs no stack and has no bound but the tree's size.  It ran on a stack
 * of CAP_MAX_DEPTH + 1, seventeen entries, and a capability with more than
 * seventeen children waiting lost the rest of them, with their subtrees: their
 * holders kept their authority after a revocation that said it succeeded.
 * CAP_MAX_DEPTH bounds how deep a tree goes, not how wide.
 */
static struct cap_entry *
cap_subtree_next(uint64_t root, struct cap_entry *e, int descend)
{
    if (descend && e->token.first_child_cap_id != 0)
        return cap_table_find_by_id(e->token.first_child_cap_id);
    while (e != NULL && e->token.cap_id != root) {
        if (e->token.next_sibling_cap_id != 0)
            return cap_table_find_by_id(e->token.next_sibling_cap_id);
        e = cap_table_find_by_id(e->token.parent_cap_id);
    }
    return NULL;
}

/*
 * Revoke `root' and everything under it in the tree: each capability marked in
 * this server's table and told to the kernel, which withdraws what it bought.
 * The number revoked now goes in *count, and the kernel's first refusal is the
 * answer; the rest of the tree is revoked all the same (#745).  cap_revoke asks
 * it for a task that may revoke, and a deadline's sweep for itself (#747).
 */
static kern_return_t
cap_revoke_tree(struct cap_entry *root, int *count)
{
    uint64_t cap_id = root->token.cap_id;
    struct cap_entry *e;
    int revoked = 0, descend = 1;
    kern_return_t kr, refused = KERN_SUCCESS;

    for (e = root; e != NULL; e = cap_subtree_next(cap_id, e, descend)) {
        uint64_t id = e->token.cap_id;

        /*
         * One revoked before took its own subtree with it, so the walk goes
         * past it and not into it.
         */
        descend = !e->token.revoked;
        if (!descend)
            continue;

        cap_table_mark_revoked(e);
        revoked++;

        /*
         * 🔴 AND TELL THE KERNEL, WHICH NOBODY WAS DOING.  This server marks
         * its OWN copy of the token; the copy a holder presents is unchanged
         * and still says revoked=0.  urmach_cap_verify falls back to the
         * kernel's revocation table for exactly that reason -- and nothing
         * had ever populated it, so a revoked capability went on verifying.
         *
         * ⚠️ The trap has existed since #216 (syscall_sw.c slot 39) and was
         * declared in <mach/mach_traps.h> beside the three that are used.  A
         * revocation that reaches every subscriber by IPC and never reaches
         * the kernel is a revocation the enforcement point does not hear.
         *
         * 🔑 It is also what withdraws authority that has been MATERIALISED:
         * a capability turned into an IOMMU domain is not consulted again, so
         * the kernel tearing that domain down inside this trap is the only
         * moment the device stops reaching (#432).
         */
        /*
         * #745: and its answer heard.  It was thrown away, so a revocation
         * the kernel refused -- a key not set, or no room in its table --
         * was counted and answered as done, while the trap went on
         * verifying the token.  The rest of the tree is still revoked, and
         * the request is answered with the first refusal.
         */
        kr = urmach_cap_revoke(id);
        if (kr != KERN_SUCCESS) {
            printf("cap: the kernel refused the revocation of cap %llu "
                   "(kr=%d) -- the trap still verifies it (#745)\n",
                   (unsigned long long)id, (int)kr);
            if (refused == KERN_SUCCESS)
                refused = kr;
        }
    }

    *count = revoked;
    return refused;
}

/*
 * Issue #183: notify every subscribed resource server so they can
 * drop cap-gated state synchronously (e.g. block_device_server
 * marks any blk_handle whose cap_id matches as revoked).  We fire
 * one notification per cap in the cascade so a subscriber that
 * only knows about a specific child can still react.  `count' is
 * what cap_revoke_tree() revoked: none, and there is nothing to tell.
 */
static void
cap_revoke_tell(struct cap_entry *root, int count)
{
    struct cap_entry *e;

    if (n_revoke_subscribers > 0 && count > 0) {
        /* The same walk again, every node of the tree now revoked: a
         * notice for each, as there was for each before (#745). */
        for (e = root; e != NULL;
             e = cap_subtree_next(root->token.cap_id, e, 1))
            fanout_revoke_notify(e->token.cap_id);
    }
}

/*
 * ── A deadline is a revocation at that time (#747) ─────────────────────────
 *
 * A token past its deadline is refused when it is presented, by the trap and
 * by this server, and what it had bought before then was kept: a claim made
 * with it, with the interrupts and the register mappings the claim had made,
 * and the pages of a buffer lent with it, in the device's IOMMU domain.  A
 * revocation takes all of that back (#745), and a deadline took none of it.
 * So a capability is revoked when its deadline comes, as cap_revoke revokes
 * it -- the kernel told of it and of its tree, and so withdrawing what they
 * bought, and the subscribers given their notices -- by this server, which
 * writes every deadline and so knows each one.
 *
 * The table is the receive loop's, and the waiting is a thread's of its own:
 * all they share is cap_expiry_due, under cap_expiry_lock.  The loop lowers it
 * when it writes a deadline earlier than it, and after a sweep sets it to the
 * earliest one left, and rings cap_expiry_port either way, so that the waiter
 * looks again.  The waiter waits on that port until the deadline comes, then
 * rings cap_sweep_port, which is in the loop's port set, every
 * CAP_EXPIRY_AGAIN_MS until a sweep has moved the deadline: a ring lost is
 * rung again, and one that finds a queue full finds a ring already there.
 *
 * ⚠️ The revocation follows the deadline by a wake-up and a message.  The
 * deadline itself holds at the instant it passes, by the clock, in the trap's
 * checks and this server's; what the revocation adds is what those cannot
 * do, taking back what was bought.
 */
#define CAP_EXPIRY_RING_ID	7470	/* a ring's id, for whoever reads one */
#define CAP_EXPIRY_AGAIN_MS	100	/* a ring unanswered, or a clock unread */
#define CAP_EXPIRY_LONGEST_MS	3600000	/* a far deadline, looked at hourly */

static pthread_mutex_t cap_expiry_lock;	/* made ready in main() */
static uint64_t        cap_expiry_due;		/* the next deadline; 0: none */
static mach_port_t     cap_expiry_port = MACH_PORT_NULL;	/* the waiter's */

/* An empty message to a port this task receives on.  It never waits. */
static void
cap_expiry_ring(mach_port_t port)
{
    mach_msg_header_t h;

    memset(&h, 0, sizeof(h));
    h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    h.msgh_size = sizeof(h);
    h.msgh_remote_port = port;
    h.msgh_id = CAP_EXPIRY_RING_ID;
    /* Unsent: the waiter rings again, or a ring is already queued. */
    (void)mach_msg(&h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(h), 0,
                   MACH_PORT_NULL, 0, MACH_PORT_NULL);
}

/* A deadline written: the waiter is told of it if it is the earliest. */
static void
cap_expiry_note(uint64_t due)
{
    int earlier = 0;

    if (due == 0)
        return;
    pthread_mutex_lock(&cap_expiry_lock);
    if (cap_expiry_due == 0 || due < cap_expiry_due) {
        cap_expiry_due = due;
        earlier = 1;
    }
    pthread_mutex_unlock(&cap_expiry_lock);
    if (earlier)
        cap_expiry_ring(cap_expiry_port);
}

/* The waiter, a thread of its own: it rings the loop when a deadline comes. */
static void *
cap_expiry_waiter(void *arg)
{
    struct {
        mach_msg_header_t           h;
        mach_msg_format_0_trailer_t t;
    } m;
    mach_msg_option_t  opt;
    mach_msg_timeout_t ms;
    mach_msg_return_t  mr;
    uint64_t           due, now, wait;

    (void)arg;
    for (;;) {
        pthread_mutex_lock(&cap_expiry_lock);
        due = cap_expiry_due;
        pthread_mutex_unlock(&cap_expiry_lock);

        opt = MACH_RCV_MSG;
        ms = MACH_MSG_TIMEOUT_NONE;
        if (due != 0) {
            opt |= MACH_RCV_TIMEOUT;
            ms = CAP_EXPIRY_AGAIN_MS;
            if (cap_now_ns(&now)) {
                if (now >= due)
                    cap_expiry_ring(cap_sweep_port);
                else {
                    wait = (due - now + 999999ULL) / 1000000ULL;
                    ms = wait < CAP_EXPIRY_LONGEST_MS
                       ? (mach_msg_timeout_t)wait : CAP_EXPIRY_LONGEST_MS;
                }
            }
        }
        mr = mach_msg(&m.h, opt, 0, sizeof(m), cap_expiry_port, ms,
                      MACH_PORT_NULL);
        if (mr != MACH_MSG_SUCCESS && mr != MACH_RCV_TIMED_OUT) {
            printf("cap: the deadlines' waiter could not receive (0x%x); it "
                   "tries again in %u ms (#747)\n", (unsigned)mr,
                   (unsigned)CAP_EXPIRY_AGAIN_MS);
            (void)thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT,
                                CAP_EXPIRY_AGAIN_MS);
        }
    }
    return NULL;
}

/*
 * The waiter rang: every capability past its deadline is revoked, with its
 * tree, and the waiter is given the earliest deadline left.  A tree's
 * deadlines are never past its root's, so its capabilities have all come to
 * theirs too.
 */
static void
cap_expiry_sweep(void)
{
    struct cap_entry *e;
    uint64_t          now, next = 0;
    int               n;

    if (!cap_now_ns(&now)) {
        printf("cap: the capability clock could not be read, so no deadline "
               "was swept; the waiter rings again in %u ms (#747)\n",
               (unsigned)CAP_EXPIRY_AGAIN_MS);
        return;
    }
    for (e = cap_table_next(NULL); e != NULL; e = cap_table_next(e)) {
        if (e->token.revoked || e->token.expires == 0)
            continue;
        if (e->token.expires > now) {
            if (next == 0 || e->token.expires < next)
                next = e->token.expires;
            continue;
        }
        /* A refusal by the kernel is said by cap_revoke_tree(). */
        (void)cap_revoke_tree(e, &n);
        printf("cap: cap %llu reached its deadline %llu ms ago and was "
               "revoked, %d cap(s) with its tree (#747)\n",
               (unsigned long long)e->token.cap_id,
               (unsigned long long)((now - e->token.expires) / 1000000ULL),
               n);
        cap_revoke_tell(e, n);
    }
    pthread_mutex_lock(&cap_expiry_lock);
    cap_expiry_due = next;
    pthread_mutex_unlock(&cap_expiry_lock);
    cap_expiry_ring(cap_expiry_port);
}

kern_return_t
cap_revoke(mach_port_t server, uint64_t cap_id)
{
    (void)server;

    struct cap_entry *root = cap_table_find_by_id(cap_id);
    if (!root) return CAP_ERR_INVALID_TOKEN;

    /*
     * #599: the revocation needed no authority at all, so any task could
     * take any other's capability away -- ext_server's page-cache buffer,
     * a driver's claim on its device.
     */
    if (!cap_revoke_authorised(root)) {
        printf("cap: DENY revoke of cap %llu from port=0x%x — not the task "
               "it was issued to, nor one it descends from\n",
               (unsigned long long)cap_id, (unsigned)g_request_local_port);
        return CAP_ERR_UNAUTHORIZED;
    }

    int revoked;
    kern_return_t refused = cap_revoke_tree(root, &revoked);

    printf("cap: revoked %d cap(s) in cascade rooted at %llu\n",
           revoked, (unsigned long long)cap_id);
    cap_revoke_tell(root, revoked);
    return refused;
}

/*
 * cap_subscribe_revoke (Issue #183) — register a notify port.  We
 * keep the send right; subsequent cap_revoke calls fan out
 * cap_revoke_notify(notify_port, cap_id) on each subscriber.
 *
 * 🔴 ONLY A TASK WHOSE MANIFEST SAYS SO, ON ITS OWN PORT (#744).  It took
 * anyone, into one of eight slots: a task could hear of every revocation, and
 * one that filled the slots first left the block, char and gpu servers
 * hearing of none.  The permission is RESOURCE_REVOKE_NOTICES with
 * CAP_OP_REVOKE_SUBSCRIBE, which their manifests grant, and the request must
 * arrive on the per-task port bootstrap provisioned -- the well-known port
 * subscribes nobody, as it issues nothing.
 *
 * ⚠️ A refusal returns an error and leaves the send right alone, because
 * mach_msg_server() destroys a request answered with an error, rights and
 * all.  The table-full refusal used to deallocate it too, which dropped two
 * references for the one received -- the second one the subscriber's, when
 * this server already held a right to the port.
 */
kern_return_t
cap_subscribe_revoke(mach_port_t server, mach_port_t notify_port)
{
    (void)server;

    if (notify_port == MACH_PORT_NULL)
        return CAP_ERR_INVALID_TOKEN;

    if (g_request_local_port == MACH_PORT_NULL ||
        g_request_local_port == cap_port ||
        !cap_manifest_allows(cap_manifest_table_get(g_request_local_port),
                             RESOURCE_REVOKE_NOTICES, CAP_MANIFEST_ANY_ID,
                             CAP_OP_REVOKE_SUBSCRIBE)) {
        printf("cap: DENY subscribe_revoke on port 0x%x -- no manifest grants "
               "it (#744)\n", (unsigned)g_request_local_port);
        return CAP_ERR_NOT_IN_MANIFEST;
    }

    if (revoke_subscribers_contains(notify_port)) {
        /* Idempotent: already subscribed.  Drop the extra send right
         * we just received so the refcount doesn't drift. */
        (void)mach_port_deallocate(mach_task_self(), notify_port);
        return KERN_SUCCESS;
    }

    if (n_revoke_subscribers >= CAP_MAX_REVOKE_SUBSCRIBERS) {
        printf("cap: subscribe_revoke FAIL — table full (%d)\n",
               n_revoke_subscribers);
        return CAP_ERR_NO_MEMORY;
    }

    revoke_subscribers[n_revoke_subscribers++] = notify_port;
    printf("cap: subscribe_revoke ok — %d subscriber(s)\n",
           n_revoke_subscribers);
    return KERN_SUCCESS;
}

/*
 * cap_task_port_make -- a fresh per-task port for `task_port', with `blob' as
 * its manifest: a receive right in cap_port_set, so that requests on it land
 * in our loop, with the manifest in the per-task table under its name.
 *
 * 🔑 IT ASKS TO BE TOLD WHEN THE PORT HAS NO SENDERS, and that is how this
 * server learns the task is gone (#743).  The task's TASK_CAP_PORT and its
 * copies are the only send rights: cap_server keeps none (the reply makes the
 * caller's from the receive right), and the provisioner gives its own back.
 * Before, the table's 64 slots were taken one per task for the life of the
 * system, and so were the task ports kept in them.
 *
 * ⚠️ sync 1, because the port has no sender yet: with 0 the notification
 * would come at once.  The reply's make-send is the first.
 *
 * On an error the request that brought `task_port' is destroyed by
 * mach_msg_server(), which releases it: so is nothing here.
 *
 * `parent' is the per-task port a derivation was asked on, or MACH_PORT_NULL
 * for a task bootstrap provisions: the table bounds derived slots (#937).
 */
static kern_return_t
cap_task_port_make(mach_port_t task_port, mach_port_t parent,
                   const void *blob, uint32_t len, mach_port_t *out_port)
{
    mach_port_t new_port = MACH_PORT_NULL;
    mach_port_t previous = MACH_PORT_NULL;
    kern_return_t kr;
    int vrc;

    kr = mach_port_allocate(mach_task_self(),
                            MACH_PORT_RIGHT_RECEIVE, &new_port);
    if (kr != KERN_SUCCESS) {
        printf("cap: per-task port — port_allocate kr=%d\n", kr);
        return CAP_ERR_NO_MEMORY;
    }

    kr = mach_port_move_member(mach_task_self(), new_port, cap_port_set);
    if (kr != KERN_SUCCESS) {
        printf("cap: per-task port — move_member kr=%d\n", kr);
        (void)mach_port_destroy(mach_task_self(), new_port);
        return CAP_ERR_INTERNAL;
    }

    kr = mach_port_request_notification(mach_task_self(), new_port,
                                        MACH_NOTIFY_NO_SENDERS, 1, new_port,
                                        MACH_MSG_TYPE_MAKE_SEND_ONCE,
                                        &previous);
    if (kr != KERN_SUCCESS) {
        printf("cap: per-task port — no-senders request kr=%d\n", kr);
        (void)mach_port_destroy(mach_task_self(), new_port);
        return CAP_ERR_INTERNAL;
    }

    vrc = cap_manifest_table_install(new_port, task_port, parent, blob, len);
    if (vrc != CAP_ERR_NONE) {
        (void)mach_port_destroy(mach_task_self(), new_port);
        return vrc;
    }

    *out_port = new_port;
    return KERN_SUCCESS;
}

/*
 * #938: is `task_port' a task's identity port, and nothing more?  This server
 * keeps the port it is handed with the slot, for as long as the per-task port
 * lives, and needs it only to tell which task a DMA region belongs to
 * (urmach_dma_region_owner(), which takes any port on the task).  It kept the
 * full task port of every task with a manifest: the task's memory, its
 * threads and its port rights.  A full port is refused, and so is a reduced
 * port with rights, and a port that is no task's; libcap hands over the
 * identity port.  The kernel is asked through the host, as proc_server asks
 * it (#927): a request sent to a port that is no task's would wait for a
 * reply that never comes.
 */
static int
is_an_identity_port(mach_port_t task_port)
{
    mach_port_t   host = mach_host_self();
    natural_t     rights = URMACH_TASK_RIGHTS_FULL;
    kern_return_t kr;

    kr = urmach_task_port_rights(host, task_port, &rights);
    (void)mach_port_deallocate(mach_task_self(), host);
    return kr == KERN_SUCCESS && rights == URMACH_TASK_RIGHTS_IDENTITY;
}

/*
 * cap_task_port_gone -- the kernel says `port' has no senders: the task it was
 * made for is gone, and so is every copy it handed out (#743).  The slot, the
 * task port it kept and the receive right are given back.
 */
static void
cap_task_port_gone(mach_port_t port)
{
    mach_port_t task;

    if (port == cap_port || cap_manifest_table_get(port) == NULL) {
        printf("cap: no senders on 0x%x, which is no task's port\n",
               (unsigned)port);
        return;
    }

    task = cap_manifest_table_remove(port);
    (void)mach_port_destroy(mach_task_self(), port);
    if (MACH_PORT_VALID(task))
        (void)mach_port_deallocate(mach_task_self(), task);
    printf("cap: cap_port=0x%x has no senders — its slot and its task's "
           "port are given back (#743)\n", (unsigned)port);
}

/*
 * cap_provision_task (#216 v2.1).  Validate the manifest and make a per-task
 * port for `task_port' with it.  The caller -- bootstrap, and nobody else --
 * stamps the port as the child's TASK_CAP_PORT and gives its own copy back.
 *
 * 🔴 IT ANSWERED ANYONE (#743).  `server' was discarded, the port every task
 * inherits took the request as well as a per-task port did, and the manifest
 * was whatever the caller sent: a task could provision itself with a manifest
 * of its own writing, take the per-task port it got back, and acquire through
 * it whatever that manifest said.  The manifests bound only the tasks that
 * did not try.
 *
 * 🔑 Now the port the request arrived on decides, and only the provisioning
 * port is answered.  Its one send right is bootstrap's: it is made here and
 * handed over on this server's own bootstrap port, which bootstrap knows to be
 * the cap_server it started, before bootstrap provisions anything.  A task
 * exec'd later gets its port by derivation from its caller's, which can only
 * narrow what the caller holds (cap_derive_task).
 */
kern_return_t
cap_provision_task(mach_port_t            server,
                   mach_port_t            task_port,
                   cap_manifest_blob_t   manifest_buf,
                   mach_msg_type_number_t manifest_len,
                   mach_port_t           *out_task_cap_port)
{
    mach_port_t new_port = MACH_PORT_NULL;
    kern_return_t kr;
    int vrc;

    (void)server;
    *out_task_cap_port = MACH_PORT_NULL;

    if (provision_port == MACH_PORT_NULL ||
        g_request_local_port != provision_port) {
        printf("cap: DENY provision_task on port 0x%x — only bootstrap's "
               "provisioning port provisions (#743)\n",
               (unsigned)g_request_local_port);
        return CAP_ERR_NOT_PROVISIONER;
    }

    if (!is_an_identity_port(task_port)) {
        printf("cap: DENY provision_task — the task's port carries more than "
               "its identity (#938)\n");
        return CAP_ERR_TASK_NOT_IDENTITY;
    }

    vrc = cap_manifest_validate(manifest_buf, (uint32_t)manifest_len, NULL);
    if (vrc != CAP_ERR_NONE) {
        printf("cap: provision_task — manifest invalid (rc=%d, len=%u)\n",
               vrc, (unsigned)manifest_len);
        return vrc;
    }

    kr = cap_task_port_make(task_port, MACH_PORT_NULL, manifest_buf,
                            (uint32_t)manifest_len, &new_port);
    if (kr != KERN_SUCCESS)
        return kr;

    printf("cap: provisioned task — cap_port=0x%x manifest=%u bytes\n",
           (unsigned)new_port, (unsigned)manifest_len);

    /*
     * The reply MAKES the caller's send right from our receive right
     * (mach_port_make_send_t, #743).  This server used to make one for
     * itself and send a copy, and the one it kept meant no per-task port
     * could ever have no senders: the slot of a task long dead stayed
     * taken.
     */
    *out_task_cap_port = new_port;
    return KERN_SUCCESS;
}

/*
 * cap_derive_task (#743).  A per-task port for a child of the task whose
 * per-task port the request arrived on: what the child's manifest asks for,
 * cut to what the parent's may hand on (cap_manifest_derive()).
 *
 * 🔑 THE POLICY COMES FROM THE PARENT.  exec_server used to provision every
 * program from the manifest in the program's own file, so whoever wrote a
 * binary that something exec'd wrote its policy too.  A derivation can only
 * narrow what the parent holds, and a parent with no manifest -- a request on
 * the port every task inherits, or on the provisioning port -- has nothing
 * to hand on.
 *
 * The child's blob is copied before it is read: it arrives in the request
 * buffer, and its 64-bit fields are read where malloc() puts them.
 */
kern_return_t
cap_derive_task(mach_port_t            server,
                mach_port_t            task_port,
                cap_manifest_blob_t   manifest_buf,
                mach_msg_type_number_t manifest_len,
                mach_port_t           *out_task_cap_port)
{
    const cap_manifest_header_t *parent;
    const cap_manifest_header_t *asked;
    void *copy = NULL;
    void *derived = NULL;
    uint32_t derived_len = 0;
    mach_port_t new_port = MACH_PORT_NULL;
    kern_return_t kr;
    int vrc;

    (void)server;
    *out_task_cap_port = MACH_PORT_NULL;

    parent = cap_manifest_table_get(g_request_local_port);
    if (parent == NULL) {
        printf("cap: DENY derive_task on port 0x%x — no task's port, so "
               "nothing to hand on (#743)\n", (unsigned)g_request_local_port);
        return CAP_ERR_NOT_DELEGABLE;
    }

    if (!is_an_identity_port(task_port)) {
        printf("cap: DENY derive_task on 0x%x — the child's port carries "
               "more than its identity (#938)\n",
               (unsigned)g_request_local_port);
        return CAP_ERR_TASK_NOT_IDENTITY;
    }

    if (manifest_len == 0 || manifest_len > CAP_MANIFEST_MAX_BYTES)
        return CAP_ERR_MANIFEST_INVALID;
    copy = malloc(manifest_len);
    if (copy == NULL)
        return CAP_ERR_NO_MEMORY;
    memcpy(copy, manifest_buf, manifest_len);

    vrc = cap_manifest_validate(copy, (uint32_t)manifest_len, &asked);
    if (vrc == CAP_ERR_NONE)
        vrc = cap_manifest_derive(parent, asked, &derived, &derived_len);
    if (vrc != CAP_ERR_NONE) {
        printf("cap: derive_task — manifest refused (rc=%d, len=%u)\n",
               vrc, (unsigned)manifest_len);
        free(copy);
        return vrc;
    }

    /*
     * 🔑 A DERIVATION THAT KEEPS NOTHING TAKES NO SLOT (#937).  Its port would
     * grant nothing, as no port does (#511), and it held a slot for as long as
     * its task lived: every program a caller with nothing to hand on exec'd
     * took one, and any caller could take as many as it asked for.  The
     * program runs with no per-task port, as one whose caller has none.
     */
    if (((const cap_manifest_header_t *)derived)->required_count == 0 &&
        ((const cap_manifest_header_t *)derived)->delegatable_count == 0) {
        printf("cap: derive_task on 0x%x keeps nothing of %u entries — no "
               "port and no slot (#937)\n", (unsigned)g_request_local_port,
               (unsigned)(asked->required_count + asked->delegatable_count));
        free(derived);
        free(copy);
        return CAP_ERR_NOTHING_TO_DERIVE;
    }

    kr = cap_task_port_make(task_port, g_request_local_port, derived,
                            derived_len, &new_port);
    if (kr == KERN_SUCCESS)
        printf("cap: derived task — cap_port=0x%x from 0x%x, %u of %u "
               "entries kept (#743)\n", (unsigned)new_port,
               (unsigned)g_request_local_port,
               (unsigned)(((const cap_manifest_header_t *)derived)
                              ->required_count +
                          ((const cap_manifest_header_t *)derived)
                              ->delegatable_count),
               (unsigned)(asked->required_count + asked->delegatable_count));
    else if (kr == CAP_ERR_DERIVED_QUOTA || kr == CAP_ERR_LINEAGE_FULL)
        printf("cap: DENY derive_task on 0x%x — %u derived of a quota of %u, "
               "%u in its family of at most %u (#937)\n",
               (unsigned)g_request_local_port, cap_manifest_table_derived(),
               cap_manifest_table_derived_quota(),
               cap_manifest_table_family(g_request_local_port),
               cap_manifest_table_family_max());
    free(derived);
    free(copy);
    if (kr != KERN_SUCCESS)
        return kr;

    *out_task_cap_port = new_port;
    return KERN_SUCCESS;
}

/*
 * cap_slots (#743).  The table's census, on any port: a count of tasks says
 * nothing about any one of them, and it is the only way a test can see a slot
 * come back.  With the bounds on derived slots (#937), and the family count
 * of the port it was asked on.
 */
kern_return_t
cap_slots(mach_port_t server, natural_t *in_use, natural_t *size,
          natural_t *derived, natural_t *quota, natural_t *family,
          natural_t *family_max)
{
    (void)server;
    *in_use     = (natural_t)cap_manifest_table_in_use();
    *size       = (natural_t)cap_manifest_table_size();
    *derived    = (natural_t)cap_manifest_table_derived();
    *quota      = (natural_t)cap_manifest_table_derived_quota();
    *family     = (natural_t)cap_manifest_table_family(g_request_local_port);
    *family_max = (natural_t)cap_manifest_table_family_max();
    return KERN_SUCCESS;
}

kern_return_t
cap_verify(mach_port_t             server,
           cap_token_t            token_buf,
           mach_msg_type_number_t  token_len,
           uint64_t                presenter,
           uint32_t                resource_type,
           uint64_t                op,
           uint64_t                resource_id)
{
    (void)server;

    if (token_len != sizeof(struct uros_cap)) return CAP_ERR_INVALID_TOKEN;

    struct uros_cap token;
    memcpy(&token, token_buf, sizeof(token));

    if (!cap_verify_hmac(&token))           return CAP_ERR_INVALID_TOKEN;
    if (token.revoked)                      return CAP_ERR_REVOKED;
    if (cap_expired(&token) != KERN_SUCCESS) return CAP_ERR_EXPIRED; /* #747 */
    /*
     * #747: and presented by the task it was issued to.  The asker is the
     * server verifying, so it says who presented the token, as it does to the
     * trap: the identity in the trailer of the request that carried it.
     */
    if (presenter == 0 || token.owner != presenter) return CAP_ERR_NOT_OWNER;
    /* #745: the kind as well as the id, as the trap has since #599. */
    if (token.resource_type != resource_type) return CAP_ERR_RESOURCE_MISMATCH;
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
    const char *key_source;

    kr = bootstrap_ports(bootstrap_port,
                         &host_port, &device_port,
                         &root_ledger_wired, &root_ledger_paged,
                         &security_port);
    if (kr != KERN_SUCCESS)
        _exit(1);

    printf_init(device_port);
    /*
     * 🔴 AND GIVEN BACK (#511).  That port is bus authority -- configuration
     * space, MMIO, DMA, interrupts -- and this task wanted it to open a
     * console.  See the note above printf_init() in libmach/printf.c.
     */
    (void) mach_port_deallocate(mach_task_self(), device_port);
    panic_init(host_port);

    printf("\n=== cap_server (UrMach capability) ===\n");
    printf("cap: argc=%d\n", argc);
    (void)argv;

    key_source = cap_key_init();
    cap_table_init();
    cap_manifest_table_init();
    if (key_source != NULL)
        printf("cap: hmac key from %s, table ready (#744)\n", key_source);
    else
        printf("cap: hmac key from the TSC alone -- this processor has no "
               "RDSEED or RDRAND that answers, so the key can be guessed "
               "(#744)\n");

    int ekr = cap_key_export_to_kernel();
    if (ekr != KERN_SUCCESS)
        printf("cap: urmach_cap_register (setup) failed (%d)\n", ekr);
    else
        printf("cap: kernel hmac key registered via urmach_cap_register\n");

    /*
     * 🔴 THE PORT IS BOOTSTRAP'S TO GIVE (#744).  It used to be a port of
     * this server's making, published under a name that any task could check
     * in first -- and six start before this one.  Bootstrap makes the port
     * with the other registered ports, every task inherits it in
     * CAP_SERVER_SLOT, the name server publishes it as "cap_server" before
     * anyone can ask, and the receive right is bootstrap's to hand over, on
     * this server's own bootstrap port, to cap_server and nobody else.
     * Requests sent before this point wait in its queue.
     */
    kr = service_checkin(bootstrap_port, urmach_cap_server_port, &cap_port);
    if (kr != KERN_SUCCESS) {
        printf("cap: service_checkin of CAP_SERVER_SLOT failed (%d)\n", kr);
        return 1;
    }

    kr = mach_port_insert_right(mach_task_self(),
                                cap_port, cap_port,
                                MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        printf("cap: insert send right failed (%d)\n", kr);
        return 1;
    }

    /*
     * Port set covering the well-known cap_port plus every per-task
     * cap_port allocated by cap_provision_task (#216 v2.1).  The
     * receive loop services the set; the demux uses msgh_local_port
     * to identify which task is calling.
     */
    kr = mach_port_allocate(mach_task_self(),
                            MACH_PORT_RIGHT_PORT_SET, &cap_port_set);
    if (kr != KERN_SUCCESS) {
        printf("cap: port_set allocate failed (%d)\n", kr);
        return 1;
    }
    kr = mach_port_move_member(mach_task_self(), cap_port, cap_port_set);
    if (kr != KERN_SUCCESS) {
        printf("cap: move_member(cap_port) failed (%d)\n", kr);
        return 1;
    }

    printf("cap: serving CAP_SERVER_SLOT's port 0x%x, which the name server "
           "publishes as \"cap_server\" (#744)\n", (unsigned)cap_port);

    /*
     * The provisioning port (#743), and bootstrap's one send right to it,
     * given on this server's own bootstrap port so that bootstrap knows the
     * cap_server it started gave it.  bootstrap provisions nothing, itself
     * included, until it has it; a request it sends before the loop below
     * starts waits in the port's queue.
     *
     * ⚠️ Without it nothing can be provisioned, and bootstrap would wait for
     * it as long as this task lives: so this task ends, and bootstrap, told
     * of the death, runs the rest without manifests and says so.
     */
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                            &provision_port);
    if (kr == KERN_SUCCESS)
        kr = mach_port_move_member(mach_task_self(), provision_port,
                                   cap_port_set);
    if (kr == KERN_SUCCESS)
        kr = bootstrap_cap_provisioner(bootstrap_port, provision_port);
    if (kr != KERN_SUCCESS) {
        printf("cap: the provisioning port did not reach bootstrap (%d) — "
               "nothing could be provisioned, so cap_server ends (#743)\n",
               kr);
        return 1;
    }
    printf("cap: provisioning port 0x%x given to bootstrap, the only task "
           "that provisions (#743)\n", (unsigned)provision_port);

    /* #209: cap_server boots before gpu_server in bootstrap.conf
     * (gpu_server cap_subscribe_revoke's against us at startup), so
     * a sync gpu_console_init would just fail.  Use the async
     * variant: ~5 seconds of retries at 100 ms intervals — gpu_server
     * is up well before then.  Once it succeeds, our runtime printfs
     * (cap_revoke logs, policy hits, ...) appear on the VGA console
     * with the [cap] tag.  Startup chatter above stays serial-only.
     */
    (void)gpu_console_init_async("cap", 100u, 50u);

    /*
     * #747: the deadlines' waiter, and the port it rings, in the set.  The
     * clock's port is asked for first, here, so that the two threads never
     * both ask for it.  Without the waiter a lifetime is refused, since
     * nothing would revoke a capability at its deadline: cap_deadline().
     */
    {
        pthread_t waiter;
        uint64_t  now;

        kr = cap_now_ns(&now) ? KERN_SUCCESS : KERN_FAILURE;
        if (kr == KERN_SUCCESS &&
            pthread_mutex_init(&cap_expiry_lock, NULL) != 0)
            kr = KERN_FAILURE;
        if (kr == KERN_SUCCESS)
            kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                                    &cap_sweep_port);
        if (kr == KERN_SUCCESS)
            kr = mach_port_move_member(mach_task_self(), cap_sweep_port,
                                       cap_port_set);
        if (kr == KERN_SUCCESS)
            kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                                    &cap_expiry_port);
        if (kr == KERN_SUCCESS &&
            pthread_create(&waiter, NULL, cap_expiry_waiter, NULL) != 0)
            kr = KERN_FAILURE;
        if (kr == KERN_SUCCESS) {
            cap_expiry_running = 1;
            printf("cap: the deadlines' waiter runs: a capability is revoked "
                   "when its deadline comes (#747)\n");
        } else
            printf("cap: the deadlines' waiter did not start (%d), so a "
                   "lifetime is refused (#747)\n", kr);
    }

    bootstrap_completed(bootstrap_port, mach_task_self());

    printf("cap: entering message loop\n");

    /*
     * A LIMIT REMOVED, AND NOT A DEFECT OBSERVED — the distinction is the
     * point of writing this down.
     *
     * cap_provision_task carries the manifest as an INLINE array bounded at
     * CAP_MANIFEST_MAX_BYTES, so a blob near that bound makes a message near
     * sixteen kilobytes, against a receive buffer of eight.  Such a message
     * cannot be received, no reply is ever sent, and the caller blocks
     * forever.  Every manifest this tree ships is under a hundred bytes, so
     * nothing has ever come close.
     *
     * 🔥 AND THAT IS A CORRECTION.  This was first written as the diagnosis of
     * a hang -- bootstrap stopped after cap_provision_task, with every task it
     * had already started still running -- and the hang went away when this
     * changed.  It was not this: putting 8192 back leaves provisioning working
     * and the manifest enforced.  MIG sends the ACTUAL length of a
     * variable-length inline array, not its maximum; the sixteen kilobytes are
     * the stack frame of the generated stub, not the message.  The hang has
     * the shape of #522 -- one task blocked while the rest of the system
     * continues -- and no cause established here.
     *
     * 🔑 Derived and not guessed: the biggest thing the interface can carry,
     * plus room for the header, the descriptors and the trailer.  A number
     * chosen by looking at today's messages stops being right the next time
     * one grows.
     */
    mach_msg_server(cap_demux, CAP_MANIFEST_MAX_BYTES + 2048, cap_port_set,
                    URMACH_RCV_SENDER);		/* #747: who asks */

    printf("cap: mach_msg_server exited unexpectedly\n");
    return 1;
}
