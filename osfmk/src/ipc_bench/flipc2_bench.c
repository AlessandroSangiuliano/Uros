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
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * flipc2_bench.c — FLIPC v2 shared-memory channel benchmark
 *
 * Tests:
 *   1. Single-thread throughput (produce+consume, no wakeup)
 *   2. Intra-task RPC with semaphore (null + data payloads)
 *   3. Inter-task RPC via vm_remap (null + data payloads)
 *   4. Game simulation (draw commands, texture streaming, audio, mixed)
 */

#include "flipc2_bench.h"
#include <flipc2.h>
#include <mach.h>
#include <mach/mach_host.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <mach/mach_interface.h>
#include <mach/clock.h>
#include <mach/clock_types.h>
#include <mach/thread_switch.h>
#include <mach/i386/thread_status.h>
#include <cthreads.h>
#include <stdio.h>
#include <string.h>

#define FLIPC2_WARMUP_ITERS     100
#define FLIPC2_BENCH_ITERS      10000
#define FLIPC2_BENCH_SPIN       0   /* cthreads are cooperative: spin is useless */
#define FLIPC2_BENCH_CHAN_SIZE  (64 * 1024)
#define FLIPC2_BENCH_RING_ENTRIES 256
#define FLIPC2_CHILD_STACK_SIZE (64 * 1024)

/* Well-known child port names (must not collide with ipc_bench's) */
#define FLIPC2_CHILD_SEM_FWD   ((mach_port_t) 0x2503)
#define FLIPC2_CHILD_SEM_REV   ((mach_port_t) 0x2603)

/* ===================================================================
 * Timing helpers
 * =================================================================== */

static mach_port_t flipc2_clock_port;

static void
flipc2_get_time(tvalspec_t *tv)
{
    clock_get_time(flipc2_clock_port, tv);
}

static unsigned long
flipc2_elapsed_ns(const tvalspec_t *before, const tvalspec_t *after)
{
    unsigned long ns;
    ns  = (unsigned long)(after->tv_sec  - before->tv_sec)  * 1000000000UL;
    ns += (unsigned long)(after->tv_nsec - before->tv_nsec);
    return ns;
}

static void
flipc2_print_result(const char *label, unsigned long total_ns, int iters)
{
    unsigned long ns_per_op = total_ns / (unsigned long)iters;

    if (ns_per_op >= 1000) {
        unsigned long us_whole = ns_per_op / 1000;
        unsigned long us_frac  = (ns_per_op % 1000) / 10;
        printf("  %-36s %5lu.%02lu us/op  (%d iters, %lu us)\n",
               label, us_whole, us_frac, iters, total_ns / 1000);
    } else {
        printf("  %-36s %5lu    ns/op  (%d iters, %lu us)\n",
               label, ns_per_op, iters, total_ns / 1000);
    }
}

/* ===================================================================
 * Channel pair helpers (create + cleanup)
 * =================================================================== */

struct flipc2_pair {
    flipc2_channel_t    fwd_ch;
    flipc2_channel_t    rev_ch;
    flipc2_channel_t    fwd_peer;
    flipc2_channel_t    rev_peer;
    mach_port_t         fwd_sem;
    mach_port_t         rev_sem;
};

/*
 * Create two channels (fwd + rev) and attach peer handles.
 * Returns 0 on success, -1 on failure.
 */
static int
flipc2_pair_create(struct flipc2_pair *p, const char *label)
{
    flipc2_return_t ret;

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &p->fwd_ch, &p->fwd_sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: fwd create failed %d\n", label, ret);
        return -1;
    }

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &p->rev_ch, &p->rev_sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: rev create failed %d\n", label, ret);
        flipc2_channel_destroy(p->fwd_ch);
        return -1;
    }

    ret = flipc2_channel_attach((vm_address_t)p->fwd_ch->hdr, &p->fwd_peer);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: fwd attach failed %d\n", label, ret);
        flipc2_channel_destroy(p->rev_ch);
        flipc2_channel_destroy(p->fwd_ch);
        return -1;
    }

    ret = flipc2_channel_attach((vm_address_t)p->rev_ch->hdr, &p->rev_peer);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: rev attach failed %d\n", label, ret);
        flipc2_channel_detach(p->fwd_peer);
        flipc2_channel_destroy(p->rev_ch);
        flipc2_channel_destroy(p->fwd_ch);
        return -1;
    }

    return 0;
}

static void
flipc2_pair_destroy(struct flipc2_pair *p)
{
    flipc2_channel_detach(p->fwd_peer);
    flipc2_channel_detach(p->rev_peer);
    flipc2_channel_destroy(p->fwd_ch);
    flipc2_channel_destroy(p->rev_ch);
}

/* ===================================================================
 * Echo thread for intra-task RPC (cthread)
 * =================================================================== */

struct flipc2_echo_args {
    flipc2_channel_t        recv_ch;
    flipc2_channel_t        send_ch;
    volatile int            running;
    int                     data_size;   /* 0 = null, >0 = echo data */
};

static void *
flipc2_echo_thread(void *arg)
{
    struct flipc2_echo_args *a = (struct flipc2_echo_args *)arg;
    struct flipc2_desc *d;
    struct flipc2_desc *reply;

    while (a->running) {
        d = flipc2_consume_wait(a->recv_ch, FLIPC2_BENCH_SPIN);
        if (!a->running)
            break;

        reply = flipc2_produce_reserve(a->send_ch);
        reply->opcode = d->opcode + 100;
        reply->cookie = d->cookie;
        reply->flags = 0;
        reply->status = 0;

        if (a->data_size > 0) {
            reply->data_offset = d->data_offset;
            reply->data_length = d->data_length;
            memcpy(flipc2_data_ptr(a->send_ch, d->data_offset),
                   flipc2_data_ptr(a->recv_ch, d->data_offset),
                   d->data_length);
        } else {
            reply->data_offset = 0;
            reply->data_length = 0;
        }

        flipc2_consume_release(a->recv_ch);
        flipc2_produce_commit(a->send_ch);
    }

    return (void *)0;
}

/*
 * Stop echo thread cleanly: set flag, send dummy + signal semaphore, join.
 */
