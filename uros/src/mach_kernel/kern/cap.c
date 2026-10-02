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
 *   cap_server_task     the task that installed the key; only it may
 *                       refresh the key or revoke caps (TOFU model)
 *   cap_state_table[]   hash set of cap_ids that are revoked and/or
 *                       have been consumed (for use-once tokens).
 *
 * Threading: a single global simple_lock guards the whole subsystem.
 * Suitable for v1 where cap_server is the only writer and verify is
 * uncontended; a sharded table with per-bucket locks is tracked for
 * the SMP/slab issue (#80).
 */

#include <mach/boolean.h>
#include <mach/kern_return.h>
#include <mach/cap_types.h>

#include <kern/cap.h>
#include <kern/hmac_sha256.h>
#include <kern/kalloc.h>
#include <kern/lock.h>
#include <kern/misc_protos.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/cpu_data.h>	/* #537: disable_preemption */
#include <kern/cpu_number.h>
#include <cpus.h>		/* NCPUS */

#include <mach/etap_events.h>

/* #432: a revoked capability must take its mappings with it. */
#include <device/device_master.h>

/* --- state ---------------------------------------------------------- */

#define CAP_STATE_BUCKETS       256

#define CAP_FLAG_REVOKED        0x1
#define CAP_FLAG_CONSUMED       0x2

struct cap_state_entry {
    struct cap_state_entry *next;
    uint64_t  cap_id;
    uint32_t  flags;
    uint32_t  uses_left;     /* meaningful only when max_uses != 0 */
};

static struct cap_state_entry *cap_state_table[CAP_STATE_BUCKETS];

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

#ifndef ABLATE_537_MAC_EVERY_TIME
#define ABLATE_537_MAC_EVERY_TIME 0	/* the cache never used */
#endif
#ifndef ABLATE_537_EPOCH_KEPT
#define ABLATE_537_EPOCH_KEPT 0		/* a revocation leaves the epoch */
#endif

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
 * Fetch-or-create an entry for cap_id.  Must be called with cap_lock
 * held; may temporarily drop it to kalloc.  Returns NULL on allocation
 * failure.
 */
static struct cap_state_entry *
cap_state_get_or_create(uint64_t cap_id)
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
    e->flags     = 0;
    e->uses_left = 0;
    unsigned b = cap_bucket(cap_id);
    e->next = cap_state_table[b];
    cap_state_table[b] = e;
    return e;
}

/*
 * Compute the HMAC over every byte of the token *except* the trailing
 * hmac[] field, and compare it against token->hmac in constant time.
 * Returns TRUE on match.
 */
/*
 * #537: UROS_ABLATE_537_HMAC_FULL makes the key's two blocks hashed again on
 * every check, as they were -- the per-page split's MAC column is what shows
 * the difference.
 */
#ifndef ABLATE_537_HMAC_FULL
#define ABLATE_537_HMAC_FULL 0
#endif

