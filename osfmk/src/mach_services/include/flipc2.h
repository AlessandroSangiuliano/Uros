/*
 * FLIPC v2 — Fast Local IPC Channel
 *
 * Copyright (c) 2026 Alessandro Sangiuliano <alex22_7@hotmail.com>
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

#ifndef _FLIPC2_H_
#define _FLIPC2_H_

#include <mach/mach_types.h>
#include <mach/sync.h>
#include <mach/sync_policy.h>
#include <stdint.h>

/*
 * FLIPC v2: point-to-point shared-memory IPC channel for trusted servers.
 *
 * Architecture:
 *   - Control plane: lock-free descriptor ring (64-byte entries)
 *   - Data plane: inline data region + page-mapped zero-copy regions
 *   - Adaptive wakeup: spin-poll -> semaphore fallback
 *   - Kernel involvement: setup only (alloc, map, semaphore creation)
 *
 * The channel shared memory layout:
 *
 *   +---------------------+
 *   |  Channel Header     |  256 bytes
 *   +---------------------+
 *   |  Descriptor Ring    |  ring_entries * 64 bytes
 *   +---------------------+
 *   |  Data Region        |  remainder
 *   +---------------------+
 */

/* ------------------------------------------------------------------ */
/*  Library version                                                     */
/* ------------------------------------------------------------------ */

#define FLIPC2_LIB_VERSION_MAJOR    0
#define FLIPC2_LIB_VERSION_MINOR    0
#define FLIPC2_LIB_VERSION_PATCH    3
#define FLIPC2_LIB_VERSION_STRING   "0.0.3"

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define FLIPC2_MAGIC            0x464C5032      /* "FLP2" */
#define FLIPC2_VERSION          1

#define FLIPC2_CHANNEL_SIZE_MIN     4096        /* 4 KB   */
#define FLIPC2_CHANNEL_SIZE_MAX     (16*1024*1024) /* 16 MB */
#define FLIPC2_CHANNEL_SIZE_DEFAULT (256*1024)  /* 256 KB */

#define FLIPC2_RING_ENTRIES_MIN     16
#define FLIPC2_RING_ENTRIES_MAX     16384
#define FLIPC2_RING_ENTRIES_DEFAULT 256

#define FLIPC2_HEADER_SIZE          256
#define FLIPC2_DESC_SIZE            64

#define FLIPC2_SPIN_DEFAULT         4096

/* Layout flags (stored in header->layout_flags, auto-detected by attach) */
#define FLIPC2_LAYOUT_FLAT          0x00000000
#define FLIPC2_LAYOUT_ISOLATED      0x00000001

/* Creation flags (passed to flipc2_channel_create_ex) */
#define FLIPC2_CREATE_ISOLATED      0x00000001
#define FLIPC2_CREATE_FLAT          0x00000002  /* force flat layout */

/* Sharing roles (passed to flipc2_channel_share_isolated) */
#define FLIPC2_ROLE_PRODUCER        1
#define FLIPC2_ROLE_CONSUMER        2

/* Isolated layout page offsets (i386, PAGE_SIZE=4096) */
#define FLIPC2_ISO_CONST_OFFSET     0
#define FLIPC2_ISO_PROD_OFFSET      4096
#define FLIPC2_ISO_CONS_OFFSET      8192
#define FLIPC2_ISO_RING_OFFSET      12288

/* Minimum channel size for isolated layout */
#define FLIPC2_CHANNEL_SIZE_MIN_ISOLATED   16384

/* Descriptor flags */
#define FLIPC2_FLAG_DATA_INLINE     0x00000000  /* data in inline region    */
#define FLIPC2_FLAG_DATA_MAPPED     0x00000001  /* data in page-mapped region */
#define FLIPC2_FLAG_BATCH           0x00000002  /* more descriptors follow  */
#define FLIPC2_FLAG_DATA_BUFGROUP   0x00000004  /* data in buffer group pool */

/* ------------------------------------------------------------------ */
/*  Return codes                                                       */
/* ------------------------------------------------------------------ */

typedef int flipc2_return_t;

#define FLIPC2_SUCCESS                   0
#define FLIPC2_ERR_INVALID_ARGUMENT     -1
#define FLIPC2_ERR_RESOURCE_SHORTAGE    -2
#define FLIPC2_ERR_CHANNEL_FULL         -3
#define FLIPC2_ERR_CHANNEL_EMPTY        -4
#define FLIPC2_ERR_NOT_CONNECTED        -5
#define FLIPC2_ERR_ALREADY_CONNECTED    -6
#define FLIPC2_ERR_KERNEL               -7
#define FLIPC2_ERR_DATA_REGION_FULL     -8
#define FLIPC2_ERR_SIZE_ALIGNMENT       -9
#define FLIPC2_ERR_MAP_FAILED           -10
#define FLIPC2_ERR_RING_CORRUPT         -11  /* prod_tail/cons_head inconsistent */
#define FLIPC2_ERR_NAME_EXISTS          -12  /* endpoint name already registered */
#define FLIPC2_ERR_NAME_NOT_FOUND       -13  /* endpoint name not found          */
#define FLIPC2_ERR_MAX_CLIENTS          -14  /* endpoint client limit reached    */
#define FLIPC2_ERR_PEER_DEAD            -15  /* peer task has died               */
#define FLIPC2_ERR_POOL_EMPTY           -16  /* buffer group pool exhausted      */

/*
 * Maximum consecutive spurious wakeups before declaring the channel dead.
 * A spurious wakeup means semaphore_wait returned but no data is available,
 * typically caused by a malicious peer spamming semaphore_signal (DoS).
 */
#define FLIPC2_MAX_SPURIOUS_WAKEUPS     64