static void
flipc2_stop_echo(struct flipc2_echo_args *args, flipc2_channel_t fwd_ch,
                 cthread_t ct)
{
    struct flipc2_desc *d;

    args->running = 0;
    FLIPC2_WRITE_FENCE();

    d = flipc2_produce_reserve(fwd_ch);
    if (d) {
        d->opcode = 0xFFFF;
        d->data_offset = 0;
        d->data_length = 0;
        d->flags = 0;
        d->status = 0;
        flipc2_produce_commit(fwd_ch);
    }
    semaphore_signal(fwd_ch->hdr->wakeup_sem);

    cthread_join(ct);
}

/* ===================================================================
 * 1. Single-thread throughput: produce N, consume N
 * =================================================================== */

static void
bench_flipc2_throughput(const char *label, int batch_size, int iters)
{
    flipc2_channel_t    ch;
    flipc2_channel_t    consumer;
    mach_port_t         sem;
    flipc2_return_t     ret;
    struct flipc2_desc  *d;
    tvalspec_t          t0, t1;
    int                 i, j;

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &ch, &sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: create failed %d\n", label, ret);
        return;
    }

    ret = flipc2_channel_attach((vm_address_t)ch->hdr, &consumer);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: attach failed %d\n", label, ret);
        flipc2_channel_destroy(ch);
        return;
    }

    /* Warmup */
    for (i = 0; i < FLIPC2_WARMUP_ITERS; i++) {
        d = flipc2_produce_reserve(ch);
        d->opcode = 0;
        d->cookie = 0;
        d->data_offset = 0;
        d->data_length = 0;
        d->flags = 0;
        d->status = 0;
        flipc2_produce_commit(ch);
        flipc2_consume_peek(consumer);
        flipc2_consume_release(consumer);
    }

    /* Timed run */
    flipc2_get_time(&t0);
    for (i = 0; i < iters; i++) {
        for (j = 0; j < batch_size; j++) {
            d = flipc2_produce_reserve(ch);
            d->opcode = 0;
            d->cookie = j;
            d->data_offset = 0;
            d->data_length = 0;
            d->flags = 0;
            d->status = 0;
        }
        flipc2_produce_commit_n(ch, batch_size);

        for (j = 0; j < batch_size; j++) {
            flipc2_consume_peek(consumer);
            flipc2_consume_release(consumer);
        }
    }
    flipc2_get_time(&t1);

    flipc2_print_result(label, flipc2_elapsed_ns(&t0, &t1),
                        iters * batch_size);

    flipc2_channel_detach(consumer);
    flipc2_channel_destroy(ch);
}

static void
bench_flipc2_data_throughput(const char *label, int data_size, int iters)
{
    flipc2_channel_t    ch;
    flipc2_channel_t    consumer;
    mach_port_t         sem;
    flipc2_return_t     ret;
    struct flipc2_desc  *d;
    tvalspec_t          t0, t1;
    int                 i;
    uint64_t            data_off = 0;

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &ch, &sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: create failed %d\n", label, ret);
        return;
    }

    ret = flipc2_channel_attach((vm_address_t)ch->hdr, &consumer);
    if (ret != FLIPC2_SUCCESS) {
        flipc2_channel_destroy(ch);
        return;
    }

    /* Warmup */
    for (i = 0; i < FLIPC2_WARMUP_ITERS; i++) {
        d = flipc2_produce_reserve(ch);
        d->opcode = 1;
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = data_off;
        d->data_length = data_size;
        d->status = 0;
        memset(flipc2_data_ptr(ch, data_off), 0xAA, data_size);
        flipc2_produce_commit(ch);
        flipc2_consume_peek(consumer);
        flipc2_consume_release(consumer);
    }

    flipc2_get_time(&t0);
    for (i = 0; i < iters; i++) {
        d = flipc2_produce_reserve(ch);
        d->opcode = 1;
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = data_off;
        d->data_length = data_size;
        d->status = 0;
        memset(flipc2_data_ptr(ch, data_off), 0xAA, data_size);
        flipc2_produce_commit(ch);
        flipc2_consume_peek(consumer);
        flipc2_consume_release(consumer);
    }
    flipc2_get_time(&t1);

    flipc2_print_result(label, flipc2_elapsed_ns(&t0, &t1), iters);

    flipc2_channel_detach(consumer);
    flipc2_channel_destroy(ch);
}

/* ===================================================================
 * 2. Intra-task RPC (ping-pong with semaphore wakeup)
 * =================================================================== */

static void
bench_flipc2_rpc(const char *label, int data_size, int iters)
{
    struct flipc2_pair p;
    struct flipc2_echo_args echo_args;
    cthread_t           echo_ct;
    struct flipc2_desc  *d;
    tvalspec_t          t0, t1;
    uint64_t            data_off = 0;
    int                 i;

    if (flipc2_pair_create(&p, label) != 0)
        return;

    echo_args.recv_ch  = p.fwd_peer;
    echo_args.send_ch  = p.rev_peer;
    echo_args.running  = 1;
    echo_args.data_size = data_size;
    echo_ct = cthread_fork(flipc2_echo_thread, &echo_args);
    thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);

    /* Warmup */
    for (i = 0; i < FLIPC2_WARMUP_ITERS; i++) {
        d = flipc2_produce_reserve(p.fwd_ch);
        d->opcode = 1;
        d->cookie = i;
        d->flags = 0;
        d->status = 0;
        if (data_size > 0) {
            d->data_offset = data_off;
            d->data_length = data_size;
            memset(flipc2_data_ptr(p.fwd_ch, data_off), 0xBB, data_size);
        } else {
            d->data_offset = 0;
            d->data_length = 0;
        }
        flipc2_produce_commit(p.fwd_ch);
        flipc2_consume_wait(p.rev_ch, FLIPC2_BENCH_SPIN);
        flipc2_consume_release(p.rev_ch);
    }

    /* Timed run */
    flipc2_get_time(&t0);
    for (i = 0; i < iters; i++) {
        d = flipc2_produce_reserve(p.fwd_ch);
        d->opcode = 1;
        d->cookie = i;
        d->flags = 0;
        d->status = 0;
        if (data_size > 0) {
            d->data_offset = data_off;
            d->data_length = data_size;
            memset(flipc2_data_ptr(p.fwd_ch, data_off), 0xBB, data_size);
        } else {
            d->data_offset = 0;
            d->data_length = 0;
        }
        flipc2_produce_commit(p.fwd_ch);
        flipc2_consume_wait(p.rev_ch, FLIPC2_BENCH_SPIN);
        flipc2_consume_release(p.rev_ch);
    }
    flipc2_get_time(&t1);

    flipc2_print_result(label, flipc2_elapsed_ns(&t0, &t1), iters);

    flipc2_stop_echo(&echo_args, p.fwd_ch, echo_ct);
    flipc2_pair_destroy(&p);
}