static boolean_t
cap_hmac_check(const struct uros_cap *token)
{
    const size_t signed_len = sizeof(*token) - CAP_HMAC_SIZE;
    uint8_t expected[HMAC_SHA256_SIZE];

    if (ABLATE_537_HMAC_FULL)
        hmac_sha256(cap_hmac_key, CAP_HMAC_SIZE, token, signed_len, expected);
    else
        hmac_sha256_with(&cap_hmac_ready, token, signed_len, expected);
    return hmac_sha256_equal(expected, token->hmac) ? TRUE : FALSE;
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
 * Core validation shared by urmach_cap_verify and urmach_cap_use:
 *   - HMAC must match
 *   - resource_id must match
 *   - op must be a subset of allowed_ops
 *   - not expired (for v1 `expires` is unused and must be 0)
 *   - not revoked
 * Must be called with cap_lock held.
 */
static kern_return_t cap_fields_check(const struct uros_cap *, uint32_t,
                                      uint32_t, uint64_t);
static kern_return_t cap_rest_locked(const struct uros_cap *, uint32_t,
                                     uint32_t, uint64_t);

static kern_return_t
cap_check_locked(const struct uros_cap *t,
                 uint32_t resource_type,
                 uint32_t op,
                 uint64_t resource_id)
{
    if (!cap_key_set)
        return CAP_ERR_INTERNAL;
    if (!cap_hmac_check(t))
        return CAP_ERR_INVALID_TOKEN;
    return cap_rest_locked(t, resource_type, op, resource_id);
}

/* The fields, and the look-up among the revoked.  Must be called with cap_lock held. */
static kern_return_t
cap_rest_locked(const struct uros_cap *t,
                uint32_t resource_type,
                uint32_t op,
                uint64_t resource_id)
{
    kern_return_t kr = cap_fields_check(t, resource_type, op, resource_id);

    if (kr != KERN_SUCCESS)
        return kr;

    struct cap_state_entry *e = cap_state_lookup(t->cap_id);
    if (e != NULL && (e->flags & CAP_FLAG_REVOKED))
        return CAP_ERR_REVOKED;

    return KERN_SUCCESS;
}

/*
 * #537: the MAC outside cap_lock.  The lock is a spin lock, and on x86-64 it
 * masks interrupts for its hold; the HMAC is several thousand cycles, and
 * computed under it every verification on every processor waited for the one
 * in progress, with that processor's interrupts off.  So the key is copied
 * under the lock -- the contexts made ready, or with UROS_ABLATE_537_HMAC_FULL
 * the raw key -- and the hash computed after it is dropped; the fields and the
 * look-up among the revoked take it again (cap_rest_locked).  A key installed
 * while the hash runs comes after this check, as one installed a moment later
 * would: the epoch moves with it, so nothing this check verified is kept.
 * UROS_ABLATE_537_HMAC_LOCKED computes it under the lock again.
 */
#ifndef ABLATE_537_HMAC_LOCKED
#define ABLATE_537_HMAC_LOCKED 0
#endif

static kern_return_t
cap_mac_check_unlocked(const struct uros_cap *t)
{
    const size_t signed_len = sizeof(*t) - CAP_HMAC_SIZE;
    struct hmac_sha256_key k;
    uint8_t raw[CAP_HMAC_SIZE], expected[HMAC_SHA256_SIZE];
    boolean_t set;
    int ok = 0;

    simple_lock(&cap_lock);
    set = cap_key_set;
    if (set) {
        if (ABLATE_537_HMAC_FULL)
            for (unsigned i = 0; i < CAP_HMAC_SIZE; i++)
                raw[i] = cap_hmac_key[i];
        else
            k = cap_hmac_ready;
    }
    simple_unlock(&cap_lock);
    if (!set)
        return CAP_ERR_INTERNAL;

    if (ABLATE_537_HMAC_FULL)
        hmac_sha256(raw, CAP_HMAC_SIZE, t, signed_len, expected);
    else
        hmac_sha256_with(&k, t, signed_len, expected);
    ok = hmac_sha256_equal(expected, t->hmac);

    /* The copies are the key: not left on the stack for whoever runs next. */
    bzero((char *)&k, sizeof(k));
    bzero((char *)raw, sizeof(raw));
    return ok ? KERN_SUCCESS : CAP_ERR_INVALID_TOKEN;
}

/* The whole check, cap_lock not held on entry or on return. */
static kern_return_t
cap_check_unlocked(const struct uros_cap *t,
                   uint32_t resource_type,
                   uint32_t op,
                   uint64_t resource_id)
{
    kern_return_t kr;

    if (ABLATE_537_HMAC_LOCKED) {
        simple_lock(&cap_lock);
        kr = cap_check_locked(t, resource_type, op, resource_id);
        simple_unlock(&cap_lock);
        return kr;
    }
    kr = cap_mac_check_unlocked(t);
    if (kr != KERN_SUCCESS)
        return kr;
    simple_lock(&cap_lock);
    kr = cap_rest_locked(t, resource_type, op, resource_id);
    simple_unlock(&cap_lock);
    return kr;
}

/*
 * The fields against the caller's arguments: the token's own bytes and nothing
 * else, so no lock (#537 checks them on every call, the cache's hits too).
 */
static kern_return_t
cap_fields_check(const struct uros_cap *t,
                 uint32_t resource_type,
                 uint32_t op,
                 uint64_t resource_id)
{
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

    /*
     * #537: the token this processor verified last, in this epoch?  Every
     * byte, the MAC's included, and in constant time: a comparison that
     * stopped at the first difference would say how many leading bytes of a
     * guess were right.
     */
    if (!ABLATE_537_MAC_EVERY_TIME) {
        disable_preemption();
        v = &cap_verified[cpu_number()];
        a = (const uint8_t *)&v->token;
        b = (const uint8_t *)token;
        for (size_t i = 0; i < sizeof(*token); i++)
            diff |= a[i] ^ b[i];
        if (v->valid && v->epoch == epoch && diff == 0) {
            v->hits++;
            enable_preemption();
            return cap_fields_check(token, resource_type, op, resource_id);
        }
        enable_preemption();
    }

    kr = cap_check_unlocked(token, resource_type, op, resource_id);

    /*
     * Kept with the epoch read BEFORE the check: a revocation during it moves
     * the epoch past the entry, which then matches nothing.
     */
    if (kr == KERN_SUCCESS && !ABLATE_537_MAC_EVERY_TIME) {
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

/*
 * #537: the cache asked once the key exists.  A token checked twice on one
 * processor is answered from the cache the second time -- the hit counted --
 * and once revoked through the real path, urmach_cap_revoke(), its next check
 * is refused, the cache notwithstanding.  cap_id ~0 - 1 is one cap_server
 * never issues; its revocation entry stays, and shadows nothing.  Run in
 * cap_server's registration, so urmach_cap_revoke() finds its caller allowed.
 * UROS_ABLATE_537_EPOCH_KEPT is how this is shown able to say WRONG.
 */
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

    (void)urmach_cap_revoke(t.cap_id);
    after = cap_check_in_kernel(&t, RESOURCE_DMA_BUFFER,
                                CAP_OP_DMA_DEVICE_READ, 0x537);

    printf("cap: a token checked twice on one processor: answers %d and %d, "
           "%llu from the cache; revoked, its next check answers %d -- %s "
           "(#537)\n", first, second, (unsigned long long)(hits1 - hits0),
           after,
           (ABLATE_537_MAC_EVERY_TIME && first == KERN_SUCCESS &&
            second == KERN_SUCCESS && after == CAP_ERR_REVOKED)
           ? "NOT ASKED, the cache is ablated"
           : (first != KERN_SUCCESS || second != KERN_SUCCESS)
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

kern_return_t
urmach_cap_verify(const struct uros_cap *user_token,
                  uint32_t resource_type,
                  uint32_t op,
                  uint64_t resource_id)
{
    struct uros_cap t;
    kern_return_t kr = cap_copyin_token(user_token, &t);
    if (kr != KERN_SUCCESS)
        return kr;

    return cap_check_unlocked(&t, resource_type, op, resource_id);
}

kern_return_t
urmach_cap_use(const struct uros_cap *user_token,
               uint32_t resource_type,
               uint32_t op,
               uint64_t resource_id)
{
    struct uros_cap t;
    kern_return_t kr = cap_copyin_token(user_token, &t);
    if (kr != KERN_SUCCESS)
        return kr;

    /* #537: the MAC outside the lock; the revocation look-up and the use
     * counted in one hold, as before. */
    if (!ABLATE_537_HMAC_LOCKED) {
        kr = cap_mac_check_unlocked(&t);
        if (kr != KERN_SUCCESS)
            return kr;
    }
    simple_lock(&cap_lock);
    kr = ABLATE_537_HMAC_LOCKED
        ? cap_check_locked(&t, resource_type, op, resource_id)
        : cap_rest_locked(&t, resource_type, op, resource_id);
    if (kr != KERN_SUCCESS) {
        simple_unlock(&cap_lock);
        return kr;
    }

    if (t.max_uses != 0) {
        struct cap_state_entry *e = cap_state_get_or_create(t.cap_id);
        if (e == NULL) {
            simple_unlock(&cap_lock);
            return CAP_ERR_NO_MEMORY;
        }
        /* re-check revoked state after the potentially-dropped lock */
        if (e->flags & CAP_FLAG_REVOKED) {
            simple_unlock(&cap_lock);
            return CAP_ERR_REVOKED;
        }
        if (!(e->flags & CAP_FLAG_CONSUMED)) {
            e->uses_left = t.max_uses;
            e->flags |= CAP_FLAG_CONSUMED;
        }
        if (e->uses_left == 0) {
            simple_unlock(&cap_lock);
            return CAP_ERR_EXHAUSTED;
        }
        e->uses_left--;
    }

    simple_unlock(&cap_lock);
    return KERN_SUCCESS;
}

kern_return_t
urmach_cap_revoke(uint64_t cap_id)
{
    task_t caller = current_task();

    simple_lock(&cap_lock);
    if (!cap_key_set || caller != cap_server_task) {
        simple_unlock(&cap_lock);
        return CAP_ERR_UNAUTHORIZED;
    }

    struct cap_state_entry *e = cap_state_get_or_create(cap_id);
    if (e == NULL) {
        simple_unlock(&cap_lock);
        return CAP_ERR_NO_MEMORY;
    }
    e->flags |= CAP_FLAG_REVOKED;
    if (!ABLATE_537_EPOCH_KEPT)
        cap_epoch++;			/* #537: no verified token outlives it */
    simple_unlock(&cap_lock);

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

kern_return_t
urmach_cap_register(const struct uros_cap *user_token)
{
    struct uros_cap t;
    kern_return_t kr = cap_copyin_token(user_token, &t);
    if (kr != KERN_SUCCESS)
        return kr;

    task_t caller = current_task();

    simple_lock(&cap_lock);

    /* Setup-token path: cap_id == 0 → install (or refresh) HMAC key. */
    if (t.cap_id == 0) {
        if (cap_key_set && caller != cap_server_task) {
            simple_unlock(&cap_lock);
            return CAP_ERR_UNAUTHORIZED;
        }
        for (unsigned i = 0; i < CAP_HMAC_SIZE; i++)
            cap_hmac_key[i] = t.hmac[i];
        hmac_sha256_key_init(&cap_hmac_ready, cap_hmac_key, CAP_HMAC_SIZE);
        cap_epoch++;			/* #537: nor any key */
        cap_key_set = TRUE;
        cap_server_task = caller;
        simple_unlock(&cap_lock);
        printf("cap: hmac key registered (len=%u)\n",
               (unsigned)CAP_HMAC_SIZE);
        cap_type_selftest();			/* #599 */
        cap_cache_selftest();			/* #537 */
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