/* ------------------------------------------------------------------ */
/*  Memory fences                                                      */
/* ------------------------------------------------------------------ */

/*
 * On i386/x86: stores are not reordered with other stores, and loads are
 * not reordered with other loads.  We only need a compiler barrier for
 * the write fence, and a full barrier for the rare read-after-write case.
 */
#if defined(__i386__) || defined(__x86_64__)
#define FLIPC2_WRITE_FENCE()    __asm__ __volatile__("" ::: "memory")
#define FLIPC2_READ_FENCE()     __asm__ __volatile__("" ::: "memory")
#define FLIPC2_FULL_FENCE()     __asm__ __volatile__("mfence" ::: "memory")
#define FLIPC2_PAUSE()          __asm__ __volatile__("pause")
#else
#define FLIPC2_WRITE_FENCE()    __sync_synchronize()
#define FLIPC2_READ_FENCE()     __sync_synchronize()
#define FLIPC2_FULL_FENCE()     __sync_synchronize()
#define FLIPC2_PAUSE()          ((void)0)
#endif

/* ------------------------------------------------------------------ */
/*  Descriptor                                                         */
/* ------------------------------------------------------------------ */

struct flipc2_desc {
    uint32_t    opcode;         /* operation type                        */
    uint32_t    flags;          /* FLIPC2_FLAG_*                         */
    uint64_t    cookie;         /* request/reply correlation ID          */
    uint64_t    data_offset;    /* offset in data/mapped region (0=none) */
    uint64_t    data_length;    /* payload length in bytes               */
    uint64_t    param[3];       /* opcode-specific parameters            */
    uint32_t    status;         /* completion status (replies)           */
    uint32_t    _reserved;      /* pad to 64 bytes                       */
};

/* ------------------------------------------------------------------ */
/*  Channel Header (256 bytes, at offset 0 of shared memory)           */
/* ------------------------------------------------------------------ */

struct flipc2_channel_header {
    /*
     * Cache line 0 (bytes 0–63): constants + producer pointer.
     * Constants are set once at creation and never modified.
     * prod_tail is producer-owned, read by consumer.
     */
    uint32_t    magic;              /* FLIPC2_MAGIC                      */
    uint32_t    version;            /* FLIPC2_VERSION                    */
    uint32_t    channel_size;       /* total shared memory size          */
    uint32_t    ring_offset;        /* byte offset of descriptor ring    */
    uint32_t    ring_entries;       /* number of ring slots (power of 2) */
    uint32_t    ring_mask;          /* ring_entries - 1                  */
    uint32_t    data_offset;        /* byte offset of data region        */
    uint32_t    data_size;          /* data region size in bytes         */
    uint32_t    desc_size;          /* sizeof(struct flipc2_desc)        */
    mach_port_t wakeup_sem;         /* consumer wakeup semaphore         */
    mach_port_t wakeup_sem_prod;    /* producer wakeup semaphore         */
    uint32_t    layout_flags;       /* 0=flat, FLIPC2_LAYOUT_ISOLATED    */
    volatile uint32_t prod_tail;    /* next ring slot to write           */
    volatile uint32_t prod_sleeping;/* 1 = producer blocked on full ring */
    uint32_t    _pad_prod[2];       /* pad to 64 bytes                   */

    /*
     * Cache line 1 (bytes 64–127): consumer-owned.
     * cons_head and cons_sleeping are written by consumer, read by producer.
     */
    volatile uint32_t   cons_head;      /* next ring slot to read        */
    volatile uint32_t   cons_sleeping;  /* 1 = blocked on semaphore      */
    uint32_t            _pad_cons[14];  /* fill cache line               */

    /*
     * Cache line 2 (bytes 128–191): statistics (best-effort).
     */
    volatile uint64_t   prod_total;     /* total descriptors produced    */
    volatile uint64_t   cons_total;     /* total descriptors consumed    */
    volatile uint64_t   wakeups;        /* semaphore_signal count        */
    uint8_t             _pad_stats[64 - 24]; /* fill cache line          */

    /*
     * Cache line 3 (bytes 192–255): reserved for future use.
     */
    uint8_t             _reserved[64];
};

/* Compile-time check: header must be exactly 256 bytes */
typedef char _flipc2_header_size_check
    [(sizeof(struct flipc2_channel_header) == FLIPC2_HEADER_SIZE) ? 1 : -1];

/* ------------------------------------------------------------------ */
/*  Channel handle (userspace)                                         */
/* ------------------------------------------------------------------ */