/* ===================================================================
 * 3. Inter-task RPC (true shared memory via vm_remap)
 *
 * Two separate tasks communicating via FLIPC v2 shared memory,
 * like ext2_server ↔ ahci_driver in production.
 * =================================================================== */

struct flipc2_child_args {
    vm_address_t    fwd_base;
    vm_address_t    rev_base;
    uint32_t        ring_offset;
    uint32_t        ring_mask;
    uint32_t        data_offset;
    uint32_t        ring_entries;
    mach_port_t     fwd_sem;
    mach_port_t     rev_sem;
    int             data_size;  /* 0 = null echo, >0 = memcpy echo */
};

static volatile struct flipc2_child_args flipc2_child_args_storage;

/*
 * Child echo: minimal FLIPC v2 loop using direct struct access.
 * No vm_allocate, no MIG, no cthreads — only raw memory + semaphore syscalls.
 */
static void __attribute__((noreturn, used))
flipc2_child_echo_entry(void)
{
    volatile struct flipc2_child_args *args = &flipc2_child_args_storage;
    struct flipc2_channel_header *fwd_hdr =
        (struct flipc2_channel_header *)args->fwd_base;
    struct flipc2_channel_header *rev_hdr =
        (struct flipc2_channel_header *)args->rev_base;
    struct flipc2_desc *fwd_ring =
        (struct flipc2_desc *)((uint8_t *)fwd_hdr + args->ring_offset);
    struct flipc2_desc *rev_ring =
        (struct flipc2_desc *)((uint8_t *)rev_hdr + args->ring_offset);
    uint8_t *fwd_data = (uint8_t *)fwd_hdr + args->data_offset;
    uint8_t *rev_data = (uint8_t *)rev_hdr + args->data_offset;
    uint32_t mask = args->ring_mask;
    mach_port_t fwd_sem = args->fwd_sem;
    mach_port_t rev_sem = args->rev_sem;
    int echo_data_size = args->data_size;

    for (;;) {
        uint32_t head = fwd_hdr->cons_head;

        /* Adaptive wait: set sleeping flag, re-check, block */
        while (fwd_hdr->prod_tail == head) {
            fwd_hdr->cons_sleeping = 1;
            FLIPC2_WRITE_FENCE();
            if (fwd_hdr->prod_tail != head) {
                fwd_hdr->cons_sleeping = 0;
                break;
            }
            semaphore_wait(fwd_sem);
            fwd_hdr->cons_sleeping = 0;
        }

        struct flipc2_desc *d = &fwd_ring[head & mask];

        uint32_t tail = rev_hdr->prod_tail;
        struct flipc2_desc *reply = &rev_ring[tail & mask];

        reply->opcode = d->opcode + 100;
        reply->cookie = d->cookie;
        reply->flags = 0;
        reply->status = 0;

        if (echo_data_size > 0 && d->data_length > 0) {
            reply->data_offset = d->data_offset;
            reply->data_length = d->data_length;
            memcpy(rev_data + d->data_offset,
                   fwd_data + d->data_offset,
                   d->data_length);
        } else {
            reply->data_offset = 0;
            reply->data_length = 0;
        }

        FLIPC2_WRITE_FENCE();
        fwd_hdr->cons_head = head + 1;

        FLIPC2_WRITE_FENCE();
        rev_hdr->prod_tail = tail + 1;

        FLIPC2_WRITE_FENCE();
        if (rev_hdr->cons_sleeping)
            semaphore_signal(rev_sem);
    }
}

/*
 * Setup child task with vm_remap'd channels.
 * Returns 0 on success and fills child_task/child_thread.
 * On failure returns -1, cleans up, and destroys channels.
 */
