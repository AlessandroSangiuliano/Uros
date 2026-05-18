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

#include <mach/etap_events.h>

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
static boolean_t
cap_hmac_check(const struct uros_cap *token)
{
    const size_t signed_len = sizeof(*token) - CAP_HMAC_SIZE;
    uint8_t expected[HMAC_SHA256_SIZE];

    hmac_sha256(cap_hmac_key, CAP_HMAC_SIZE,
                token, signed_len,
                expected);
    return hmac_sha256_equal(expected, token->hmac) ? TRUE : FALSE;
}

/*
 * Copy a cap token from user space.  Returns KERN_SUCCESS on success,
 * CAP_ERR_INVALID_TOKEN on a bad user pointer.
 */
static kern_return_t
cap_copyin_token(struct uros_cap *user_token, struct uros_cap *out)
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
static kern_return_t
cap_check_locked(const struct uros_cap *t,
                 uint32_t op,
                 uint64_t resource_id)
{
    if (!cap_key_set)
        return CAP_ERR_INTERNAL;
    if (!cap_hmac_check(t))
        return CAP_ERR_INVALID_TOKEN;
    if (t->resource_id != resource_id)
        return CAP_ERR_RESOURCE_MISMATCH;
    if ((t->allowed_ops & (uint64_t)op) != (uint64_t)op)
        return CAP_ERR_OP_NOT_ALLOWED;
    if (t->revoked)
        return CAP_ERR_REVOKED;

    struct cap_state_entry *e = cap_state_lookup(t->cap_id);
    if (e != NULL && (e->flags & CAP_FLAG_REVOKED))
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
}

kern_return_t
urmach_cap_verify(struct uros_cap *user_token,
                  uint32_t op,
                  uint64_t resource_id)
{
    struct uros_cap t;
    kern_return_t kr = cap_copyin_token(user_token, &t);
    if (kr != KERN_SUCCESS)
        return kr;

    simple_lock(&cap_lock);
    kr = cap_check_locked(&t, op, resource_id);
    simple_unlock(&cap_lock);
    return kr;
}

kern_return_t
urmach_cap_use(struct uros_cap *user_token,
               uint32_t op,
               uint64_t resource_id)
{
    struct uros_cap t;
    kern_return_t kr = cap_copyin_token(user_token, &t);
    if (kr != KERN_SUCCESS)
        return kr;

    simple_lock(&cap_lock);
    kr = cap_check_locked(&t, op, resource_id);
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
    simple_unlock(&cap_lock);
    return KERN_SUCCESS;
}

kern_return_t
urmach_cap_register(struct uros_cap *user_token)
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
        cap_key_set = TRUE;
        cap_server_task = caller;
        simple_unlock(&cap_lock);
        printf("cap: hmac key registered (len=%u)\n",
               (unsigned)CAP_HMAC_SIZE);
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
