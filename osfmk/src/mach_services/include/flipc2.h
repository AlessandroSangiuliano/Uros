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

/* Descriptor flags */
#define FLIPC2_FLAG_DATA_INLINE     0x00000000  /* data in inline region    */
#define FLIPC2_FLAG_DATA_MAPPED     0x00000001  /* data in page-mapped region */
#define FLIPC2_FLAG_BATCH           0x00000002  /* more descriptors follow  */

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
    uint32_t    _const_pad[1];      /* pad to 48 bytes                   */
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
    struct flipc2_channel_header *hdr = ch->hdr;
    uint32_t tail = hdr->prod_tail;
    uint32_t space;

    /* Check cached cons_head first (avoid volatile read) */
    space = hdr->ring_entries - (tail - ch->cached_cons_head);
    if (space == 0 || space > hdr->ring_entries) {
        /* Refresh from volatile */
        ch->cached_cons_head = hdr->cons_head;
        FLIPC2_READ_FENCE();
        space = hdr->ring_entries - (tail - ch->cached_cons_head);
        if (space == 0)
            return (struct flipc2_desc *)0;
        /* Ring corruption: cons_head was pushed past prod_tail */
        if (space > hdr->ring_entries)
            return (struct flipc2_desc *)0;
    }

    return &ch->ring[flipc2_ring_idx(hdr, tail)];
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
    struct flipc2_channel_header *hdr = ch->hdr;

    FLIPC2_WRITE_FENCE();
    hdr->prod_tail++;
    hdr->prod_total++;

    FLIPC2_WRITE_FENCE();
    if (hdr->cons_sleeping) {
        semaphore_signal(hdr->wakeup_sem);
        hdr->wakeups++;
    }
}

/*
 * Commit N descriptors in batch.
 */