static int
flipc2_inter_setup(flipc2_channel_t fwd_ch, flipc2_channel_t rev_ch,
                   mach_port_t fwd_sem, mach_port_t rev_sem,
                   int data_size, void (*child_entry)(void),
                   const char *label,
                   mach_port_t *out_task, mach_port_t *out_thread)
{
    kern_return_t       kr;
    flipc2_return_t     ret;
    mach_port_t         child_task, child_thread;
    vm_offset_t         child_stack;
    vm_address_t        child_fwd_addr, child_rev_addr;
    struct i386_thread_state state;
    mach_msg_type_number_t state_count;

    /* Prepare args struct before task_create */
    flipc2_child_args_storage.ring_offset  = fwd_ch->hdr->ring_offset;
    flipc2_child_args_storage.ring_mask    = fwd_ch->hdr->ring_mask;
    flipc2_child_args_storage.data_offset  = fwd_ch->hdr->data_offset;
    flipc2_child_args_storage.ring_entries = fwd_ch->hdr->ring_entries;
    flipc2_child_args_storage.fwd_sem      = FLIPC2_CHILD_SEM_FWD;
    flipc2_child_args_storage.rev_sem      = FLIPC2_CHILD_SEM_REV;
    flipc2_child_args_storage.data_size    = data_size;

    kr = task_create(mach_task_self(),
                     (ledger_port_array_t)0, 0,
                     TRUE, &child_task);
    if (kr) {
        printf("  %s: task_create failed %d\n", label, kr);
        return -1;
    }

    /* Share channels as true shared memory via library API */
    ret = flipc2_channel_share(fwd_ch, child_task, &child_fwd_addr);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: channel_share fwd failed %d\n", label, ret);
        task_terminate(child_task);
        return -1;
    }

    ret = flipc2_channel_share(rev_ch, child_task, &child_rev_addr);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: channel_share rev failed %d\n", label, ret);
        task_terminate(child_task);
        return -1;
    }

    /* Patch child's copy of args struct with child-side addresses */
    {
        struct flipc2_child_args child_args;
        child_args.fwd_base     = child_fwd_addr;
        child_args.rev_base     = child_rev_addr;
        child_args.ring_offset  = fwd_ch->hdr->ring_offset;
        child_args.ring_mask    = fwd_ch->hdr->ring_mask;
        child_args.data_offset  = fwd_ch->hdr->data_offset;
        child_args.ring_entries = fwd_ch->hdr->ring_entries;
        child_args.fwd_sem      = FLIPC2_CHILD_SEM_FWD;
        child_args.rev_sem      = FLIPC2_CHILD_SEM_REV;
        child_args.data_size    = data_size;

        kr = vm_write(child_task,
                      (vm_address_t)&flipc2_child_args_storage,
                      (vm_address_t)&child_args,
                      sizeof(child_args));
        if (kr) {
            printf("  %s: vm_write args failed %d\n", label, kr);
            task_terminate(child_task);
            return -1;
        }
    }

    /* Share semaphore ports with child via library API */
    ret = flipc2_semaphore_share(fwd_sem, child_task, FLIPC2_CHILD_SEM_FWD);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: semaphore_share fwd failed %d\n", label, ret);
        task_terminate(child_task);
        return -1;
    }

    ret = flipc2_semaphore_share(rev_sem, child_task, FLIPC2_CHILD_SEM_REV);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: semaphore_share rev failed %d\n", label, ret);
        task_terminate(child_task);
        return -1;
    }

    /* Stack + thread */
    child_stack = 0;
    kr = vm_allocate(child_task, &child_stack, FLIPC2_CHILD_STACK_SIZE, TRUE);
    if (kr) {
        printf("  %s: child stack failed %d\n", label, kr);
        task_terminate(child_task);
        return -1;
    }

    kr = thread_create(child_task, &child_thread);
    if (kr) {
        printf("  %s: thread_create failed %d\n", label, kr);
        task_terminate(child_task);
        return -1;
    }

    state_count = i386_THREAD_STATE_COUNT;
    thread_get_state(child_thread, i386_THREAD_STATE,
                     (thread_state_t)&state, &state_count);
    state.eip  = (unsigned int)child_entry;
    state.uesp = (unsigned int)(child_stack + FLIPC2_CHILD_STACK_SIZE);
    thread_set_state(child_thread, i386_THREAD_STATE,
                     (thread_state_t)&state, i386_THREAD_STATE_COUNT);
    thread_resume(child_thread);

    thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);

    *out_task   = child_task;
    *out_thread = child_thread;
    return 0;
}

static void
flipc2_inter_cleanup(mach_port_t child_task, mach_port_t child_thread,
                     flipc2_channel_t fwd_ch, flipc2_channel_t rev_ch)
{
    task_terminate(child_task);
    mach_port_deallocate(mach_task_self(), child_thread);
    mach_port_deallocate(mach_task_self(), child_task);
    flipc2_channel_destroy(fwd_ch);
    flipc2_channel_destroy(rev_ch);
}

static void
bench_flipc2_inter_rpc(const char *label, int data_size, int iters)
{
    flipc2_channel_t    fwd_ch, rev_ch;
    mach_port_t         fwd_sem, rev_sem;
    mach_port_t         child_task, child_thread;
    flipc2_return_t     ret;
    struct flipc2_desc  *d;
    tvalspec_t          t0, t1;
    uint64_t            data_off = 0;
    int                 i;

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &fwd_ch, &fwd_sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: fwd create failed %d\n", label, ret);
        return;
    }

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &rev_ch, &rev_sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: rev create failed %d\n", label, ret);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    if (flipc2_inter_setup(fwd_ch, rev_ch, fwd_sem, rev_sem,
                           data_size, flipc2_child_echo_entry,
                           label,
                           &child_task, &child_thread) != 0) {
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    /* Warmup */
    for (i = 0; i < FLIPC2_WARMUP_ITERS; i++) {
        d = flipc2_produce_reserve(fwd_ch);
        d->opcode = 1;
        d->cookie = i;
        d->flags = 0;
        d->status = 0;
        if (data_size > 0) {
            d->data_offset = data_off;
            d->data_length = data_size;
            memset(flipc2_data_ptr(fwd_ch, data_off), 0xCC, data_size);
        } else {
            d->data_offset = 0;
            d->data_length = 0;
        }
        flipc2_produce_commit(fwd_ch);
        flipc2_consume_wait(rev_ch, FLIPC2_BENCH_SPIN);
        flipc2_consume_release(rev_ch);
    }

    /* Timed run */
    flipc2_get_time(&t0);
    for (i = 0; i < iters; i++) {
        d = flipc2_produce_reserve(fwd_ch);
        d->opcode = 1;
        d->cookie = i;
        d->flags = 0;
        d->status = 0;
        if (data_size > 0) {
            d->data_offset = data_off;
            d->data_length = data_size;
            memset(flipc2_data_ptr(fwd_ch, data_off), 0xCC, data_size);
        } else {
            d->data_offset = 0;
            d->data_length = 0;
        }
        flipc2_produce_commit(fwd_ch);
        flipc2_consume_wait(rev_ch, FLIPC2_BENCH_SPIN);
        flipc2_consume_release(rev_ch);
    }
    flipc2_get_time(&t1);

    flipc2_print_result(label, flipc2_elapsed_ns(&t0, &t1), iters);

    flipc2_inter_cleanup(child_task, child_thread, fwd_ch, rev_ch);
}

/* ===================================================================
 * 3b. Inter-task BATCH throughput (vm_remap shared memory)
 *
 * The real-world fast path: parent produces a batch of N descriptors,
 * signals ONCE, child consumes all N and replies with N, signals ONCE.
 * Cost = 2 context switches (fixed) + N × ring ops (~5 ns each).
 * Amortized per-descriptor cost drops rapidly with batch size.
 * =================================================================== */

/*
 * Child batch echo: waits for batch, processes all available,
 * replies in batch, signals once.
 */
