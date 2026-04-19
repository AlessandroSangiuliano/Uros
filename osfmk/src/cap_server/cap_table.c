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

#include "cap_table.h"

#include <string.h>

#include "hmac_sha256.h"

/*
 * TSC read — i386 inline asm.  OSF Mach userspace does not ship
 * mach_absolute_time(), so we read the time-stamp counter directly.
 * Only used once at startup to seed the HMAC key, so the lack of
 * serialization (lfence/rdtscp) is not a concern.
 */
static inline uint64_t cap_tsc_read(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * Issue B will move the HMAC key into the UrMach kernel (set via
 * urmach_cap_register and consulted by urmach_cap_verify).  For Issue A
 * we keep it in cap_server memory, initialized at startup from a mix of
 * available entropy sources.
 */
static uint8_t cap_hmac_key[SHA256_DIGEST_SIZE];

/*
 * Hash index sizes.  256 buckets is generous for the v1 workload
 * (ext_server + default_pager + bootstrap open a handful of disks each).
 * Sharding arrives with slab + SMP (#80).
 */
#define CAP_BUCKETS   256

static struct cap_entry *bucket_by_id    [CAP_BUCKETS];
static struct cap_entry *bucket_by_owner [CAP_BUCKETS];
static struct cap_entry *bucket_by_parent[CAP_BUCKETS];

static uint64_t next_cap_id;

/* v1: single global mutex.  pthreads is already linked for every
 * server, and Issue A is single-threaded anyway.  No lock taken yet —
 * acquired via cap_table_lock()/unlock() once the main loop migrates
 * to multi-thread demux in a follow-up. */

static inline unsigned hash64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (unsigned)(x & (CAP_BUCKETS - 1));
}

static inline unsigned hash_port(mach_port_t p) {
    return hash64((uint64_t)p);
}

void cap_table_init(void) {
    for (unsigned i = 0; i < CAP_BUCKETS; i++) {
        bucket_by_id[i]     = NULL;
        bucket_by_owner[i]  = NULL;
        bucket_by_parent[i] = NULL;
    }
    next_cap_id = 1;
}

uint64_t cap_table_alloc_id(void) {
    return __atomic_fetch_add(&next_cap_id, 1, __ATOMIC_SEQ_CST);
}

int cap_table_insert(struct cap_entry *e) {
    if (!e) return -1;
    unsigned h = hash64(e->token.cap_id);
    e->next_by_id = bucket_by_id[h];
    bucket_by_id[h] = e;

    h = hash_port(e->sender);
    e->next_by_owner = bucket_by_owner[h];
    bucket_by_owner[h] = e;

    if (e->token.parent_cap_id != 0) {
        h = hash64(e->token.parent_cap_id);
        e->next_by_parent = bucket_by_parent[h];
        bucket_by_parent[h] = e;
    } else {
        e->next_by_parent = NULL;
    }
    return 0;
}

struct cap_entry *cap_table_find_by_id(uint64_t cap_id) {
    for (struct cap_entry *p = bucket_by_id[hash64(cap_id)];
         p; p = p->next_by_id) {
        if (p->token.cap_id == cap_id) return p;
    }
    return NULL;
}

struct cap_entry *cap_table_find_children(uint64_t parent_id) {
    return bucket_by_parent[hash64(parent_id)];
    /* Caller walks via next_by_parent and filters on token.parent_cap_id
     * (since a bucket may hold entries with different parents). */
}

void cap_table_mark_revoked(struct cap_entry *e) {
    e->token.revoked = 1;
    /* Re-sign so the revoked=1 bit is part of the HMAC.  This lets a
     * cheaply-cached token copy become invalid if ever replayed. */
    cap_sign(&e->token);
}

/*
 * The HMAC input is every byte of uros_cap up to (not including) the
 * hmac[] field.  The field order in the struct is wire-format, so a
 * single memcpy-style pass over [&token, &token.hmac) works.
 */
static inline size_t hmac_input_size(void) {
    return offsetof(struct uros_cap, hmac);
}

void cap_sign(struct uros_cap *token) {
    hmac_sha256(cap_hmac_key, sizeof(cap_hmac_key),
                token, hmac_input_size(),
                token->hmac);
}

int cap_verify_hmac(const struct uros_cap *token) {
    uint8_t expected[HMAC_SHA256_SIZE];
    hmac_sha256(cap_hmac_key, sizeof(cap_hmac_key),
                token, hmac_input_size(),
                expected);
    return hmac_sha256_equal(expected, token->hmac);
}

/*
 * Bootstrap entropy: a defensive mix of sources available pre-Issue-B.
 *
 * - cap_tsc_read() is a raw rdtsc — non-zero and boot-time varying
 * - mach_task_self() port name varies per boot
 * - a fixed salt disambiguates distinct uros_cap instances on a shared
 *   kernel image (meaningful once SMP lands)
 *
 * The mix is hashed with SHA-256 to derive a 32-byte key.  Issue B
 * replaces this with a kernel-generated key stored in cap_hmac_key and
 * consulted inside urmach_cap_verify.
 */
void cap_key_init(void) {
    struct {
        uint64_t tsc;
        uint32_t self;
        uint32_t salt[6];
    } seed;

    seed.tsc = cap_tsc_read();
    seed.self = (uint32_t)mach_task_self();
    /* ASCII "UrMach_cap_v1_\0\0" — constant, just spreads the hash input. */
    seed.salt[0] = 0x4d724855; /* 'UrMa' */
    seed.salt[1] = 0x635f6863; /* 'ch_c' */
    seed.salt[2] = 0x765f7061; /* 'ap_v' */
    seed.salt[3] = 0x00003100; /* '1\0\0\0' */
    seed.salt[4] = 0x51b2a64f;
    seed.salt[5] = 0x9e87cd32;

    sha256(&seed, sizeof(seed), cap_hmac_key);
}