typedef struct flipc2_channel {
    struct flipc2_channel_header *hdr;  /* mapped shared memory base     */
    struct flipc2_desc           *ring; /* &hdr + ring_offset            */
    uint8_t                      *data; /* &hdr + data_offset            */
    mach_port_t                   mem_port; /* memory object port        */

    /*
     * Direct pointers to volatile shared fields.
     * Flat layout: point into the 256-byte header.
     * Isolated layout: point to separate pages.
     * All fast-path inline functions use these (layout-agnostic).
     */
    volatile uint32_t   *prod_tail;
    volatile uint32_t   *prod_sleeping;
    volatile uint64_t   *prod_total;
    volatile uint64_t   *wakeups;
    volatile uint32_t   *cons_head;
    volatile uint32_t   *cons_sleeping;
    volatile uint64_t   *cons_total;

    /* Cached constants from header (avoid ch->hdr-> on fast path) */
    uint32_t    ring_entries;
    uint32_t    ring_mask;

    /* producer-side cached copy of cons_head (avoid false sharing) */
    uint32_t    cached_cons_head;
    /* consumer-side cached copy of prod_tail */
    uint32_t    cached_prod_tail;

    /*
     * mapped_size > 0: this handle owns a vm_remap'd mapping of this size.
     * flipc2_channel_detach() will vm_deallocate the region.
     * Set by flipc2_channel_attach_remote() for inter-task peers.
     * 0 = intra-task peer, does not own the mapping.
     */
    uint32_t    mapped_size;

    /*
     * Spurious wakeup counter (consumer-side).
     * Incremented when semaphore_wait returns but no data is available.
     * Reset to zero on every successful consume.
     * If this exceeds FLIPC2_MAX_SPURIOUS_WAKEUPS, the channel is
     * considered compromised (DoS) and wait functions return NULL.
     */
    uint32_t    spurious_wakeups;

    /*
     * Per-handle semaphore port names (local IPC space).
     * For intra-task handles these match hdr->wakeup_sem/wakeup_sem_prod.
     * For inter-task handles the client receives its own send rights
     * (different port names than the server) via MIG or insert_right.
     * All inline fast-path functions use these instead of the header fields.
     */
    mach_port_t sem_port;           /* consumer wakeup semaphore  */
    mach_port_t sem_port_prod;      /* producer wakeup semaphore  */

    /*
     * Associated buffer group (or NULL).
     * When set, flipc2_resolve_data() uses the buffer group pool
     * for descriptors with FLIPC2_FLAG_DATA_BUFGROUP.
     */
    struct flipc2_bufgroup *bufgroup;
} *flipc2_channel_t;

/* ------------------------------------------------------------------ */
/*  Ring index helpers                                                  */
/* ------------------------------------------------------------------ */

static inline uint32_t
flipc2_ring_idx(struct flipc2_channel_header *hdr, uint32_t ptr)
{
    return ptr & hdr->ring_mask;
}

static inline uint32_t
flipc2_ring_count(struct flipc2_channel_header *hdr,
                  uint32_t tail, uint32_t head)
{
    return tail - head;
}

static inline uint32_t
flipc2_ring_free(struct flipc2_channel_header *hdr,
                 uint32_t tail, uint32_t head)
{
    return hdr->ring_entries - (tail - head);
}

/* ------------------------------------------------------------------ */
/*  Fast-path: produce (inline)                                        */
/* ------------------------------------------------------------------ */

/*
 * Reserve the next ring slot for writing.
 * Returns pointer to the descriptor, or NULL if ring is full or corrupt.
 */
static inline struct flipc2_desc *
flipc2_produce_reserve(flipc2_channel_t ch)
{
    uint32_t tail = *ch->prod_tail;
    uint32_t re = ch->ring_entries;
    uint32_t space;

    /* Check cached cons_head first (avoid volatile read) */
    space = re - (tail - ch->cached_cons_head);
    if (space == 0 || space > re) {
        /* Refresh from volatile */
        ch->cached_cons_head = *ch->cons_head;
        FLIPC2_READ_FENCE();
        space = re - (tail - ch->cached_cons_head);
        if (space == 0)
            return (struct flipc2_desc *)0;
        /* Ring corruption: cons_head was pushed past prod_tail */
        if (space > re)
            return (struct flipc2_desc *)0;
    }

    return &ch->ring[tail & ch->ring_mask];
}

/*
 * Commit one descriptor: advance prod_tail, wake consumer if sleeping.
 *
 * Fence correctness: on x86 stores are naturally ordered, so the
 * store to prod_tail is visible before the load of cons_sleeping.
 * The compiler barrier (FLIPC2_WRITE_FENCE on x86) prevents the
 * compiler from reordering.  On ARM/RISC-V the fallback
 * __sync_synchronize() provides a full barrier.
 */
static inline void
flipc2_produce_commit(flipc2_channel_t ch)
{
    FLIPC2_WRITE_FENCE();
    (*ch->prod_tail)++;
    (*ch->prod_total)++;

    FLIPC2_WRITE_FENCE();
    if (*ch->cons_sleeping) {
        semaphore_signal(ch->sem_port);
        (*ch->wakeups)++;
    }
}

/*
 * Commit N descriptors in batch.
 */
static inline void
flipc2_produce_commit_n(flipc2_channel_t ch, uint32_t n)
{
    FLIPC2_WRITE_FENCE();
    *ch->prod_tail += n;
    *ch->prod_total += n;

    FLIPC2_WRITE_FENCE();
    if (*ch->cons_sleeping) {
        semaphore_signal(ch->sem_port);
        (*ch->wakeups)++;
    }
}

/* ------------------------------------------------------------------ */
/*  Fast-path: consume (inline)                                        */
/* ------------------------------------------------------------------ */

/*
 * Peek at the next available descriptor.
 * Returns pointer to the descriptor, or NULL if ring is empty or corrupt.
 */
static inline struct flipc2_desc *
flipc2_consume_peek(flipc2_channel_t ch)
{
    uint32_t head = *ch->cons_head;
    uint32_t re = ch->ring_entries;
    uint32_t avail;

    /* Check cached prod_tail first */
    avail = ch->cached_prod_tail - head;
    if (avail == 0 || avail > re) {
        FLIPC2_READ_FENCE();
        ch->cached_prod_tail = *ch->prod_tail;
        avail = ch->cached_prod_tail - head;
        if (avail == 0)
            return (struct flipc2_desc *)0;
        /* Ring corruption: prod_tail jumped past ring capacity */
        if (avail > re)
            return (struct flipc2_desc *)0;
    }

    return &ch->ring[head & ch->ring_mask];
}

/*
 * Release one descriptor: advance cons_head, wake producer if blocked.
 */
static inline void
flipc2_consume_release(flipc2_channel_t ch)
{
    FLIPC2_WRITE_FENCE();
    (*ch->cons_head)++;
    (*ch->cons_total)++;

    FLIPC2_WRITE_FENCE();
    if (*ch->prod_sleeping) {
        semaphore_signal(ch->sem_port_prod);
    }
}

