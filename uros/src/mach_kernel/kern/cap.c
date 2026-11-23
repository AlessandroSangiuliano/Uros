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
 * cap.c — UrMach capability subsystem.
 *
 * Globals (all protected by cap_lock):
 *   cap_hmac_key[32]    HMAC-SHA256 key, installed by cap_server at boot
 *   cap_key_set         TRUE once a key has been installed
 *   cap_server_task     the task bootstrap started as cap_server, named by
 *                       urmach_cap_server_set() before it ran, with a
 *                       reference held; only it may install or refresh the
 *                       key or revoke caps (#744 -- it used to be whichever
 *                       task installed a key first)
 *   cap_state_table[]   hash set of cap_ids that are revoked; #974: those
 *                       of them the kernel may forget when a deadline comes
 *                       are also in a list by deadline (cap_dated_first).
 *
 * Threading: a single global simple_lock guards the whole subsystem.
 * Suitable for v1 where cap_server is the only writer and verify is
 * uncontended; a sharded table with per-bucket locks is tracked for
 * the SMP/slab issue (#80).
 */

#include <mach/boolean.h>
#include <mach/kern_return.h>
#include <mach/cap_types.h>

#include <kern/assert.h>		/* #974 */
#include <kern/cap.h>
#include <kern/hmac_sha256.h>
#include <kern/kalloc.h>
#include <kern/lock.h>
#include <kern/misc_protos.h>
#include <kern/host.h>		/* #744: HOST_NULL */
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/cpu_data.h>	/* #537: disable_preemption */
#include <kern/clock.h>		/* #747: the clock a deadline is read on */
#include <mach/clock_types.h>	/* #747: URMACH_CAP_CLOCK */
#include <mach/urmach_identity.h>	/* #747: who presents */
#include <kern/cpu_number.h>
#include <cpus.h>		/* NCPUS */

#ifndef PROBE_537_CAP_CONCURRENCY
#define PROBE_537_CAP_CONCURRENCY 0
#endif
#if PROBE_537_CAP_CONCURRENCY && NCPUS > 1
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/thread_act.h>
#include <kern/thread_swap.h>
#include <kern/tsc.h>
#include <mach/machine.h>
#endif

#include <mach/etap_events.h>

/* #432: a revoked capability must take its mappings with it. */
#include <device/device_master.h>

/* --- state ---------------------------------------------------------- */

#define CAP_STATE_BUCKETS       256

#define CAP_FLAG_REVOKED        0x1

/*
 * A revoked id, in its bucket's chain.  #974: with the deadline of the token
 * it was revoked as, and, while that deadline is to come, in the list of the
 * dated entries, earliest first.
 */
struct cap_state_entry {
    struct cap_state_entry *next;
    struct cap_state_entry *earlier;	/* #974: in the list by deadline */
    struct cap_state_entry *later;
    uint64_t  cap_id;
    uint64_t  expires;			/* #974: 0 -- kept for good */
    uint32_t  flags;
};

static struct cap_state_entry *cap_state_table[CAP_STATE_BUCKETS];

/*
 * ── What the kernel may forget, and when (#974) ──────────────────────────────
 *
 * An entry was made for every id revoked and none was ever freed, and since
 * #747 a deadline revokes every capability asked for with a lifetime: a task
 * that asked for short-lived ones steadily grew kernel memory, and the chains
 * cap_rest_locked() walks on every verification the cache misses, without
 * bound.
 *
 * 🔑 An entry decides an answer only while its token could otherwise verify.
 * The deadline is checked before this table, on every call and never from the
 * cache (cap_deadline_check()), so a token past its deadline is refused before
 * the table is asked, and its entry can no longer decide anything.  So:
 *
 *  - an id revoked when its deadline has already come gets no entry -- which is
 *    every revocation a deadline makes, cap_server's sweep coming after it;
 *  - an id revoked before its deadline gets one with that deadline, kept in
 *    cap_dated_first's list by deadline, and freed by the first revocation
 *    after the deadline comes (cap_dated_prune_locked());
 *  - an id whose token has no deadline keeps its entry for good: nothing else
 *    would ever refuse the token.
 *
 * ⚠️ ONLY ON A CLOCK THAT IS NEVER SET.  A passed deadline stays passed on
 * x86-64's monotonic clock; i386's REALTIME_CLOCK can be set back
 * (URMACH_CAP_CLOCK_NEVER_SET, <mach/clock_types.h>), and with it a deadline
 * that has come, so there every entry is kept, as before.
 *
 * The epoch moves with every revocation all the same, and the token's
 * mappings are torn down all the same: what is skipped is the entry, never the
 * revocation.
 */
static struct cap_state_entry *cap_dated_first, *cap_dated_last;
static unsigned int             cap_state_entries;	/* all of them */
static unsigned int             cap_state_dated;	/* in the list */

static uint8_t   cap_hmac_key[CAP_HMAC_SIZE];
/* #537: the key made ready when it is set -- its two padded blocks hashed once */
static struct hmac_sha256_key cap_hmac_ready;

/*
 * #537: the MAC checked once per token.  A DMA ask carries the same token for
 * every page of a buffer, and its MAC was computed again for each -- about
 * half of what an ask costs.  So each processor keeps the last token it
 * verified, whole, with the epoch it was verified in, and a token identical
 * to it in all its bytes, compared in constant time, under the same epoch, is
 * not verified again.  What is skipped is what cannot change while the bytes
 * and the epoch do not: the MAC, and the look-up among the revoked.  The
 * fields are checked on every call, against that call's arguments -- the token
 * may have been verified for another operation.
 *
 * cap_epoch moves, under cap_lock, with every revocation and every key
 * installed, so an entry made before either is an entry no longer matched.
 * It is read without the lock: an ask that read it just before a revocation
 * is an ask answered just before it, as one that finished then would be.
 * Thirty-two bits, read whole on both targets; a match across a wrap would
 * take four thousand million revocations between two asks of one processor.
 */
static volatile uint32_t cap_epoch;

static struct cap_verified {
    struct uros_cap token;
    uint32_t        epoch;
    uint32_t        valid;
    uint64_t        hits;
} __attribute__((aligned(64))) cap_verified[NCPUS];
static boolean_t cap_key_set = FALSE;
static task_t    cap_server_task = TASK_NULL;

decl_simple_lock_data(static, cap_lock)

/* --- helpers -------------------------------------------------------- */

static unsigned
cap_bucket(uint64_t cap_id)
{
    /* splitmix-ish fold into 8 bits — uniform enough for small tables */
    uint64_t x = cap_id;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (unsigned)(x & (CAP_STATE_BUCKETS - 1));
}

static struct cap_state_entry *
cap_state_lookup(uint64_t cap_id)
{
    struct cap_state_entry *e = cap_state_table[cap_bucket(cap_id)];
    while (e != NULL && e->cap_id != cap_id)
        e = e->next;
    return e;
}

/*
 * #974: `e' into the list by deadline, after every entry whose deadline is not
 * later than its own.  Looked for from the latest end: a task's capabilities
 * mostly come to their deadlines in the order they were asked for.  cap_lock
 * held.
 */
static void
cap_dated_insert(struct cap_state_entry *e)
{
    struct cap_state_entry *p = cap_dated_last;

    while (p != NULL && p->expires > e->expires)
        p = p->earlier;
    e->earlier = p;
    e->later = p != NULL ? p->later : cap_dated_first;
    if (e->later != NULL)
        e->later->earlier = e;
    else
        cap_dated_last = e;
    if (p != NULL)
        p->later = e;
    else
        cap_dated_first = e;
    cap_state_dated++;
}

/*
 * #974: every dated entry whose deadline has come by `now', out of the list
 * and out of its bucket, chained by `next' for cap_state_free() once cap_lock
 * is dropped -- the lock masks interrupts on x86-64, and kfree() need not run
 * under it.  The list is by deadline, so this stops at the first that has not
 * come.  cap_lock held.
 */
static struct cap_state_entry *
cap_dated_prune_locked(uint64_t now)
{
    struct cap_state_entry *e, **p, *gone = NULL;

    while ((e = cap_dated_first) != NULL && e->expires <= now) {
        cap_dated_first = e->later;
        if (cap_dated_first != NULL)
            cap_dated_first->earlier = NULL;
        else
            cap_dated_last = NULL;
        cap_state_dated--;
        for (p = &cap_state_table[cap_bucket(e->cap_id)];
             *p != NULL && *p != e; p = &(*p)->next)
            ;
        assert(*p == e);
        *p = e->next;
        cap_state_entries--;
        e->next = gone;
        gone = e;
    }
    return gone;
}

static void
cap_state_free(struct cap_state_entry *gone)
{
    struct cap_state_entry *e;

    while ((e = gone) != NULL) {
        gone = e->next;
        kfree((vm_offset_t)e, sizeof(*e));
    }
}

/*
 * Fetch-or-create an entry for cap_id.  Must be called with cap_lock
 * held; may temporarily drop it to kalloc.  Returns NULL on allocation
 * failure.  #974: `expires' is the deadline after which it may be
 * forgotten, 0 for none; an entry already there was made for the same
 * token, with the same deadline.
 */
static struct cap_state_entry *
cap_state_get_or_create(uint64_t cap_id, uint64_t expires)
{
    struct cap_state_entry *e = cap_state_lookup(cap_id);
    if (e != NULL)
        return e;

    simple_unlock(&cap_lock);
    e = (struct cap_state_entry *)kalloc(sizeof(*e));
    simple_lock(&cap_lock);
    if (e == NULL)
        return NULL;

    /* another thread may have raced us to insert the same cap_id */
    struct cap_state_entry *race = cap_state_lookup(cap_id);
    if (race != NULL) {
        kfree((vm_offset_t)e, sizeof(*e));
        return race;
    }

    e->cap_id    = cap_id;
    e->expires   = expires;		/* #974 */
    e->flags     = 0;
    e->earlier   = NULL;
    e->later     = NULL;
    unsigned b = cap_bucket(cap_id);
    e->next = cap_state_table[b];
    cap_state_table[b] = e;
    cap_state_entries++;		/* #974 */
    if (expires != 0)
        cap_dated_insert(e);		/* #974 */
    return e;
}

/*
 * #537: the HMAC with the key made ready once, and the one made ready for each
 * message, against the answer RFC 4231 publishes (test case 2) -- so a check
 * that verifies tokens fast verifies them RIGHT.  Both paths, because the
 * token self-test below signs with one and checks with the other, and two
 * paths that agree could still both be wrong.  The digest is printed as read.
 */
static void
cap_hmac_selftest(void)
{
    static const uint8_t want[HMAC_SHA256_SIZE] = {
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
        0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
        0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
        0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43,
    };
    static const char key[] = "Jefe";
    static const char msg[] = "what do ya want for nothing?";
    struct hmac_sha256_key k;
    uint8_t once[HMAC_SHA256_SIZE], each[HMAC_SHA256_SIZE];
    char hex[2 * HMAC_SHA256_SIZE + 1];
    static const char digits[] = "0123456789abcdef";

    hmac_sha256_key_init(&k, key, sizeof(key) - 1);
    hmac_sha256_with(&k, msg, sizeof(msg) - 1, once);
    hmac_sha256(key, sizeof(key) - 1, msg, sizeof(msg) - 1, each);

    for (unsigned i = 0; i < HMAC_SHA256_SIZE; i++) {
        hex[2 * i] = digits[once[i] >> 4];
        hex[2 * i + 1] = digits[once[i] & 0xF];
    }
    hex[2 * HMAC_SHA256_SIZE] = 0;

    printf("cap: HMAC-SHA256 of RFC 4231 case 2 with the key made ready once: "
           "%s -- %s (#537)\n", hex,
           !hmac_sha256_equal(once, want) ? "WRONG, not the RFC's"
           : !hmac_sha256_equal(each, want) ? "the RFC's, but the per-message "
                                              "path disagrees: WRONG"
           : "the RFC's, and so is the per-message path");
}

/*
 * Copy a cap token from user space.  Returns KERN_SUCCESS on success,
 * CAP_ERR_INVALID_TOKEN on a bad user pointer.
 */
static kern_return_t
cap_copyin_token(const struct uros_cap *user_token, struct uros_cap *out)
{
    if (user_token == NULL)
        return CAP_ERR_INVALID_TOKEN;
    if (copyin((const char *)user_token, (char *)out, sizeof(*out)) != 0)
        return CAP_ERR_INVALID_TOKEN;
    return KERN_SUCCESS;
}

/*
 * Core validation, urmach_cap_verify's and the kernel's own:
 *   - HMAC must match
 *   - not past its deadline (cap_deadline_check, #747)
 *   - presented by the task it was issued to (#747)
 *   - resource type and id must match
 *   - op must be a subset of allowed_ops
 *   - not revoked
 */
static kern_return_t cap_fields_check(const struct uros_cap *, uint64_t,
                                      uint32_t, uint32_t, uint64_t);

/* The fields, and the look-up among the revoked.  Must be called with cap_lock held. */
static kern_return_t
cap_rest_locked(const struct uros_cap *t,
                uint64_t presenter,
                uint32_t resource_type,
                uint32_t op,
                uint64_t resource_id)
{
    kern_return_t kr = cap_fields_check(t, presenter, resource_type, op,
                                        resource_id);

    if (kr != KERN_SUCCESS)
        return kr;

    struct cap_state_entry *e = cap_state_lookup(t->cap_id);
    if (e != NULL && (e->flags & CAP_FLAG_REVOKED))
        return CAP_ERR_REVOKED;

    return KERN_SUCCESS;
}

/*
 * Now on URMACH_CAP_CLOCK, nanoseconds since boot; FALSE when it cannot be
 * read.  #974: the deadline check's reading, and the revocation's, which asks
 * whether a deadline has come by the same clock and the same arithmetic.
 */
static boolean_t
cap_clock_now(uint64_t *ns)
{
    clock_t clock;
    tvalspec_t now;

    if (URMACH_CAP_CLOCK >= clock_count)
        return FALSE;
    clock = &clock_list[URMACH_CAP_CLOCK];
    if (clock->cl_ops == 0 || (*clock->cl_ops->c_gettime)(&now) != KERN_SUCCESS)
        return FALSE;
    *ns = (uint64_t)now.tv_sec * NSEC_PER_SEC + (uint64_t)now.tv_nsec;
    return TRUE;
}

/*
 * #747: is the token past its deadline?  `expires' is nanoseconds since boot on
 * URMACH_CAP_CLOCK, written by cap_server from that clock when it issued the
 * token, and 0 for a token asked for with no lifetime.  It was written 0 in
 * every token and read nowhere, so a lifetime asked for was dropped and the
 * token never ended.
 *
 * 🔑 Checked on every call and never cached: it is the one field whose answer
 * changes while the token's bytes do not, so #537's per-processor cache, which
 * skips what cannot change, must not skip it.  And outside cap_lock, so that no
 * clock is read under a lock every verification on every processor takes.
 *
 * ⚠️ A deadline that cannot be read against is not kept, and a token that has
 * one is refused: a clock that did not configure would otherwise make every
 * lifetime endless, which is the defect this check ends.
 */
static kern_return_t
cap_deadline_check(const struct uros_cap *t)
{
    uint64_t ns;

    if (t->expires == 0)
        return KERN_SUCCESS;
    if (!cap_clock_now(&ns))
        return CAP_ERR_EXPIRED;
    return ns < t->expires ? KERN_SUCCESS : CAP_ERR_EXPIRED;
}

/*
 * #537: the MAC outside cap_lock.  The lock is a spin lock, and on x86-64 it
 * masks interrupts for its hold; the HMAC is several thousand cycles, and
 * computed under it every verification on every processor waited for the one
 * in progress, with that processor's interrupts off.  So the key's contexts,
 * made ready, are copied under the lock, and the hash is computed after it is
 * dropped; the fields and the look-up among the revoked take it again
 * (cap_rest_locked).  A key installed while the hash runs comes after this
 * check, as one installed a moment later would: the epoch moves with it, so
 * nothing this check verified is kept.  ablations/537-hmac-locked.patch
 * computes it under the lock again.
 */
static kern_return_t
cap_mac_check_unlocked(const struct uros_cap *t)
{
    const size_t signed_len = sizeof(*t) - CAP_HMAC_SIZE;
    struct hmac_sha256_key k;
    uint8_t expected[HMAC_SHA256_SIZE];
    boolean_t set;
    int ok = 0;

    simple_lock(&cap_lock);
    set = cap_key_set;
    if (set)
        k = cap_hmac_ready;
    simple_unlock(&cap_lock);
    if (!set)
        return CAP_ERR_INTERNAL;

    hmac_sha256_with(&k, t, signed_len, expected);
    ok = hmac_sha256_equal(expected, t->hmac);

    /* The copy is the key: not left on the stack for whoever runs next. */
    bzero((char *)&k, sizeof(k));
    return ok ? KERN_SUCCESS : CAP_ERR_INVALID_TOKEN;
}

/* The whole check, cap_lock not held on entry or on return. */
static kern_return_t
cap_check_unlocked(const struct uros_cap *t,
                   uint64_t presenter,
                   uint32_t resource_type,
                   uint32_t op,
                   uint64_t resource_id)
{
    kern_return_t kr;

    kr = cap_mac_check_unlocked(t);
    if (kr != KERN_SUCCESS)
        return kr;
    kr = cap_deadline_check(t);		/* #747 */
    if (kr != KERN_SUCCESS)
        return kr;
    simple_lock(&cap_lock);
    kr = cap_rest_locked(t, presenter, resource_type, op, resource_id);
    simple_unlock(&cap_lock);
    return kr;
}

/*
 * The fields against the caller's arguments: the token's own bytes and nothing
 * else, so no lock (#537 checks them on every call, the cache's hits too).
 */
static kern_return_t
cap_fields_check(const struct uros_cap *t,
                 uint64_t presenter,
                 uint32_t resource_type,
                 uint32_t op,
                 uint64_t resource_id)
{
    /*
     * 🔴 #747: AND WHO PRESENTS IT.  A token was good for whoever held its
     * bytes -- one passed on in a message, one read out of another task's
     * memory, one a forked child found in its copy of its parent's.  It is
     * good for the task it was issued to now: the caller's own identity for
     * the kernel's checks, the requester's from its message's trailer for a
     * server's.  A presenter of 0 is nobody, and an owner of 0 was never
     * issued, so neither matches.
     */
    if (presenter == 0 || t->owner != presenter)
        return CAP_ERR_NOT_OWNER;
    /*
     * #599: and the KIND of resource the token names, for every caller --
     * the kernel's own and the two traps.  None compared it, so a token for
     * one kind was accepted for another whose ids matched: a block-device
     * capability carrying somebody's DMA region id passed
     * device_dma_map_foreign.  The type is under the MAC, so once that
     * verifies the field is the issuer's.
     */
    if (t->resource_type != resource_type)
        return CAP_ERR_RESOURCE_MISMATCH;
    if (t->resource_id != resource_id)
        return CAP_ERR_RESOURCE_MISMATCH;
    if ((t->allowed_ops & (uint64_t)op) != (uint64_t)op)
        return CAP_ERR_OP_NOT_ALLOWED;
    if (t->revoked)
        return CAP_ERR_REVOKED;
    return KERN_SUCCESS;
}

/* --- public API ----------------------------------------------------- */

void
cap_init(void)
{
    simple_lock_init(&cap_lock, ETAP_NO_TRACE);
    for (unsigned i = 0; i < CAP_STATE_BUCKETS; i++)
        cap_state_table[i] = NULL;
    for (unsigned i = 0; i < CAP_HMAC_SIZE; i++)
        cap_hmac_key[i] = 0;
    cap_key_set = FALSE;
    cap_server_task = TASK_NULL;
    printf("cap: subsystem initialized (slots 37-40)\n");
    cap_hmac_selftest();			/* #537 */
}

/*
 * The same check, on a token that is ALREADY IN KERNEL MEMORY.
 *
 * 🔴 SEPARATE FROM urmach_cap_verify BECAUSE THAT ONE COPIES IN.  It is a
 * trap: its argument is a user pointer, and cap_copyin_token() is what makes
 * that safe.  A kernel caller passing a kernel pointer to it would be asking
 * copyin to read kernel memory as if it were the caller's -- which on this
 * machine happens to work and is a fault waiting for the day it does not.
 *
 * The caller here is device_master.c, whose token arrived through MIG and is
 * therefore already in a kernel message buffer, checked and bounded (#432).
 */
kern_return_t
cap_check_in_kernel(const struct uros_cap *token,
                    uint32_t resource_type,
                    uint32_t op,
                    uint64_t resource_id)
{
    uint32_t epoch = cap_epoch;
    struct cap_verified *v;
    const uint8_t *a, *b;
    uint8_t diff = 0;
    kern_return_t kr;
    /* #747: inside the kernel the presenter is the task calling it. */
    uint64_t presenter = urmach_identity_of(current_task()->sec_token);

    /*
     * #537: the token this processor verified last, in this epoch?  Every
     * byte, the MAC's included, and in constant time: a comparison that
     * stopped at the first difference would say how many leading bytes of a
     * guess were right.
     */
    disable_preemption();
    v = &cap_verified[cpu_number()];
    a = (const uint8_t *)&v->token;
    b = (const uint8_t *)token;
    for (size_t i = 0; i < sizeof(*token); i++)
        diff |= a[i] ^ b[i];
    if (v->valid && v->epoch == epoch && diff == 0) {
        v->hits++;
        enable_preemption();
        kr = cap_deadline_check(token);		/* #747: never cached */
        if (kr != KERN_SUCCESS)
            return kr;
        return cap_fields_check(token, presenter, resource_type, op,
                                resource_id);
    }
    enable_preemption();

    kr = cap_check_unlocked(token, presenter, resource_type, op, resource_id);

    /*
     * Kept with the epoch read BEFORE the check: a revocation during it moves
     * the epoch past the entry, which then matches nothing.
     */
    if (kr == KERN_SUCCESS) {
        disable_preemption();
        v = &cap_verified[cpu_number()];
        v->token = *token;
        v->epoch = epoch;
        v->valid = 1;
        enable_preemption();
    }
    return kr;
}

boolean_t
cap_id_revoked(uint64_t cap_id)
{
    struct cap_state_entry *e;
    boolean_t               revoked;

    simple_lock(&cap_lock);
    e = cap_state_lookup(cap_id);
    revoked = (e != NULL && (e->flags & CAP_FLAG_REVOKED)) ? TRUE : FALSE;
    simple_unlock(&cap_lock);
    return revoked;
}

#if PROBE_537_CAP_CONCURRENCY && NCPUS > 1
/*
 * #537: what the MAC outside cap_lock is for, asked of the machine.  One
 * checker alone first -- this thread, K checks of a valid token on the path
 * with no cache (cap_check_unlocked) -- then one kernel thread bound to every
 * other running processor, all released together, each making the same K
 * checks.  The cycles a check took, alone and together, are the answer: with
 * the MAC under the lock (ablations/537-hmac-locked.patch) checkers on
 * different processors wait for each other, and a check together costs about
 * as many times one alone as there are checkers.  Bounded waits, so a thread
 * that never starts is a line, not a silence.  A build option, off by default.
 */
#define CAP_PROBE_ROUNDS	2000
#define CAP_PROBE_PATIENCE	20000000000ULL	/* TSC cycles, seconds' worth */

static struct uros_cap		cap_probe_token;
static volatile uint32_t	cap_probe_go, cap_probe_ready, cap_probe_finished;
static volatile uint32_t	cap_probe_refused;
static uint64_t			cap_probe_cycles[NCPUS];
static int			cap_probe_parked;
decl_simple_lock_data(static, cap_probe_lock)

static void
cap_probe_body(void)
{
    uint32_t refused = 0;
    uint64_t t0;

    simple_lock(&cap_probe_lock);
    cap_probe_ready++;
    simple_unlock(&cap_probe_lock);
    while (!cap_probe_go)
        continue;

    t0 = urmach_tsc();
    for (unsigned k = 0; k < CAP_PROBE_ROUNDS; k++)
        if (cap_check_unlocked(&cap_probe_token, cap_probe_token.owner,
                               RESOURCE_DMA_BUFFER,
                               CAP_OP_DMA_DEVICE_READ, 0x537) != KERN_SUCCESS)
            refused++;
    cap_probe_cycles[cpu_number()] = urmach_tsc() - t0;

    simple_lock(&cap_probe_lock);
    cap_probe_refused += refused;
    cap_probe_finished++;
    simple_unlock(&cap_probe_lock);

    for (;;) {			/* parked in a wait nobody signals */
        spl_t s = splsched();

        assert_wait((event_t)&cap_probe_parked, FALSE);
        splx(s);
        thread_block((void (*)(void))0);
    }
}

static int
cap_probe_start(processor_t p)
{
    thread_t th;
    thread_act_t act;
    spl_t s;

    if (thread_create_at(kernel_task, &th, cap_probe_body) != KERN_SUCCESS)
        return 0;
    act = th->top_act;
    thread_swappable(act, FALSE);
    s = splsched();
    thread_lock(th);
    th->max_priority = BASEPRI_SYSTEM;
    th->priority = BASEPRI_SYSTEM;
    th->sched_pri = BASEPRI_SYSTEM;
    thread_bind_locked(th, p);
    th->state |= TH_RUN;
    thread_setrun(th, TRUE, TAIL_Q);
    thread_unlock(th);
    splx(s);
    act_deallocate(act);
    thread_resume(act);
    return 1;
}

static void
cap_probe_drive(int me)
{
    uint64_t t0, alone, lo = ~0ULL, hi = 0;
    uint32_t started = 0, refused = 0;
    int i;

    simple_lock_init(&cap_probe_lock, ETAP_NO_TRACE);
    bzero((char *)&cap_probe_token, sizeof(cap_probe_token));
    cap_probe_token.cap_id = ~0ULL - 2;
    cap_probe_token.resource_type = RESOURCE_DMA_BUFFER;
    cap_probe_token.resource_id = 0x537;
    cap_probe_token.allowed_ops = CAP_OP_DMA_DEVICE_READ;
    cap_probe_token.owner = URMACH_IDENTITY_TASK_BIT | 0x537;	/* #747 */
    simple_lock(&cap_lock);
    hmac_sha256_with(&cap_hmac_ready, &cap_probe_token,
                     sizeof(cap_probe_token) - CAP_HMAC_SIZE,
                     cap_probe_token.hmac);
    simple_unlock(&cap_lock);

    disable_preemption();
    t0 = urmach_tsc();
    for (unsigned k = 0; k < CAP_PROBE_ROUNDS; k++)
        if (cap_check_unlocked(&cap_probe_token, cap_probe_token.owner,
                               RESOURCE_DMA_BUFFER,
                               CAP_OP_DMA_DEVICE_READ, 0x537) != KERN_SUCCESS)
            refused++;
    alone = (urmach_tsc() - t0) / CAP_PROBE_ROUNDS;
    enable_preemption();

    for (i = 0; i < NCPUS; i++) {
        if (i == me || !machine_slot[i].is_cpu || !machine_slot[i].running)
            continue;
        started += cap_probe_start(cpu_to_processor(i));
    }
    if (started == 0) {
        printf("cap: probe -- one checker alone: %llu cycles a check; NOT ASKED "
               "together, no other processor is running (#537)\n",
               (unsigned long long)alone);
        return;
    }

    for (t0 = urmach_tsc(); cap_probe_ready < started &&
         urmach_tsc() - t0 < CAP_PROBE_PATIENCE; )
        continue;
    if (cap_probe_ready < started) {
        printf("cap: probe -- only %u of %u checker threads started; NOT ASKED "
               "(#537)\n", cap_probe_ready, started);
        return;
    }
    cap_probe_go = 1;
    for (t0 = urmach_tsc(); cap_probe_finished < started &&
         urmach_tsc() - t0 < CAP_PROBE_PATIENCE; )
        continue;
    if (cap_probe_finished < started) {
        printf("cap: probe -- only %u of %u checkers finished their %u checks; "
               "WRONG, or slower than seconds (#537)\n", cap_probe_finished,
               started, CAP_PROBE_ROUNDS);
        return;
    }

    for (i = 0; i < NCPUS; i++) {
        uint64_t c = cap_probe_cycles[i] / CAP_PROBE_ROUNDS;

        if (cap_probe_cycles[i] == 0)
            continue;
        if (c < lo)
            lo = c;
        if (c > hi)
            hi = c;
    }
    printf("cap: probe -- one checker alone: %llu cycles a check; %u together, "
           "one a processor: %llu-%llu cycles a check, %u refused; the MAC "
           "outside cap_lock (#537)\n", (unsigned long long)alone, started,
           (unsigned long long)lo, (unsigned long long)hi,
           refused + cap_probe_refused);
}

/*
 * The driver bound to the processor it starts on, for the whole probe (#648).
 * cap_probe_drive() starts a checker on every processor but `me', and read
 * `me' with preemption off and used it with preemption on: a driver that moved
 * started a checker on the processor it then ran on and none on the one it had
 * left.  Bound, it cannot move; bound here rather than in the body, so that
 * none of its four ways out has to undo it.
 */
static void
cap_probe_run(void)
{
    cap_probe_drive(thread_bind_here());
    thread_bind(current_thread(), PROCESSOR_NULL);
}
#endif	/* PROBE_537_CAP_CONCURRENCY && NCPUS > 1 */

/*
 * #537: the cache asked once the key exists.  A token checked twice on one
 * processor is answered from the cache the second time -- the hit counted --
 * and once revoked through the real path, cap_revoke_token(), its next check
 * is refused, the cache notwithstanding.  cap_id ~0 - 1 is one cap_server
 * never issues; its revocation entry stays, and shadows nothing.  Run in
 * cap_server's registration, so cap_revoke_token() finds its caller allowed.
 * ablations/537-epoch-kept.patch is how this is shown able to say WRONG.
 */
static kern_return_t cap_revoke_token(const struct uros_cap *);	/* #974 */

static void
cap_cache_selftest(void)
{
    struct uros_cap t;
    struct cap_verified *v;
    kern_return_t first, second, after;
    uint64_t hits0, hits1;

    bzero((char *)&t, sizeof(t));
    t.cap_id = ~0ULL - 1;
    t.resource_type = RESOURCE_DMA_BUFFER;
    t.resource_id = 0x537;
    t.allowed_ops = CAP_OP_DMA_DEVICE_READ;
    /* #747: the task this runs in presents it -- cap_server's registration. */
    t.owner = urmach_identity_of(current_task()->sec_token);
    simple_lock(&cap_lock);
    hmac_sha256_with(&cap_hmac_ready, &t, sizeof(t) - CAP_HMAC_SIZE, t.hmac);
    simple_unlock(&cap_lock);

    disable_preemption();
    v = &cap_verified[cpu_number()];
    first = cap_check_in_kernel(&t, RESOURCE_DMA_BUFFER,
                                CAP_OP_DMA_DEVICE_READ, 0x537);
    hits0 = v->hits;
    second = cap_check_in_kernel(&t, RESOURCE_DMA_BUFFER,
                                 CAP_OP_DMA_DEVICE_READ, 0x537);
    hits1 = v->hits;
    enable_preemption();

    (void)cap_revoke_token(&t);
    after = cap_check_in_kernel(&t, RESOURCE_DMA_BUFFER,
                                CAP_OP_DMA_DEVICE_READ, 0x537);

    printf("cap: a token checked twice on one processor: answers %d and %d, "
           "%llu from the cache; revoked, its next check answers %d -- %s "
           "(#537)\n", first, second, (unsigned long long)(hits1 - hits0),
           after,
           (first != KERN_SUCCESS || second != KERN_SUCCESS)
           ? "WRONG, a valid token was refused"
           : hits1 - hits0 != 1 ? "WRONG, the second check did not hit"
           : after != CAP_ERR_REVOKED ? "WRONG, the cache outlived the revocation"
           : "the cache answered, and followed the revocation");
}

/*
 * #599: the type check above, asked once the key exists.  A token signed
 * here for a PCI class must answer as PCI and must not answer as a DMA buffer
 * with the same id.  cap_id ~0 is one cap_server never issues, so no
 * revocation entry can shadow it.  One line either way, with both answers.
 */
static void
cap_type_selftest(void)
{
    struct uros_cap t;
    kern_return_t   as_pci, as_dma;

    bzero((char *)&t, sizeof(t));
    t.cap_id = ~0ULL;
    t.resource_type = RESOURCE_PCI_DEVICE;
    t.resource_id = 0x010601;
    t.allowed_ops = 0x3;
    t.owner = urmach_identity_of(current_task()->sec_token);	/* #747 */
    simple_lock(&cap_lock);
    hmac_sha256(cap_hmac_key, CAP_HMAC_SIZE, &t,
                sizeof(t) - CAP_HMAC_SIZE, t.hmac);
    simple_unlock(&cap_lock);

    as_pci = cap_check_in_kernel(&t, RESOURCE_PCI_DEVICE, 0x1, 0x010601);
    as_dma = cap_check_in_kernel(&t, RESOURCE_DMA_BUFFER, 0x1, 0x010601);
    bzero((char *)&t, sizeof(t));	/* a signed token does not linger */

    if (as_pci == KERN_SUCCESS && as_dma == CAP_ERR_RESOURCE_MISMATCH)
        printf("cap: a capability answers for the kind it names — a PCI "
               "token checked as PCI gives %d, as a DMA buffer with the same "
               "id %d (#599)\n", as_pci, as_dma);
    else
        printf("cap: WRONG — a PCI token checked as PCI gives %d and as a "
               "DMA buffer with the same id %d; the kind is not what is "
               "checked (#599)\n", as_pci, as_dma);
}

/*
 * #747: `presenter' is the identity of the task presenting the token, which a
 * server reads from the trailer of the request that carried it
 * (urmach_msg_sender()).  The trap cannot know it: the caller is the server
 * verifying, not the client presenting.
 */
kern_return_t
urmach_cap_verify(const struct uros_cap *user_token,
                  uint64_t presenter,
                  uint32_t resource_type,
                  uint32_t op,
                  uint64_t resource_id)
{
    struct uros_cap t;
    kern_return_t kr = cap_copyin_token(user_token, &t);
    if (kr != KERN_SUCCESS)
        return kr;

    return cap_check_unlocked(&t, presenter, resource_type, op, resource_id);
}

/*
 * #537: the revocation epoch, as a trap (45) -- what a server that keeps a
 * buffer's translation reads after a transfer to know whether the capability
 * it was granted on can have been revoked meanwhile.  cap_epoch moves with
 * every revocation and every key installed; the same value before and after
 * means no revocation happened in between, and a different one sends the
 * server to verify its capability again.  No lock, no argument: thirty-two
 * bits read whole on both targets.  Answered in the return word, as the
 * epoch itself -- there is no failure to report.
 */
kern_return_t
urmach_cap_epoch(void)
{
    return (kern_return_t)cap_epoch;
}

/*
 * The revocation of `t', a token in kernel memory: urmach_cap_revoke's, on the
 * copy it made, and the cache's self-test's.
 *
 * #974: a token and not an id.  The trap took cap_server's word for the id; it
 * is told the token now, cap_server's own copy, and checks its MAC, so that
 * what it reads of it -- the id, and the deadline it may forget the revocation
 * after -- is what the issuer signed.  Who may revoke is asked first, as
 * before, and the MAC outside cap_lock, as every check computes it.
 */
static kern_return_t
cap_revoke_token(const struct uros_cap *t)
{
    task_t caller = current_task();
    uint64_t cap_id = t->cap_id;
    struct cap_state_entry *e, *gone;
    boolean_t have_now;
    uint64_t now = 0;
    kern_return_t kr;

    simple_lock(&cap_lock);
    if (!cap_key_set || caller != cap_server_task) {
        simple_unlock(&cap_lock);
        return CAP_ERR_UNAUTHORIZED;
    }
    simple_unlock(&cap_lock);

    kr = cap_mac_check_unlocked(t);
    if (kr != KERN_SUCCESS)
        return kr;
    /*
     * #974: whether deadlines have come, read before cap_lock as the check
     * reads it -- and only on a clock that is never set.  Read a moment early,
     * it forgets less, never more.
     */
    have_now = URMACH_CAP_CLOCK_NEVER_SET && cap_clock_now(&now);

    simple_lock(&cap_lock);
    if (!cap_key_set || caller != cap_server_task) {
        simple_unlock(&cap_lock);
        return CAP_ERR_UNAUTHORIZED;
    }

    gone = have_now ? cap_dated_prune_locked(now) : NULL;	/* #974 */
    /*
     * #974: a token whose deadline has come is refused by it before this table
     * is asked, so its id is given no entry.  The deadline's own revocation,
     * which cap_server's sweep makes after it, is always such a one.
     */
    if (!have_now || t->expires == 0 || now < t->expires) {
        e = cap_state_get_or_create(cap_id, have_now ? t->expires : 0);
        if (e == NULL) {
            simple_unlock(&cap_lock);
            cap_state_free(gone);
            return CAP_ERR_NO_MEMORY;
        }
        e->flags |= CAP_FLAG_REVOKED;
    }
    cap_epoch++;			/* #537: no verified token outlives it */
    simple_unlock(&cap_lock);
    cap_state_free(gone);

    /*
     * 🔴 AND THE MAPPINGS THIS TOKEN BOUGHT, torn down (#432).
     *
     * Marking the id revoked withdraws it from everything that CHECKS it on
     * use.  A capability that was turned into a MAPPING is not checked again:
     * an IOMMU domain is a page table, and the device walks it whether or not
     * the token behind it is still good.  So the withdrawal has to reach the
     * mapping, and this is where the kernel knows first.
     *
     * ⚠️ OUTSIDE cap_lock, deliberately.  The teardown writes engine registers
     * and spins on them, and holding a simple lock the whole machine's
     * capability checks contend for while doing memory-mapped I/O is a
     * serialisation nobody would find by reading either side alone.
     */
    device_master_cap_revoked(cap_id);
    return KERN_SUCCESS;
}

/*
 * #974: how many revocations the kernel holds, and how many of them carry a
 * deadline after which it forgets them -- trap 47, how a test sees an entry
 * leave.  Any task may ask: a count says nothing about any one capability, as
 * cap_server's cap_census says nothing either.
 */
kern_return_t
urmach_cap_census(natural_t *user_entries, natural_t *user_dated)
{
    natural_t entries, dated;

    simple_lock(&cap_lock);
    entries = cap_state_entries;
    dated = cap_state_dated;
    simple_unlock(&cap_lock);
    if (copyout((char *)&entries, (char *)user_entries, sizeof(entries)) != 0 ||
        copyout((char *)&dated, (char *)user_dated, sizeof(dated)) != 0)
        return KERN_INVALID_ADDRESS;
    return KERN_SUCCESS;
}

kern_return_t
urmach_cap_revoke(const struct uros_cap *user_token)
{
    struct uros_cap t;
    kern_return_t kr = cap_copyin_token(user_token, &t);

    if (kr != KERN_SUCCESS)
        return kr;
    kr = cap_revoke_token(&t);
    bzero((char *)&t, sizeof(t));	/* a signed token does not linger */
    return kr;
}

/*
 * Which task may install the HMAC key: the one bootstrap started as cap_server,
 * named before it has a thread (#744).  By the privileged host port, which
 * bootstrap holds and hands to nobody it does not mean to.
 *
 * ⚠️ WITH A REFERENCE.  The task used to be kept as a bare pointer, and a
 * cap_server that died left it naming freed memory, which the task zone gives
 * to the next task it makes -- the shape of #749.  The reference keeps the
 * task, ended or not, until another is named.
 *
 * Naming another task forgets the key: a key is its cap_server's, and the
 * tokens it signed are no one's once that server is gone.
 */
kern_return_t
urmach_cap_server_set(host_t host_priv, task_t task)
{
    task_t old;

    if (host_priv == HOST_NULL)
        return KERN_INVALID_HOST;
    if (task == TASK_NULL)
        return KERN_INVALID_ARGUMENT;

    task_reference(task);
    simple_lock(&cap_lock);
    old = cap_server_task;
    cap_server_task = task;
    if (old != task && cap_key_set) {
        for (unsigned i = 0; i < CAP_HMAC_SIZE; i++)
            cap_hmac_key[i] = 0;
        cap_key_set = FALSE;
        cap_epoch++;			/* #537: no verified token outlives it */
    }
    simple_unlock(&cap_lock);

    if (old != TASK_NULL)
        task_deallocate(old);
    return KERN_SUCCESS;
}

kern_return_t
urmach_cap_register(const struct uros_cap *user_token)
{
    struct uros_cap t;
    kern_return_t kr = cap_copyin_token(user_token, &t);
    if (kr != KERN_SUCCESS)
        return kr;

    task_t caller = current_task();

    simple_lock(&cap_lock);

    /*
     * Setup-token path: cap_id == 0 → install (or refresh) HMAC key.
     *
     * 🔴 ONLY FROM THE TASK BOOTSTRAP NAMED (#744).  It used to be whichever
     * task installed a key first, and nothing tied that task to the one
     * bootstrap started as cap_server: six tasks start before it in the
     * x86-64 bundle, and one that registered first would have held the key
     * and left cap_server's own registration refused.  Before bootstrap
     * names a task, nobody may: a capability system with no key refuses
     * every token, which is the answer for a boot with no cap_server.
     */
    if (t.cap_id == 0) {
        if (caller != cap_server_task) {
            simple_unlock(&cap_lock);
            return CAP_ERR_UNAUTHORIZED;
        }
        for (unsigned i = 0; i < CAP_HMAC_SIZE; i++)
            cap_hmac_key[i] = t.hmac[i];
        hmac_sha256_key_init(&cap_hmac_ready, cap_hmac_key, CAP_HMAC_SIZE);
        cap_epoch++;			/* #537: nor any key */
        cap_key_set = TRUE;
        simple_unlock(&cap_lock);
        printf("cap: hmac key registered (len=%u)\n",
               (unsigned)CAP_HMAC_SIZE);
        cap_type_selftest();			/* #599 */
        cap_cache_selftest();			/* #537 */
#if PROBE_537_CAP_CONCURRENCY && NCPUS > 1
        cap_probe_run();			/* #537 */
#endif
        return KERN_SUCCESS;
    }

    /* Non-setup register: reserved for future bookkeeping.  v1 accepts
     * it (validates caller identity) but stores nothing beyond what the
     * revocation table already provides. */
    if (!cap_key_set || caller != cap_server_task) {
        simple_unlock(&cap_lock);
        return CAP_ERR_UNAUTHORIZED;
    }
    simple_unlock(&cap_lock);
    return KERN_SUCCESS;
}
