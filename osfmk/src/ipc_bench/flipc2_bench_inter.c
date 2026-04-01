/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * MIT License — see flipc2_bench.h
 */

/*
 * flipc2_bench_inter.c — Inter-task FLIPC v2 benchmarks.
 *
 * Two separate tasks communicating via FLIPC v2 shared memory (vm_remap),
 * like ext2_server ↔ ahci_driver in production.
 *
 * Includes:
 *   - Child echo entries (single + batch)
 *   - Setup / cleanup helpers
 *   - Inter-task RPC benchmark
 *   - Inter-task batch throughput benchmark
 */

#include "flipc2_bench.h"
#include <mach.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <mach/mach_interface.h>
#include <mach/thread_switch.h>
#include <mach/i386/thread_status.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 * Shared child args (inherited via task_create + patched via vm_write)
 * =================================================================== */

volatile struct flipc2_child_args flipc2_child_args_storage;

/* ===================================================================
 * Child echo: minimal FLIPC v2 loop using direct struct access.
 * No vm_allocate, no MIG, no cthreads — only raw memory + semaphore.
 * =================================================================== */

void __attribute__((noreturn, used))
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

/* ===================================================================
 * Child batch echo: waits for batch, processes all available,
 * replies in batch, signals once.
 * =================================================================== */

void __attribute__((noreturn, used))
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

/* ===================================================================
 * Setup child task with vm_remap'd channels.
 * Returns 0 on success and fills child_task/child_thread.
 * On failure returns -1, cleans up, and destroys channels.
 * =================================================================== */

int
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

void
flipc2_inter_cleanup(mach_port_t child_task, mach_port_t child_thread,
                     flipc2_channel_t fwd_ch, flipc2_channel_t rev_ch)
{
    task_terminate(child_task);
    mach_port_deallocate(mach_task_self(), child_thread);
    mach_port_deallocate(mach_task_self(), child_task);
    flipc2_channel_destroy(fwd_ch);
    flipc2_channel_destroy(rev_ch);
}

/* ===================================================================
 * Inter-task RPC benchmark (single descriptor per round-trip)
 * =================================================================== */

void
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
 * Inter-task BATCH throughput (vm_remap shared memory)
 *
 * Parent produces N descriptors, signals ONCE, child consumes all N
 * and replies with N, signals ONCE.  Amortized cost drops with N.
 * =================================================================== */

void
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