/*
 * Release N descriptors in batch, wake producer if blocked.
 */
static inline void
flipc2_consume_release_n(flipc2_channel_t ch, uint32_t n)
{
    FLIPC2_WRITE_FENCE();
    *ch->cons_head += n;
    *ch->cons_total += n;

    FLIPC2_WRITE_FENCE();
    if (*ch->prod_sleeping) {
        semaphore_signal(ch->sem_port_prod);
    }
}

/*
 * Wait for a descriptor: spin for spin_count iterations, then block
 * on the Mach semaphore.  Returns pointer to the descriptor, or NULL
 * if the ring is corrupt or the peer is spamming spurious wakeups.
 *
 * This implements the adaptive wakeup protocol:
 *   1. Spin-poll prod_tail for spin_count iterations
 *   2. Set cons_sleeping = 1
 *   3. Write fence + re-check (avoid lost wakeup)
 *   4. semaphore_wait()
 *   5. Verify data actually arrived (spurious wakeup defense)
 *   6. Clear cons_sleeping
 *
 * Lost wakeup safety: Mach semaphores are counting semaphores.
 * If the producer calls semaphore_signal() between step 2 and step 4,
 * the signal is not lost — it increments the semaphore count, so the
 * subsequent semaphore_wait() returns immediately.
 *
 * DoS defense: if semaphore_wait returns but no data is available,
 * this is a spurious wakeup (malicious semaphore_signal spam).
 * After FLIPC2_MAX_SPURIOUS_WAKEUPS consecutive spurious wakeups,
 * the channel is declared dead and NULL is returned.
 */
static inline struct flipc2_desc *
flipc2_consume_wait(flipc2_channel_t ch, uint32_t spin_count)
{
    uint32_t head = *ch->cons_head;
    uint32_t re = ch->ring_entries;
    uint32_t avail;
    uint32_t i;

    /* Phase 1: spin-poll */
    for (i = 0; i < spin_count; i++) {
        FLIPC2_READ_FENCE();
        ch->cached_prod_tail = *ch->prod_tail;
        if (ch->cached_prod_tail != head) {
            avail = ch->cached_prod_tail - head;
            if (avail > re)
                return (struct flipc2_desc *)0;  /* ring corrupt */
            ch->spurious_wakeups = 0;
            return &ch->ring[head & ch->ring_mask];
        }
        FLIPC2_PAUSE();
    }

    /* Phase 2: prepare to sleep */
    *ch->cons_sleeping = 1;
    FLIPC2_WRITE_FENCE();

    /* Re-check to avoid lost wakeup */
    ch->cached_prod_tail = *ch->prod_tail;
    FLIPC2_READ_FENCE();
    if (ch->cached_prod_tail != head) {
        *ch->cons_sleeping = 0;
        avail = ch->cached_prod_tail - head;
        if (avail > re)
            return (struct flipc2_desc *)0;
        ch->spurious_wakeups = 0;
        return &ch->ring[head & ch->ring_mask];
    }

    /* Phase 3: block, then verify data actually arrived */
    semaphore_wait(ch->sem_port);
    *ch->cons_sleeping = 0;

    ch->cached_prod_tail = *ch->prod_tail;
    FLIPC2_READ_FENCE();
    avail = ch->cached_prod_tail - head;

    if (avail == 0) {
        /* Spurious wakeup — peer signaled without producing data */
        ch->spurious_wakeups++;
        if (ch->spurious_wakeups >= FLIPC2_MAX_SPURIOUS_WAKEUPS)
            return (struct flipc2_desc *)0;  /* channel dead (DoS) */
        /* Retry: re-enter wait (recursive tail call) */
        return flipc2_consume_wait(ch, 0);
    }

    if (avail > re)
        return (struct flipc2_desc *)0;  /* ring corrupt */

    ch->spurious_wakeups = 0;
    return &ch->ring[head & ch->ring_mask];
}

/*
 * Wait for a descriptor with timeout (milliseconds).
 * Returns pointer to the descriptor, or NULL on timeout, corruption,
 * or DoS (spurious wakeup threshold exceeded).
 *
 * Same adaptive protocol as flipc2_consume_wait, but uses
 * semaphore_timedwait to avoid blocking indefinitely when
 * the producer dies or is too slow.
 */
static inline struct flipc2_desc *
flipc2_consume_timedwait(flipc2_channel_t ch, uint32_t spin_count,
                         uint32_t timeout_ms)
{
    uint32_t head = *ch->cons_head;
    uint32_t re = ch->ring_entries;
    uint32_t avail;
    uint32_t i;
    kern_return_t kr;
    tvalspec_t ts;

    /* Phase 1: spin-poll */
    for (i = 0; i < spin_count; i++) {
        FLIPC2_READ_FENCE();
        ch->cached_prod_tail = *ch->prod_tail;
        if (ch->cached_prod_tail != head) {
            avail = ch->cached_prod_tail - head;
            if (avail > re)
                return (struct flipc2_desc *)0;
            ch->spurious_wakeups = 0;
            return &ch->ring[head & ch->ring_mask];
        }
        FLIPC2_PAUSE();
    }

    /* Phase 2: prepare to sleep */
    *ch->cons_sleeping = 1;
    FLIPC2_WRITE_FENCE();

    ch->cached_prod_tail = *ch->prod_tail;
    FLIPC2_READ_FENCE();
    if (ch->cached_prod_tail != head) {
        *ch->cons_sleeping = 0;
        avail = ch->cached_prod_tail - head;
        if (avail > re)
            return (struct flipc2_desc *)0;
        ch->spurious_wakeups = 0;
        return &ch->ring[head & ch->ring_mask];
    }

    /* Phase 3: block with timeout */
    ts.tv_sec  = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000;
    kr = semaphore_timedwait(ch->sem_port, ts);
    *ch->cons_sleeping = 0;

    if (kr != KERN_SUCCESS)
        return (struct flipc2_desc *)0;

    ch->cached_prod_tail = *ch->prod_tail;
    FLIPC2_READ_FENCE();
    avail = ch->cached_prod_tail - head;

    if (avail == 0) {
        ch->spurious_wakeups++;
        if (ch->spurious_wakeups >= FLIPC2_MAX_SPURIOUS_WAKEUPS)
            return (struct flipc2_desc *)0;
        return flipc2_consume_timedwait(ch, 0, timeout_ms);
    }

    if (avail > re)
        return (struct flipc2_desc *)0;

    ch->spurious_wakeups = 0;
    return &ch->ring[head & ch->ring_mask];
}