static void __attribute__((noreturn, used))
flipc2_child_batch_echo_entry(void)
{
    volatile struct flipc2_child_args *args = &flipc2_child_args_storage;
    struct flipc2_channel_header *fwd_hdr =
        (struct flipc2_channel_header *)args->fwd_base;
    struct flipc2_channel_header *rev_hdr =
        (struct flipc2_channel_header *)args->rev_base;
    struct flipc2_desc *fwd_ring =
        (struct flipc2_desc *)((uint8_t *)fwd_hdr + args->ring_offset);
    struct flipc2_desc *rev_ring =
        (struct flipc2_desc *)((uint8_t *)rev_hdr + args->ring_offset);
    uint32_t mask = args->ring_mask;
    mach_port_t fwd_sem = args->fwd_sem;
    mach_port_t rev_sem = args->rev_sem;

    for (;;) {
        uint32_t head = fwd_hdr->cons_head;

        /* Wait until at least one descriptor is available */
        while (fwd_hdr->prod_tail == head) {
            fwd_hdr->cons_sleeping = 1;
            FLIPC2_WRITE_FENCE();
            if (fwd_hdr->prod_tail != head) {
                fwd_hdr->cons_sleeping = 0;
                break;
            }
            semaphore_wait(fwd_sem);
            fwd_hdr->cons_sleeping = 0;
        }

        /* Process ALL available descriptors in one shot */
        FLIPC2_READ_FENCE();
        uint32_t avail = fwd_hdr->prod_tail - head;
        uint32_t tail = rev_hdr->prod_tail;
        uint32_t j;

        for (j = 0; j < avail; j++) {
            struct flipc2_desc *d = &fwd_ring[(head + j) & mask];
            struct flipc2_desc *reply = &rev_ring[(tail + j) & mask];

            reply->opcode = d->opcode + 100;
            reply->cookie = d->cookie;
            reply->data_offset = 0;
            reply->data_length = 0;
            reply->flags = 0;
            reply->status = 0;
        }

        /* Release fwd batch + commit rev batch */
        FLIPC2_WRITE_FENCE();
        fwd_hdr->cons_head = head + avail;

        FLIPC2_WRITE_FENCE();
        rev_hdr->prod_tail = tail + avail;

        /* Signal parent once for entire batch */
        FLIPC2_WRITE_FENCE();
        if (rev_hdr->cons_sleeping)
            semaphore_signal(rev_sem);
    }
}

static void
bench_flipc2_inter_batch(const char *label, int batch_size, int iters)
{
    flipc2_channel_t    fwd_ch, rev_ch;
    mach_port_t         fwd_sem, rev_sem;
    mach_port_t         child_task, child_thread;
    flipc2_return_t     ret;
    struct flipc2_desc  *d;
    tvalspec_t          t0, t1;
    int                 i, j;

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &fwd_ch, &fwd_sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: fwd create failed %d\n", label, ret);
        return;
    }

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &rev_ch, &rev_sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: rev create failed %d\n", label, ret);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    if (flipc2_inter_setup(fwd_ch, rev_ch, fwd_sem, rev_sem,
                           0, flipc2_child_batch_echo_entry,
                           label,
                           &child_task, &child_thread) != 0) {
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    /* Warmup */
    for (i = 0; i < FLIPC2_WARMUP_ITERS; i++) {
        for (j = 0; j < batch_size; j++) {
            d = flipc2_produce_reserve(fwd_ch);
            d->opcode = 1;
            d->cookie = j;
            d->data_offset = 0;
            d->data_length = 0;
            d->flags = 0;
            d->status = 0;
        }
        flipc2_produce_commit_n(fwd_ch, batch_size);
        /* Wait for child to process all and reply */
        for (j = 0; j < batch_size; j++) {
            flipc2_consume_wait(rev_ch, FLIPC2_BENCH_SPIN);
            flipc2_consume_release(rev_ch);
        }
    }

    /* Timed run */
    flipc2_get_time(&t0);
    for (i = 0; i < iters; i++) {
        for (j = 0; j < batch_size; j++) {
            d = flipc2_produce_reserve(fwd_ch);
            d->opcode = 1;
            d->cookie = j;
            d->data_offset = 0;
            d->data_length = 0;
            d->flags = 0;
            d->status = 0;
        }
        flipc2_produce_commit_n(fwd_ch, batch_size);

        for (j = 0; j < batch_size; j++) {
            flipc2_consume_wait(rev_ch, FLIPC2_BENCH_SPIN);
            flipc2_consume_release(rev_ch);
        }
    }
    flipc2_get_time(&t1);

    flipc2_print_result(label, flipc2_elapsed_ns(&t0, &t1),
                        iters * batch_size);

    flipc2_inter_cleanup(child_task, child_thread, fwd_ch, rev_ch);
}

/* ===================================================================
 * 4. Game simulation benchmarks
 *
 * Simulates realistic game workloads over FLIPC v2:
 *   - Draw commands: 60 small descriptors batched (one frame @60fps)
 *   - Texture streaming: 16 KB chunks (texture upload)
 *   - Audio buffers: 4 KB chunks (PCM audio frames)
 *   - Mixed frame: draw batch + texture + audio in one frame
 * =================================================================== */

#define GAME_DRAW_CMDS_PER_FRAME   60
#define GAME_TEXTURE_CHUNK_SIZE    (16 * 1024)  /* 16 KB */
#define GAME_AUDIO_CHUNK_SIZE      4096         /* 4 KB  */
#define GAME_FRAMES                1000

/*
 * Draw command batch: 60 null descriptors per frame, batch-committed.
 * Single-thread throughput — no kernel.
 */