static inline void
flipc2_produce_commit_n(flipc2_channel_t ch, uint32_t n)
{
    struct flipc2_channel_header *hdr = ch->hdr;

    FLIPC2_WRITE_FENCE();
    hdr->prod_tail += n;
    hdr->prod_total += n;

    FLIPC2_WRITE_FENCE();
    if (hdr->cons_sleeping) {
        semaphore_signal(hdr->wakeup_sem);
        hdr->wakeups++;
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
    struct flipc2_channel_header *hdr = ch->hdr;
    uint32_t head = hdr->cons_head;
    uint32_t avail;

    /* Check cached prod_tail first */
    avail = ch->cached_prod_tail - head;
    if (avail == 0 || avail > hdr->ring_entries) {
        FLIPC2_READ_FENCE();
        ch->cached_prod_tail = hdr->prod_tail;
        avail = ch->cached_prod_tail - head;
        if (avail == 0)
            return (struct flipc2_desc *)0;
        /* Ring corruption: prod_tail jumped past ring capacity */
        if (avail > hdr->ring_entries)
            return (struct flipc2_desc *)0;
    }

    return &ch->ring[flipc2_ring_idx(hdr, head)];
}

/*
 * Release one descriptor: advance cons_head, wake producer if blocked.
 */
static inline void
flipc2_consume_release(flipc2_channel_t ch)
{
    FLIPC2_WRITE_FENCE();
    ch->hdr->cons_head++;
    ch->hdr->cons_total++;

    FLIPC2_WRITE_FENCE();
    if (ch->hdr->prod_sleeping) {
        semaphore_signal(ch->hdr->wakeup_sem_prod);
    }
}

/*
 * Release N descriptors in batch, wake producer if blocked.
 */
static inline void
flipc2_consume_release_n(flipc2_channel_t ch, uint32_t n)
{
    FLIPC2_WRITE_FENCE();
    ch->hdr->cons_head += n;
    ch->hdr->cons_total += n;

    FLIPC2_WRITE_FENCE();
    if (ch->hdr->prod_sleeping) {
        semaphore_signal(ch->hdr->wakeup_sem_prod);
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
    struct flipc2_channel_header *hdr = ch->hdr;
    uint32_t head = hdr->cons_head;
    uint32_t avail;
    uint32_t i;

    /* Phase 1: spin-poll */
    for (i = 0; i < spin_count; i++) {
        FLIPC2_READ_FENCE();
        ch->cached_prod_tail = hdr->prod_tail;
        if (ch->cached_prod_tail != head) {
            avail = ch->cached_prod_tail - head;
            if (avail > hdr->ring_entries)
                return (struct flipc2_desc *)0;  /* ring corrupt */
            ch->spurious_wakeups = 0;
            return &ch->ring[flipc2_ring_idx(hdr, head)];
        }
        FLIPC2_PAUSE();
    }

    /* Phase 2: prepare to sleep */
    hdr->cons_sleeping = 1;
    FLIPC2_WRITE_FENCE();

    /* Re-check to avoid lost wakeup */
    ch->cached_prod_tail = hdr->prod_tail;
    FLIPC2_READ_FENCE();
    if (ch->cached_prod_tail != head) {
        hdr->cons_sleeping = 0;
        avail = ch->cached_prod_tail - head;
        if (avail > hdr->ring_entries)
            return (struct flipc2_desc *)0;
        ch->spurious_wakeups = 0;
        return &ch->ring[flipc2_ring_idx(hdr, head)];
    }

    /* Phase 3: block, then verify data actually arrived */
    semaphore_wait(hdr->wakeup_sem);
    hdr->cons_sleeping = 0;

    ch->cached_prod_tail = hdr->prod_tail;
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

    if (avail > hdr->ring_entries)
        return (struct flipc2_desc *)0;  /* ring corrupt */

    ch->spurious_wakeups = 0;
    return &ch->ring[flipc2_ring_idx(hdr, head)];
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
    struct flipc2_channel_header *hdr = ch->hdr;
    uint32_t head = hdr->cons_head;
    uint32_t avail;
    uint32_t i;
    kern_return_t kr;
    tvalspec_t ts;

    /* Phase 1: spin-poll */
    for (i = 0; i < spin_count; i++) {
        FLIPC2_READ_FENCE();
        ch->cached_prod_tail = hdr->prod_tail;
        if (ch->cached_prod_tail != head) {
            avail = ch->cached_prod_tail - head;
            if (avail > hdr->ring_entries)
                return (struct flipc2_desc *)0;
            ch->spurious_wakeups = 0;
            return &ch->ring[flipc2_ring_idx(hdr, head)];
        }
        FLIPC2_PAUSE();
    }

    /* Phase 2: prepare to sleep */
    hdr->cons_sleeping = 1;
    FLIPC2_WRITE_FENCE();

    ch->cached_prod_tail = hdr->prod_tail;
    FLIPC2_READ_FENCE();
    if (ch->cached_prod_tail != head) {
        hdr->cons_sleeping = 0;
        avail = ch->cached_prod_tail - head;
        if (avail > hdr->ring_entries)
            return (struct flipc2_desc *)0;
        ch->spurious_wakeups = 0;
        return &ch->ring[flipc2_ring_idx(hdr, head)];
    }

    /* Phase 3: block with timeout */
    ts.tv_sec  = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000;
    kr = semaphore_timedwait(hdr->wakeup_sem, ts);
    hdr->cons_sleeping = 0;

    if (kr != KERN_SUCCESS)
        return (struct flipc2_desc *)0;

    ch->cached_prod_tail = hdr->prod_tail;
    FLIPC2_READ_FENCE();
    avail = ch->cached_prod_tail - head;

    if (avail == 0) {
        ch->spurious_wakeups++;
        if (ch->spurious_wakeups >= FLIPC2_MAX_SPURIOUS_WAKEUPS)
            return (struct flipc2_desc *)0;
        return flipc2_consume_timedwait(ch, 0, timeout_ms);
    }

    if (avail > hdr->ring_entries)
        return (struct flipc2_desc *)0;

    ch->spurious_wakeups = 0;
    return &ch->ring[flipc2_ring_idx(hdr, head)];
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
    struct flipc2_channel_header *hdr = ch->hdr;
    uint32_t tail = hdr->prod_tail;
    uint32_t space;
    uint32_t i;

    /* Try fast path first */
    space = hdr->ring_entries - (tail - ch->cached_cons_head);
    if (space == 0 || space > hdr->ring_entries) {
        ch->cached_cons_head = hdr->cons_head;
        FLIPC2_READ_FENCE();
        space = hdr->ring_entries - (tail - ch->cached_cons_head);
    }
    if (space > 0 && space <= hdr->ring_entries)
        return &ch->ring[flipc2_ring_idx(hdr, tail)];
    if (space > hdr->ring_entries)
        return (struct flipc2_desc *)0;  /* ring corrupt */

    /* Phase 1: spin-poll */
    for (i = 0; i < spin_count; i++) {
        FLIPC2_READ_FENCE();
        ch->cached_cons_head = hdr->cons_head;
        space = hdr->ring_entries - (tail - ch->cached_cons_head);
        if (space > 0 && space <= hdr->ring_entries)
            return &ch->ring[flipc2_ring_idx(hdr, tail)];
        if (space > hdr->ring_entries)
            return (struct flipc2_desc *)0;
        FLIPC2_PAUSE();
    }

    /* Phase 2: prepare to sleep */
    hdr->prod_sleeping = 1;
    FLIPC2_WRITE_FENCE();

    ch->cached_cons_head = hdr->cons_head;
    FLIPC2_READ_FENCE();
    space = hdr->ring_entries - (tail - ch->cached_cons_head);
    if (space > 0 && space <= hdr->ring_entries) {
        hdr->prod_sleeping = 0;
        return &ch->ring[flipc2_ring_idx(hdr, tail)];
    }
    if (space > hdr->ring_entries) {
        hdr->prod_sleeping = 0;
        return (struct flipc2_desc *)0;
    }

    /* Phase 3: block on producer semaphore */
    semaphore_wait(hdr->wakeup_sem_prod);
    hdr->prod_sleeping = 0;

    ch->cached_cons_head = hdr->cons_head;
    space = hdr->ring_entries - (tail - ch->cached_cons_head);
    if (space == 0 || space > hdr->ring_entries)
        return (struct flipc2_desc *)0;
    return &ch->ring[flipc2_ring_idx(hdr, tail)];
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
    ch->cached_prod_tail = ch->hdr->prod_tail;
    avail = ch->cached_prod_tail - ch->hdr->cons_head;
    if (avail > ch->hdr->ring_entries)
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

#endif /* _FLIPC2_H_ */