/*
 * Reserve a ring slot, blocking if the ring is full.
 *
 * Same adaptive protocol as flipc2_consume_wait but for the producer:
 * spin-poll cons_head, then block on wakeup_sem_prod until the
 * consumer releases a slot.  flipc2_consume_release() signals
 * the producer semaphore when prod_sleeping is set.
 */
static inline struct flipc2_desc *
flipc2_produce_wait(flipc2_channel_t ch, uint32_t spin_count)
{
    uint32_t tail = *ch->prod_tail;
    uint32_t re = ch->ring_entries;
    uint32_t space;
    uint32_t i;

    /* Try fast path first */
    space = re - (tail - ch->cached_cons_head);
    if (space == 0 || space > re) {
        ch->cached_cons_head = *ch->cons_head;
        FLIPC2_READ_FENCE();
        space = re - (tail - ch->cached_cons_head);
    }
    if (space > 0 && space <= re)
        return &ch->ring[tail & ch->ring_mask];
    if (space > re)
        return (struct flipc2_desc *)0;  /* ring corrupt */

    /* Phase 1: spin-poll */
    for (i = 0; i < spin_count; i++) {
        FLIPC2_READ_FENCE();
        ch->cached_cons_head = *ch->cons_head;
        space = re - (tail - ch->cached_cons_head);
        if (space > 0 && space <= re)
            return &ch->ring[tail & ch->ring_mask];
        if (space > re)
            return (struct flipc2_desc *)0;
        FLIPC2_PAUSE();
    }

    /* Phase 2: prepare to sleep */
    *ch->prod_sleeping = 1;
    FLIPC2_WRITE_FENCE();

    ch->cached_cons_head = *ch->cons_head;
    FLIPC2_READ_FENCE();
    space = re - (tail - ch->cached_cons_head);
    if (space > 0 && space <= re) {
        *ch->prod_sleeping = 0;
        return &ch->ring[tail & ch->ring_mask];
    }
    if (space > re) {
        *ch->prod_sleeping = 0;
        return (struct flipc2_desc *)0;
    }

    /* Phase 3: block on producer semaphore */
    semaphore_wait(ch->sem_port_prod);
    *ch->prod_sleeping = 0;

    ch->cached_cons_head = *ch->cons_head;
    space = re - (tail - ch->cached_cons_head);
    if (space == 0 || space > re)
        return (struct flipc2_desc *)0;
    return &ch->ring[tail & ch->ring_mask];
}

/*
 * How many descriptors are available for consumption?
 * Returns 0 if empty, or 0 if ring is corrupt (avail > ring_entries).
 */
static inline uint32_t
flipc2_available(flipc2_channel_t ch)
{
    uint32_t avail;
    FLIPC2_READ_FENCE();
    ch->cached_prod_tail = *ch->prod_tail;
    avail = ch->cached_prod_tail - *ch->cons_head;
    if (avail > ch->ring_entries)
        return 0;  /* ring corrupt */
    return avail;
}

/* ------------------------------------------------------------------ */
/*  Data region helpers                                                 */
/* ------------------------------------------------------------------ */

/*
 * Simple bump allocator for the inline data region.
 * NOT thread-safe — producer only.
 *
 * Returns offset from data region start, or (uint64_t)-1 on failure.
 * Allocations are 8-byte aligned.
 */
static inline void *
flipc2_data_ptr(flipc2_channel_t ch, uint64_t offset)
{
    return (void *)(ch->data + offset);
}

/*
 * Bounds-checked data pointer.
 * Returns NULL if offset + length exceeds the data region.
 * Use this for untrusted inputs; the unchecked flipc2_data_ptr()
 * is fine for hot paths with validated offsets.
 */
static inline void *
flipc2_data_ptr_safe(flipc2_channel_t ch, uint64_t offset, uint64_t length)
{
    if (offset + length > ch->hdr->data_size)
        return (void *)0;
    if (offset + length < offset)  /* overflow check */
        return (void *)0;
    return (void *)(ch->data + offset);
}

/* ------------------------------------------------------------------ */
/*  Buffer Group (shared memory pool for multi-channel data)           */
/* ------------------------------------------------------------------ */

#define FLIPC2_BUFGROUP_MAGIC           0x46425032  /* "FBP2" */
#define FLIPC2_BUFGROUP_VERSION         1
#define FLIPC2_BUFGROUP_SLOT_NONE       0xFFFFFFFF
#define FLIPC2_BUFGROUP_HEADER_SIZE     64

#define FLIPC2_BUFGROUP_SLOT_SIZE_MIN   64
#define FLIPC2_BUFGROUP_SLOT_SIZE_MAX   65536
#define FLIPC2_BUFGROUP_POOL_SIZE_MIN   4096
#define FLIPC2_BUFGROUP_POOL_SIZE_MAX   (16*1024*1024)