static void
bench_game_draw_commands(void)
{
    flipc2_channel_t    ch;
    flipc2_channel_t    consumer;
    mach_port_t         sem;
    flipc2_return_t     ret;
    struct flipc2_desc  *d;
    tvalspec_t          t0, t1;
    int                 frame, cmd;

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &ch, &sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  game draw: create failed %d\n", ret);
        return;
    }

    ret = flipc2_channel_attach((vm_address_t)ch->hdr, &consumer);
    if (ret != FLIPC2_SUCCESS) {
        flipc2_channel_destroy(ch);
        return;
    }

    /* Warmup */
    for (frame = 0; frame < 10; frame++) {
        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            d = flipc2_produce_reserve(ch);
            d->opcode = 0x10;  /* DRAW_TRIANGLES */
            d->cookie = cmd;
            d->param[0] = 1000 + cmd;  /* vertex count */
            d->param[1] = 0;           /* texture id */
            d->data_offset = 0;
            d->data_length = 0;
            d->flags = (cmd < GAME_DRAW_CMDS_PER_FRAME - 1)
                       ? FLIPC2_FLAG_BATCH : 0;
            d->status = 0;
        }
        flipc2_produce_commit_n(ch, GAME_DRAW_CMDS_PER_FRAME);

        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            flipc2_consume_peek(consumer);
            flipc2_consume_release(consumer);
        }
    }

    /* Timed run */
    flipc2_get_time(&t0);
    for (frame = 0; frame < GAME_FRAMES; frame++) {
        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            d = flipc2_produce_reserve(ch);
            d->opcode = 0x10;
            d->cookie = cmd;
            d->param[0] = 1000 + cmd;
            d->param[1] = frame;
            d->data_offset = 0;
            d->data_length = 0;
            d->flags = (cmd < GAME_DRAW_CMDS_PER_FRAME - 1)
                       ? FLIPC2_FLAG_BATCH : 0;
            d->status = 0;
        }
        flipc2_produce_commit_n(ch, GAME_DRAW_CMDS_PER_FRAME);

        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            flipc2_consume_peek(consumer);
            flipc2_consume_release(consumer);
        }
    }
    flipc2_get_time(&t1);

    {
        unsigned long total_ns = flipc2_elapsed_ns(&t0, &t1);
        unsigned long ns_per_frame = total_ns / GAME_FRAMES;
        unsigned long us_frame = ns_per_frame / 1000;
        unsigned long us_frac  = (ns_per_frame % 1000) / 10;

        printf("  draw cmds (60/frame, batched)       "
               "%5lu.%02lu us/frame (%d frames, %lu us total)\n",
               us_frame, us_frac, GAME_FRAMES, total_ns / 1000);
        flipc2_print_result("  per draw command",
                            total_ns, GAME_FRAMES * GAME_DRAW_CMDS_PER_FRAME);
    }

    flipc2_channel_detach(consumer);
    flipc2_channel_destroy(ch);
}

/*
 * Texture streaming: 16 KB chunks with memset (simulates DMA fill).
 */
static void
bench_game_texture_stream(void)
{
    flipc2_channel_t    ch;
    flipc2_channel_t    consumer;
    mach_port_t         sem;
    flipc2_return_t     ret;
    struct flipc2_desc  *d;
    tvalspec_t          t0, t1;
    int                 i;
    uint64_t            data_off = 0;

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &ch, &sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  game texture: create failed %d\n", ret);
        return;
    }

    ret = flipc2_channel_attach((vm_address_t)ch->hdr, &consumer);
    if (ret != FLIPC2_SUCCESS) {
        flipc2_channel_destroy(ch);
        return;
    }

    /* Warmup */
    for (i = 0; i < FLIPC2_WARMUP_ITERS; i++) {
        d = flipc2_produce_reserve(ch);
        d->opcode = 0x20;  /* TEXTURE_UPLOAD */
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = data_off;
        d->data_length = GAME_TEXTURE_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, data_off), 0xDD, GAME_TEXTURE_CHUNK_SIZE);
        flipc2_produce_commit(ch);
        flipc2_consume_peek(consumer);
        flipc2_consume_release(consumer);
    }

    flipc2_get_time(&t0);
    for (i = 0; i < FLIPC2_BENCH_ITERS; i++) {
        d = flipc2_produce_reserve(ch);
        d->opcode = 0x20;
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = data_off;
        d->data_length = GAME_TEXTURE_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, data_off), 0xDD, GAME_TEXTURE_CHUNK_SIZE);
        flipc2_produce_commit(ch);
        flipc2_consume_peek(consumer);
        flipc2_consume_release(consumer);
    }
    flipc2_get_time(&t1);

    flipc2_print_result("texture 16KB chunk",
                        flipc2_elapsed_ns(&t0, &t1), FLIPC2_BENCH_ITERS);

    flipc2_channel_detach(consumer);
    flipc2_channel_destroy(ch);
}

/*
 * Audio buffer: 4 KB PCM frames.
 */
static void
bench_game_audio_buffer(void)
{
    flipc2_channel_t    ch;
    flipc2_channel_t    consumer;
    mach_port_t         sem;
    flipc2_return_t     ret;
    struct flipc2_desc  *d;
    tvalspec_t          t0, t1;
    int                 i;
    uint64_t            data_off = 0;

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &ch, &sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  game audio: create failed %d\n", ret);
        return;
    }

    ret = flipc2_channel_attach((vm_address_t)ch->hdr, &consumer);
    if (ret != FLIPC2_SUCCESS) {
        flipc2_channel_destroy(ch);
        return;
    }

    for (i = 0; i < FLIPC2_WARMUP_ITERS; i++) {
        d = flipc2_produce_reserve(ch);
        d->opcode = 0x30;  /* AUDIO_PCM */
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = data_off;
        d->data_length = GAME_AUDIO_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, data_off), 0x80, GAME_AUDIO_CHUNK_SIZE);
        flipc2_produce_commit(ch);
        flipc2_consume_peek(consumer);
        flipc2_consume_release(consumer);
    }

    flipc2_get_time(&t0);
    for (i = 0; i < FLIPC2_BENCH_ITERS; i++) {
        d = flipc2_produce_reserve(ch);
        d->opcode = 0x30;
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = data_off;
        d->data_length = GAME_AUDIO_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, data_off), 0x80, GAME_AUDIO_CHUNK_SIZE);
        flipc2_produce_commit(ch);
        flipc2_consume_peek(consumer);
        flipc2_consume_release(consumer);
    }
    flipc2_get_time(&t1);

    flipc2_print_result("audio 4KB PCM frame",
                        flipc2_elapsed_ns(&t0, &t1), FLIPC2_BENCH_ITERS);

    flipc2_channel_detach(consumer);
    flipc2_channel_destroy(ch);
}

/*
 * Mixed frame: per-frame batch of 60 draw cmds + 1 texture 16KB + 1 audio 4KB.
 * This simulates one complete game frame submission.
 */