/*
 * Buffer group header (64 bytes, at offset 0 of pool shared memory).
 *
 * Layout:
 *   [0..63]   header
 *   [64..]    next[] array (slot_count * 4 bytes)
 *   [aligned] data region (slot_count * slot_stride bytes)
 *
 * The free list is a LIFO stack using a tagged pointer to prevent
 * the ABA problem:  low 32 bits = slot index (or SLOT_NONE),
 * high 32 bits = version counter.  On i686, CAS on the 64-bit
 * free_head uses cmpxchg8b.
 */
struct flipc2_bufgroup_header {
    uint32_t    magic;              /*  0: FLIPC2_BUFGROUP_MAGIC         */
    uint32_t    version;            /*  4: FLIPC2_BUFGROUP_VERSION       */
    uint32_t    pool_size;          /*  8: total shared memory size      */
    uint32_t    slot_size;          /* 12: usable data bytes per slot    */
    uint32_t    slot_count;         /* 16: total number of slots         */
    uint32_t    next_offset;        /* 20: byte offset of next[] array   */
    uint32_t    data_offset;        /* 24: byte offset of data region    */
    uint32_t    slot_stride;        /* 28: aligned slot size (>= slot)   */

    /* 32: Free list head — tagged pointer for ABA-safe CAS             */
    /*     low 32 bits = slot index (or SLOT_NONE)                      */
    /*     high 32 bits = version counter                               */
    volatile uint64_t free_head;    /* naturally 8-byte aligned at 32    */

    /* 40: Statistics (best-effort, not synchronized)                   */
    volatile uint64_t alloc_total;
    volatile uint64_t free_total;

    /* 56: pad to 64 bytes                                              */
    uint8_t     _reserved[8];
};

/* Compile-time check: header must be exactly 64 bytes */
typedef char _flipc2_bufgroup_header_size_check
    [(sizeof(struct flipc2_bufgroup_header) == FLIPC2_BUFGROUP_HEADER_SIZE) ? 1 : -1];

/*
 * Buffer group handle (userspace).
 */
typedef struct flipc2_bufgroup {
    struct flipc2_bufgroup_header *hdr;   /* mapped shared memory base  */
    uint32_t                     *next;   /* &hdr + next_offset         */
    uint8_t                      *data;   /* &hdr + data_offset         */
    uint32_t                      mapped_size; /* > 0: owns vm_remap    */
} *flipc2_bufgroup_t;

/* ------------------------------------------------------------------ */
/*  Buffer group: inline fast-path                                     */
/* ------------------------------------------------------------------ */

static inline uint32_t
flipc2_bufgroup_head_index(uint64_t tagged)
{
    return (uint32_t)(tagged & 0xFFFFFFFF);
}

static inline uint32_t
flipc2_bufgroup_head_version(uint64_t tagged)
{
    return (uint32_t)(tagged >> 32);
}

static inline uint64_t
flipc2_bufgroup_make_head(uint32_t index, uint32_t version)
{
    return ((uint64_t)version << 32) | (uint64_t)index;
}

/*
 * Allocate a buffer slot from the pool (CAS-based LIFO pop).
 * Returns byte offset into the data region, or (uint64_t)-1 if empty.
 */
static inline uint64_t
flipc2_bufgroup_alloc(flipc2_bufgroup_t bg)
{
    struct flipc2_bufgroup_header *hdr = bg->hdr;

    for (;;) {
        uint64_t old_tagged = hdr->free_head;
        uint32_t idx = flipc2_bufgroup_head_index(old_tagged);
        uint32_t ver = flipc2_bufgroup_head_version(old_tagged);
        uint32_t next_idx;
        uint64_t new_tagged;

        if (idx == FLIPC2_BUFGROUP_SLOT_NONE)
            return (uint64_t)-1;

        FLIPC2_READ_FENCE();
        next_idx = bg->next[idx];
        new_tagged = flipc2_bufgroup_make_head(next_idx, ver + 1);

        if (__sync_val_compare_and_swap(&hdr->free_head,
                                        old_tagged, new_tagged) == old_tagged) {
            hdr->alloc_total++;
            return (uint64_t)idx * (uint64_t)hdr->slot_stride;
        }
        FLIPC2_PAUSE();
    }
}

/*
 * Free a buffer slot back to the pool (CAS-based LIFO push).
 */
static inline void
flipc2_bufgroup_free(flipc2_bufgroup_t bg, uint64_t offset)
{
    struct flipc2_bufgroup_header *hdr = bg->hdr;
    uint32_t slot = (uint32_t)offset / hdr->slot_stride;

    for (;;) {
        uint64_t old_tagged = hdr->free_head;
        uint32_t old_idx = flipc2_bufgroup_head_index(old_tagged);
        uint32_t ver = flipc2_bufgroup_head_version(old_tagged);
        uint64_t new_tagged;

        bg->next[slot] = old_idx;
        FLIPC2_WRITE_FENCE();

        new_tagged = flipc2_bufgroup_make_head(slot, ver + 1);

        if (__sync_val_compare_and_swap(&hdr->free_head,
                                        old_tagged, new_tagged) == old_tagged) {
            hdr->free_total++;
            return;
        }
        FLIPC2_PAUSE();
    }
}

/*
 * Get data pointer for an allocated slot.
 */
static inline void *
flipc2_bufgroup_data(flipc2_bufgroup_t bg, uint64_t offset)
{
    return (void *)(bg->data + offset);
}

/*
 * Bounds-checked data pointer for buffer group.
 */