static void
bench_game_mixed_frame(void)
{
    flipc2_channel_t    ch;
    flipc2_channel_t    consumer;
    mach_port_t         sem;
    flipc2_return_t     ret;
    struct flipc2_desc  *d;
    tvalspec_t          t0, t1;
    int                 frame, cmd;
    uint64_t            tex_off = 0;
    uint64_t            audio_off = GAME_TEXTURE_CHUNK_SIZE;
    int                 total_descs = GAME_DRAW_CMDS_PER_FRAME + 2;

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &ch, &sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  game mixed: create failed %d\n", ret);
        return;
    }

    ret = flipc2_channel_attach((vm_address_t)ch->hdr, &consumer);
    if (ret != FLIPC2_SUCCESS) {
        flipc2_channel_destroy(ch);
        return;
    }

    /* Warmup */
    for (frame = 0; frame < 10; frame++) {
        /* 60 draw commands */
        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            d = flipc2_produce_reserve(ch);
            d->opcode = 0x10;
            d->cookie = cmd;
            d->param[0] = 1000 + cmd;
            d->data_offset = 0;
            d->data_length = 0;
            d->flags = FLIPC2_FLAG_BATCH;
            d->status = 0;
        }
        /* texture upload */
        d = flipc2_produce_reserve(ch);
        d->opcode = 0x20;
        d->flags = FLIPC2_FLAG_DATA_INLINE | FLIPC2_FLAG_BATCH;
        d->data_offset = tex_off;
        d->data_length = GAME_TEXTURE_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, tex_off), 0xDD, GAME_TEXTURE_CHUNK_SIZE);
        /* audio buffer */
        d = flipc2_produce_reserve(ch);
        d->opcode = 0x30;
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = audio_off;
        d->data_length = GAME_AUDIO_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, audio_off), 0x80, GAME_AUDIO_CHUNK_SIZE);

        flipc2_produce_commit_n(ch, total_descs);

        for (cmd = 0; cmd < total_descs; cmd++) {
            flipc2_consume_peek(consumer);
            flipc2_consume_release(consumer);
        }
    }

    /* Timed run */
    flipc2_get_time(&t0);
    for (frame = 0; frame < GAME_FRAMES; frame++) {
        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            d = flipc2_produce_reserve(ch);
            d->opcode = 0x10;
            d->cookie = cmd;
            d->param[0] = 1000 + cmd;
            d->param[1] = frame;
            d->data_offset = 0;
            d->data_length = 0;
            d->flags = FLIPC2_FLAG_BATCH;
            d->status = 0;
        }
        d = flipc2_produce_reserve(ch);
        d->opcode = 0x20;
        d->flags = FLIPC2_FLAG_DATA_INLINE | FLIPC2_FLAG_BATCH;
        d->data_offset = tex_off;
        d->data_length = GAME_TEXTURE_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, tex_off), 0xDD, GAME_TEXTURE_CHUNK_SIZE);
        d = flipc2_produce_reserve(ch);
        d->opcode = 0x30;
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = audio_off;
        d->data_length = GAME_AUDIO_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, audio_off), 0x80, GAME_AUDIO_CHUNK_SIZE);

        flipc2_produce_commit_n(ch, total_descs);

        for (cmd = 0; cmd < total_descs; cmd++) {
            flipc2_consume_peek(consumer);
            flipc2_consume_release(consumer);
        }
    }
    flipc2_get_time(&t1);

    {
        unsigned long total_ns = flipc2_elapsed_ns(&t0, &t1);
        unsigned long ns_per_frame = total_ns / GAME_FRAMES;
        unsigned long us_frame = ns_per_frame / 1000;
        unsigned long us_frac  = (ns_per_frame % 1000) / 10;
        unsigned long budget_us = 16666; /* 16.67 ms @ 60fps */
        unsigned long pct = (ns_per_frame / 10) / (budget_us / 10);

        printf("  mixed frame (60 draw+tex+audio)     "
               "%5lu.%02lu us/frame (%d frames)\n",
               us_frame, us_frac, GAME_FRAMES);
        printf("  frame budget @ 60fps: %lu.%02lu us / 16666 us = %lu.%lu%%\n",
               us_frame, us_frac,
               pct / 10, pct % 10);
    }

    flipc2_channel_detach(consumer);
    flipc2_channel_destroy(ch);
}

/* ===================================================================
 * 4b. Game simulation — inter-task (two separate tasks, vm_remap)
 *
 * Simulates a real game engine → renderer pipeline across tasks.
 * Engine produces 60 draw commands + 1 texture upload + 1 audio
 * buffer per frame as a batch.  Renderer (child task) consumes the
 * entire batch and sends a single "frame done" reply.
 * =================================================================== */