static inline void *
flipc2_bufgroup_data_safe(flipc2_bufgroup_t bg, uint64_t offset, uint64_t length)
{
    uint32_t slot = (uint32_t)offset / bg->hdr->slot_stride;

    if (slot >= bg->hdr->slot_count)
        return (void *)0;
    if (length > bg->hdr->slot_size)
        return (void *)0;
    if (offset + length < offset)
        return (void *)0;
    return (void *)(bg->data + offset);
}

/*
 * Resolve data pointer based on descriptor flags.
 * Dispatches to buffer group or inline data region.
 */
static inline void *
flipc2_resolve_data(flipc2_channel_t ch, uint32_t flags, uint64_t offset)
{
    if (flags & FLIPC2_FLAG_DATA_BUFGROUP)
        return flipc2_bufgroup_data(ch->bufgroup, offset);
    return flipc2_data_ptr(ch, offset);
}

/* ------------------------------------------------------------------ */
/*  Setup API (slow path — implemented in libflipc2, uses Mach IPC)    */
/* ------------------------------------------------------------------ */

/*
 * Create a new channel.
 *
 * channel_size:  total shared memory (FLIPC2_CHANNEL_SIZE_MIN..MAX)
 * ring_entries:  descriptor ring slots (power of 2, MIN..MAX)
 * channel:       [out] channel handle
 * sem_port:      [out] semaphore port (store or send to peer)
 */
flipc2_return_t
flipc2_channel_create(
    uint32_t            channel_size,
    uint32_t            ring_entries,
    flipc2_channel_t   *channel,
    mach_port_t        *sem_port);

/*
 * Create a new channel with flags.
 *
 * channel_size:  total shared memory
 * ring_entries:  descriptor ring slots (power of 2)
 * flags:         FLIPC2_CREATE_ISOLATED or 0
 * channel:       [out] channel handle
 * sem_port:      [out] semaphore port
 *
 * When FLIPC2_CREATE_ISOLATED is set, the header is split into
 * page-aligned sections for per-role protections.  Minimum size
 * is FLIPC2_CHANNEL_SIZE_MIN_ISOLATED (16 KB).
 */
flipc2_return_t
flipc2_channel_create_ex(
    uint32_t            channel_size,
    uint32_t            ring_entries,
    uint32_t            flags,
    flipc2_channel_t   *channel,
    mach_port_t        *sem_port);

/*
 * Attach to an existing channel via its base address.
 *
 * Used by peer threads (same task), child tasks (inherited mapping),
 * or remote tasks that mapped the region via vm_remap.
 *
 * base_addr:     address of the channel header in this task's space
 * channel:       [out] channel handle
 */
flipc2_return_t
flipc2_channel_attach(
    vm_address_t        base_addr,
    flipc2_channel_t   *channel);

/*
 * Attach to a channel mapped via vm_remap (inter-task peer).
 *
 * Like flipc2_channel_attach, but records the mapped_size so that
 * flipc2_channel_detach() will vm_deallocate the shared memory
 * region.  Use this for inter-task peers that own their mapping.
 *
 * base_addr:     address of the channel header in this task's space
 * mapped_size:   size of the vm_remap'd region (channel_size)
 * channel:       [out] channel handle
 */
flipc2_return_t
flipc2_channel_attach_remote(
    vm_address_t        base_addr,
    uint32_t            mapped_size,
    flipc2_channel_t   *channel);

/*
 * Destroy the channel and release all resources.
 * Only the creator should call this.
 */
flipc2_return_t
flipc2_channel_destroy(
    flipc2_channel_t    channel);

/*
 * Detach from a channel without destroying it.
 * Peers (non-creators) call this.
 */
flipc2_return_t
flipc2_channel_detach(
    flipc2_channel_t    channel);

/*
 * Map pages from the producer's address space into the consumer
 * for zero-copy data plane.
 *
 * source_addr:   address in the producer's space
 * size:          mapping size (page-aligned)
 * region_id:     [out] region ID for use in descriptors
 */
flipc2_return_t
flipc2_channel_map_pages(
    flipc2_channel_t    channel,
    vm_offset_t         source_addr,
    vm_size_t           size,
    uint64_t           *region_id);

/*
 * Share a channel with another task via vm_remap (true shared memory).
 *
 * Maps the channel's shared memory region into target_task's address
 * space with VM_INHERIT_SHARE (not COW).  Both tasks read/write the
 * same physical pages.
 *
 * channel:       channel to share
 * target_task:   task to map the channel into
 * out_addr:      [out] address in target_task's space
 */
flipc2_return_t
flipc2_channel_share(
    flipc2_channel_t    channel,
    mach_port_t         target_task,
    vm_address_t       *out_addr);

/*
 * Share an isolated channel with per-role page protections.
 *
 * Maps the channel into target_task, then applies vm_protect per section:
 *   - Constants page: RO for both roles
 *   - Producer page: RW for producer, RO for consumer
 *   - Consumer page: RO for producer, RW for consumer
 *   - Ring + data: RW for producer, RO for consumer
 *
 * Requires channel created with FLIPC2_CREATE_ISOLATED.
 *
 * channel:       channel to share
 * target_task:   task to map the channel into
 * target_role:   FLIPC2_ROLE_PRODUCER or FLIPC2_ROLE_CONSUMER
 * out_addr:      [out] address in target_task's space
 */
flipc2_return_t
flipc2_channel_share_isolated(
    flipc2_channel_t    channel,
    mach_port_t         target_task,
    uint32_t            target_role,
    vm_address_t       *out_addr);

/*
 * Insert a semaphore port into another task's IPC space.
 *
 * The target task receives a send right to the semaphore under
 * the specified port name.
 *
 * sem:           semaphore port (send right)
 * target_task:   task to insert the right into
 * target_name:   port name in the target's IPC space
 */
flipc2_return_t
flipc2_semaphore_share(
    mach_port_t         sem,
    mach_port_t         target_task,
    mach_port_t         target_name);

/*
 * Override the per-handle semaphore port names.
 *
 * For inter-task peers: after flipc2_channel_attach_remote(), call
 * this with the semaphore send rights received via MIG to set the
 * correct local IPC-space port names.
 */
void
flipc2_channel_set_semaphores(
    flipc2_channel_t    ch,
    mach_port_t         sem,
    mach_port_t         sem_prod);

/* ------------------------------------------------------------------ */
/*  Endpoint API (named channel discovery + connection handshake)       */
/* ------------------------------------------------------------------ */

/* Opaque endpoint handle */
typedef struct flipc2_endpoint *flipc2_endpoint_t;

/*
 * Server-side: create and register a named endpoint.
 *
 * Registers a contact port with the Mach name server under the
 * given name.  Clients connect via flipc2_endpoint_connect().
 *
 * name:           endpoint name (up to 128 chars)
 * max_clients:    maximum simultaneous connections (1..64)
 * channel_size:   default channel size for new connections
 * ring_entries:   default ring entries for new connections
 * flags:          FLIPC2_CREATE_ISOLATED or 0
 * ep:             [out] endpoint handle
 */
flipc2_return_t
flipc2_endpoint_create(
    const char         *name,
    uint32_t            max_clients,
    uint32_t            channel_size,
    uint32_t            ring_entries,
    uint32_t            flags,
    flipc2_endpoint_t  *ep);

/*
 * Server-side: wait for a client to connect.
 *
 * Blocks until a client calls flipc2_endpoint_connect().
 * Creates a dedicated SPSC channel pair (fwd: server→client,
 * rev: client→server) and returns the server-side handles.
 *
 * ep:        endpoint handle
 * fwd_ch:    [out] forward channel (server produces, client consumes)
 * rev_ch:    [out] reverse channel (client produces, server consumes)
 */
flipc2_return_t
flipc2_endpoint_accept(
    flipc2_endpoint_t   ep,
    flipc2_channel_t   *fwd_ch,
    flipc2_channel_t   *rev_ch);

/*
 * Server-side: tear down endpoint, disconnect all clients.
 *
 * Unregisters from the name server, destroys all active connections'
 * channels, and frees the endpoint handle.
 */
flipc2_return_t
flipc2_endpoint_destroy(
    flipc2_endpoint_t   ep);

/*
 * Server-side: get the endpoint's receive port.
 *
 * Useful for adding to a custom port set for multiplexed receive.
 */
mach_port_t
flipc2_endpoint_port(
    flipc2_endpoint_t   ep);

/*
 * Client-side: connect to a named endpoint.
 *
 * Looks up the endpoint by name via the Mach name server, then
 * executes the connection handshake.  On success, returns a
 * channel pair with per-handle semaphore ports already set.
 *
 * name:           endpoint name
 * channel_size:   requested channel size (0 = use server default)
 * ring_entries:   requested ring entries (0 = use server default)
 * fwd_ch:         [out] forward channel (server→client, client consumes)
 * rev_ch:         [out] reverse channel (client→server, client produces)
 */
flipc2_return_t
flipc2_endpoint_connect(
    const char         *name,
    uint32_t            channel_size,
    uint32_t            ring_entries,
    flipc2_channel_t   *fwd_ch,
    flipc2_channel_t   *rev_ch);

/*
 * Client-side: connect to an endpoint by port (no name lookup).
 *
 * Same as flipc2_endpoint_connect but takes a send right to
 * the server's endpoint port directly.
 */
flipc2_return_t
flipc2_endpoint_connect_port(
    mach_port_t         server_port,
    uint32_t            channel_size,
    uint32_t            ring_entries,
    flipc2_channel_t   *fwd_ch,
    flipc2_channel_t   *rev_ch);

/*
 * Client-side: disconnect and detach from both channels.
 *
 * Detaches the channel pair and deallocates semaphore send rights.
 */
flipc2_return_t
flipc2_endpoint_disconnect(
    flipc2_channel_t    fwd_ch,
    flipc2_channel_t    rev_ch);

/* ------------------------------------------------------------------ */
/*  Buffer Group API (shared memory pool, implemented in libflipc2)    */
/* ------------------------------------------------------------------ */

/*
 * Create a buffer group with the given pool size and slot size.
 */
flipc2_return_t
flipc2_bufgroup_create(
    uint32_t            pool_size,
    uint32_t            slot_size,
    flipc2_bufgroup_t  *bg);

/*
 * Destroy a buffer group and release all resources.
 */
flipc2_return_t
flipc2_bufgroup_destroy(
    flipc2_bufgroup_t   bg);

/*
 * Attach to an existing buffer group via its base address.
 */
flipc2_return_t
flipc2_bufgroup_attach(
    vm_address_t        base_addr,
    flipc2_bufgroup_t  *bg);

/*
 * Attach to a buffer group mapped via vm_remap (inter-task peer).
 */
flipc2_return_t
flipc2_bufgroup_attach_remote(
    vm_address_t        base_addr,
    uint32_t            mapped_size,
    flipc2_bufgroup_t  *bg);

/*
 * Detach from a buffer group without destroying it.
 */
flipc2_return_t
flipc2_bufgroup_detach(
    flipc2_bufgroup_t   bg);

/*
 * Share a buffer group with another task via vm_remap.
 */
flipc2_return_t
flipc2_bufgroup_share(
    flipc2_bufgroup_t   bg,
    mach_port_t         target_task,
    vm_address_t       *out_addr);

/*
 * Associate a buffer group with a channel handle.
 */
void
flipc2_channel_set_bufgroup(
    flipc2_channel_t    ch,
    flipc2_bufgroup_t   bg);

#endif /* _FLIPC2_H_ */