static void
bench_game_mixed_frame_inter(void)
{
    flipc2_channel_t    fwd_ch, rev_ch;
    mach_port_t         fwd_sem, rev_sem;
    mach_port_t         child_task, child_thread;
    flipc2_return_t     ret;
    struct flipc2_desc  *d;
    tvalspec_t          t0, t1;
    int                 frame, cmd;
    uint64_t            tex_off = 0;
    uint64_t            audio_off = GAME_TEXTURE_CHUNK_SIZE;
    int                 total_descs = GAME_DRAW_CMDS_PER_FRAME + 2;

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &fwd_ch, &fwd_sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  game inter: fwd create failed %d\n", ret);
        return;
    }

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &rev_ch, &rev_sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  game inter: rev create failed %d\n", ret);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    if (flipc2_inter_setup(fwd_ch, rev_ch, fwd_sem, rev_sem,
                           0, flipc2_child_batch_echo_entry,
                           "game inter",
                           &child_task, &child_thread) != 0) {
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    /* Warmup */
    for (frame = 0; frame < 10; frame++) {
        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            d = flipc2_produce_reserve(fwd_ch);
            d->opcode = 0x10;
            d->cookie = cmd;
            d->param[0] = 1000 + cmd;
            d->data_offset = 0;
            d->data_length = 0;
            d->flags = FLIPC2_FLAG_BATCH;
            d->status = 0;
        }
        d = flipc2_produce_reserve(fwd_ch);
        d->opcode = 0x20;
        d->flags = FLIPC2_FLAG_DATA_INLINE | FLIPC2_FLAG_BATCH;
        d->data_offset = tex_off;
        d->data_length = GAME_TEXTURE_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(fwd_ch, tex_off), 0xDD,
               GAME_TEXTURE_CHUNK_SIZE);
        d = flipc2_produce_reserve(fwd_ch);
        d->opcode = 0x30;
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = audio_off;
        d->data_length = GAME_AUDIO_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(fwd_ch, audio_off), 0x80,
               GAME_AUDIO_CHUNK_SIZE);

        flipc2_produce_commit_n(fwd_ch, total_descs);

        /* Wait for child to process batch and reply */
        for (cmd = 0; cmd < total_descs; cmd++) {
            flipc2_consume_wait(rev_ch, FLIPC2_BENCH_SPIN);
            flipc2_consume_release(rev_ch);
        }
    }

    /* Timed run */
    flipc2_get_time(&t0);
    for (frame = 0; frame < GAME_FRAMES; frame++) {
        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            d = flipc2_produce_reserve(fwd_ch);
            d->opcode = 0x10;
            d->cookie = cmd;
            d->param[0] = 1000 + cmd;
            d->param[1] = frame;
            d->data_offset = 0;
            d->data_length = 0;
            d->flags = FLIPC2_FLAG_BATCH;
            d->status = 0;
        }
        d = flipc2_produce_reserve(fwd_ch);
        d->opcode = 0x20;
        d->flags = FLIPC2_FLAG_DATA_INLINE | FLIPC2_FLAG_BATCH;
        d->data_offset = tex_off;
        d->data_length = GAME_TEXTURE_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(fwd_ch, tex_off), 0xDD,
               GAME_TEXTURE_CHUNK_SIZE);
        d = flipc2_produce_reserve(fwd_ch);
        d->opcode = 0x30;
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = audio_off;
        d->data_length = GAME_AUDIO_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(fwd_ch, audio_off), 0x80,
               GAME_AUDIO_CHUNK_SIZE);

        flipc2_produce_commit_n(fwd_ch, total_descs);

        for (cmd = 0; cmd < total_descs; cmd++) {
            flipc2_consume_wait(rev_ch, FLIPC2_BENCH_SPIN);
            flipc2_consume_release(rev_ch);
        }
    }
    flipc2_get_time(&t1);

    {
        unsigned long total_ns = flipc2_elapsed_ns(&t0, &t1);
        unsigned long ns_per_frame = total_ns / GAME_FRAMES;
        unsigned long us_frame = ns_per_frame / 1000;
        unsigned long us_frac  = (ns_per_frame % 1000) / 10;
        unsigned long budget_us = 16666;
        unsigned long pct = (ns_per_frame / 10) / (budget_us / 10);

        printf("  mixed frame INTER-TASK (62 desc)     "
               "%5lu.%02lu us/frame (%d frames)\n",
               us_frame, us_frac, GAME_FRAMES);
        printf("  frame budget @ 60fps: %lu.%02lu us / 16666 us = %lu.%lu%%\n",
               us_frame, us_frac,
               pct / 10, pct % 10);
    }

    flipc2_inter_cleanup(child_task, child_thread, fwd_ch, rev_ch);
}

void
bench_flipc2_run(mach_port_t clock_port)
{
    flipc2_clock_port = clock_port;

    printf("\n=== FLIPC v2 Benchmarks ===\n");

    /* --- Throughput (single-thread, zero kernel) --- */
    printf("\n--- FLIPC2 throughput (single-thread, no kernel) ---\n");
    bench_flipc2_throughput("null desc (batch=1)",   1, FLIPC2_BENCH_ITERS);
    bench_flipc2_throughput("null desc (batch=16)", 16, FLIPC2_BENCH_ITERS);
    bench_flipc2_throughput("null desc (batch=64)", 64, FLIPC2_BENCH_ITERS);

    printf("\n--- FLIPC2 throughput with data ---\n");
    bench_flipc2_data_throughput("128B produce+consume",  128,
                                FLIPC2_BENCH_ITERS);
    bench_flipc2_data_throughput("1024B produce+consume", 1024,
                                FLIPC2_BENCH_ITERS);
    bench_flipc2_data_throughput("4096B produce+consume", 4096,
                                FLIPC2_BENCH_ITERS);

    /* --- Intra-task RPC (semaphore path) --- */
    printf("\n--- FLIPC2 intra-task RPC (semaphore path) ---\n");
    bench_flipc2_rpc("null RPC (intra)",    0, FLIPC2_BENCH_ITERS);
    bench_flipc2_rpc("128B RPC (intra)",  128, FLIPC2_BENCH_ITERS);
    bench_flipc2_rpc("1024B RPC (intra)", 1024, FLIPC2_BENCH_ITERS);
    bench_flipc2_rpc("4096B RPC (intra)", 4096, FLIPC2_BENCH_ITERS);

    /* --- Inter-task RPC (vm_remap shared memory) --- */
    printf("\n--- FLIPC2 inter-task RPC (vm_remap shared memory) ---\n");
    bench_flipc2_inter_rpc("null RPC (inter)",    0, FLIPC2_BENCH_ITERS);
    bench_flipc2_inter_rpc("128B RPC (inter)",  128, FLIPC2_BENCH_ITERS);
    bench_flipc2_inter_rpc("1024B RPC (inter)", 1024, FLIPC2_BENCH_ITERS);
    bench_flipc2_inter_rpc("4096B RPC (inter)", 4096, FLIPC2_BENCH_ITERS);

    /* --- Inter-task BATCH throughput (amortized) --- */
    printf("\n--- FLIPC2 inter-task BATCH (vm_remap, amortized) ---\n");
    bench_flipc2_inter_batch("batch=1 (inter)",   1, FLIPC2_BENCH_ITERS);
    bench_flipc2_inter_batch("batch=16 (inter)", 16, FLIPC2_BENCH_ITERS);
    bench_flipc2_inter_batch("batch=64 (inter)", 64, FLIPC2_BENCH_ITERS);

    /* --- Game simulation --- */
    printf("\n--- FLIPC2 game simulation (intra-task throughput) ---\n");
    bench_game_draw_commands();
    bench_game_texture_stream();
    bench_game_audio_buffer();
    bench_game_mixed_frame();

    /* --- Game simulation inter-task --- */
    printf("\n--- FLIPC2 game simulation (INTER-TASK, vm_remap) ---\n");
    bench_game_mixed_frame_inter();

    printf("\n");
}
