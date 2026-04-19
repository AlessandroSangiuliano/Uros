/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * MIT License — see flipc2_bench.h
 */

/*
 * flipc2_bench_bufgroup.c — Buffer group benchmarks.
 *
 * Tests:
 *   1. alloc+free microbenchmark (CAS latency)
 *   2. Intra-task RPC with bufgroup data (alloc/fill/send/echo/free/reply)
 *   3. Inter-task RPC with bufgroup data (vm_remap shared pool)
 */

#include "flipc2_bench.h"

/* See flipc2_bench_inter.c — reset in child tasks */
extern int _mig_multithreaded;
#include <mach.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <mach/mach_interface.h>
#include <mach/thread_switch.h>
#include <mach/i386/thread_status.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 * Echo thread for bufgroup RPC.
 *
 * Same as flipc2_echo_thread but uses flipc2_resolve_data() for
 * reading and flipc2_bufgroup_free() to release the data slot.
 * The reply goes on the channel's inline data region (offset 0)
 * since we only need to echo the payload, not allocate a new slot.
 * =================================================================== */

struct flipc2_bg_echo_args {
    flipc2_channel_t    recv_ch;
    flipc2_channel_t    send_ch;
    flipc2_bufgroup_t   bg;
    volatile int        running;
    int                 data_size;
};

static void *
flipc2_bg_echo_thread(void *arg)
{
    struct flipc2_bg_echo_args *a = (struct flipc2_bg_echo_args *)arg;
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
        reply->data_offset = 0;
        reply->data_length = 0;

        if (a->data_size > 0 && (d->flags & FLIPC2_FLAG_DATA_BUFGROUP)) {
            /* Free the bufgroup slot after reading */
            flipc2_bufgroup_free(a->bg, d->data_offset);
        }

        flipc2_consume_release(a->recv_ch);
        flipc2_produce_commit(a->send_ch);
    }

    return (void *)0;
}

/* ===================================================================
 * Benchmark: alloc + free microbenchmark (single-thread, CAS latency)
 * =================================================================== */

void
bench_flipc2_bufgroup_alloc_free(void)
{
    flipc2_bufgroup_t   bg;
    flipc2_return_t     ret;
    tvalspec_t          t0, t1;
    int                 iters = 50000;
    int                 i;
    uint64_t            offset;

    ret = flipc2_bufgroup_create(FLIPC2_BENCH_BG_POOL_SIZE,
                                 FLIPC2_BENCH_BG_SLOT_SIZE, &bg);
    if (ret != FLIPC2_SUCCESS) {
        printf("  bufgroup alloc+free: create failed %d\n", ret);
        return;
    }

    /* Warmup */
    for (i = 0; i < 1000; i++) {
        offset = flipc2_bufgroup_alloc(bg);
        if (offset == (uint64_t)-1) {
            printf("  bufgroup alloc+free: warmup alloc failed\n");
            flipc2_bufgroup_destroy(bg);
            return;
        }
        flipc2_bufgroup_free(bg, offset);
    }

    flipc2_get_time(&t0);
    for (i = 0; i < iters; i++) {
        offset = flipc2_bufgroup_alloc(bg);
        flipc2_bufgroup_free(bg, offset);
    }
    flipc2_get_time(&t1);

    flipc2_print_result("bufgroup alloc+free",
                        flipc2_elapsed_ns(&t0, &t1), iters);

    flipc2_bufgroup_destroy(bg);
}

/* ===================================================================
 * Benchmark: intra-task RPC using buffer group for data.
 *
 * Producer allocs from bufgroup, fills slot, sends descriptor
 * with FLIPC2_FLAG_DATA_BUFGROUP.  Echo thread reads data, frees
 * the slot, and replies.
 * =================================================================== */

void
bench_flipc2_bufgroup_rpc(const char *label, int data_size, int iters)
{
    struct flipc2_pair          pair;
    flipc2_bufgroup_t           bg;
    flipc2_return_t             ret;
    struct flipc2_bg_echo_args  args;
    pthread_t                   ct;
    struct flipc2_desc          *d;
    tvalspec_t                  t0, t1;
    uint64_t                    offset;
    int                         i;

    if (flipc2_pair_create(&pair, label) != 0)
        return;

    ret = flipc2_bufgroup_create(FLIPC2_BENCH_BG_POOL_SIZE,
                                 FLIPC2_BENCH_BG_SLOT_SIZE, &bg);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: bufgroup create failed %d\n", label, ret);
        flipc2_pair_destroy(&pair);
        return;
    }

    /* Associate bufgroup with both channel handles */
    flipc2_channel_set_bufgroup(pair.fwd_ch, bg);
    flipc2_channel_set_bufgroup(pair.fwd_peer, bg);

    /* Start echo thread */
    args.recv_ch   = pair.fwd_peer;
    args.send_ch   = pair.rev_peer;
    args.bg        = bg;
    args.running   = 1;
    args.data_size = data_size;

    pthread_create(&ct, NULL, (void *(*)(void *))flipc2_bg_echo_thread, (void *)&args);
    thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);

    /* Warmup */
    for (i = 0; i < FLIPC2_WARMUP_ITERS; i++) {
        offset = flipc2_bufgroup_alloc(bg);

        d = flipc2_produce_reserve(pair.fwd_ch);
        d->opcode = 1;
        d->cookie = i;
        d->flags = FLIPC2_FLAG_DATA_BUFGROUP;
        d->status = 0;
        d->data_offset = offset;
        d->data_length = data_size;
        memset(flipc2_bufgroup_data(bg, offset), 0xCC, data_size);

        flipc2_produce_commit(pair.fwd_ch);
        flipc2_consume_wait(pair.rev_ch, FLIPC2_BENCH_SPIN);
        flipc2_consume_release(pair.rev_ch);
    }

    /* Timed run */
    flipc2_get_time(&t0);
    for (i = 0; i < iters; i++) {
        offset = flipc2_bufgroup_alloc(bg);

        d = flipc2_produce_reserve(pair.fwd_ch);
        d->opcode = 1;
        d->cookie = i;
        d->flags = FLIPC2_FLAG_DATA_BUFGROUP;
        d->status = 0;
        d->data_offset = offset;
        d->data_length = data_size;
        memset(flipc2_bufgroup_data(bg, offset), 0xCC, data_size);

        flipc2_produce_commit(pair.fwd_ch);
        flipc2_consume_wait(pair.rev_ch, FLIPC2_BENCH_SPIN);
        flipc2_consume_release(pair.rev_ch);
    }
    flipc2_get_time(&t1);

    flipc2_print_result(label, flipc2_elapsed_ns(&t0, &t1), iters);

    /* Stop echo thread */
    args.running = 0;
    FLIPC2_WRITE_FENCE();
    d = flipc2_produce_reserve(pair.fwd_ch);
    if (d) {
        d->opcode = 0xFFFF;
        d->data_offset = 0;
        d->data_length = 0;
        d->flags = 0;
        d->status = 0;
        flipc2_produce_commit(pair.fwd_ch);
    }
    semaphore_signal(pair.fwd_ch->sem_port);
    pthread_join(ct, NULL);

    flipc2_bufgroup_destroy(bg);
    flipc2_pair_destroy(&pair);
}

/* ===================================================================
 * Benchmark: inter-task RPC with buffer group (vm_remap shared pool).
 *
 * Parent allocs slot from bufgroup, fills data, sends descriptor
 * with FLIPC2_FLAG_DATA_BUFGROUP to child task.  Child reads data
 * from the shared pool, frees the slot (CAS), sends null reply.
 *
 * Setup is inline (not via flipc2_inter_setup) because we need to
 * share the bufgroup and patch child args BEFORE thread_resume.
 * =================================================================== */

void
bench_flipc2_bufgroup_inter_rpc(const char *label, int data_size, int iters)
{
    flipc2_channel_t    fwd_ch, rev_ch;
    mach_port_t         fwd_sem, rev_sem;
    flipc2_bufgroup_t   bg;
    flipc2_return_t     ret;
    kern_return_t       kr;
    mach_port_t         child_task, child_thread;
    vm_offset_t         child_stack;
    vm_address_t        child_fwd_addr, child_rev_addr, child_bg_addr;
    struct i386_thread_state state;
    mach_msg_type_number_t state_count;
    struct flipc2_desc  *d;
    tvalspec_t          t0, t1;
    uint64_t            offset;
    int                 i;

    /* Create channels */
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

    /* Create buffer group */
    ret = flipc2_bufgroup_create(FLIPC2_BENCH_BG_POOL_SIZE,
                                 FLIPC2_BENCH_BG_SLOT_SIZE, &bg);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: bufgroup create failed %d\n", label, ret);
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    /* Populate child args (inherited COW by task_create) */
    flipc2_child_args_storage.ring_offset  = fwd_ch->hdr->ring_offset;
    flipc2_child_args_storage.ring_mask    = fwd_ch->hdr->ring_mask;
    flipc2_child_args_storage.data_offset  = fwd_ch->hdr->data_offset;
    flipc2_child_args_storage.ring_entries = fwd_ch->hdr->ring_entries;
    flipc2_child_args_storage.fwd_sem      = FLIPC2_CHILD_SEM_FWD;
    flipc2_child_args_storage.rev_sem      = FLIPC2_CHILD_SEM_REV;
    flipc2_child_args_storage.data_size    = data_size;
    flipc2_child_args_storage.bg_data_offset  = bg->hdr->data_offset;
    flipc2_child_args_storage.bg_slot_stride  = bg->hdr->slot_stride;
    flipc2_child_args_storage.bg_slot_count   = bg->hdr->slot_count;
    flipc2_child_args_storage.bg_next_offset  = bg->hdr->next_offset;
    flipc2_child_args_storage.prod_tail_off =
        (uint32_t)((uint8_t *)fwd_ch->prod_tail - (uint8_t *)fwd_ch->hdr);
    flipc2_child_args_storage.cons_head_off =
        (uint32_t)((uint8_t *)fwd_ch->cons_head - (uint8_t *)fwd_ch->hdr);

    /* Create child task (inherits memory COW) */
    kr = task_create(mach_task_self(),
                     (ledger_port_array_t)0, 0,
                     TRUE, &child_task);
    if (kr) {
        printf("  %s: task_create failed %d\n", label, kr);
        flipc2_bufgroup_destroy(bg);
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    /* Reset _mig_multithreaded in child (see flipc2_bench_inter.c) */
    {
        int zero = 0;
        vm_write(child_task, (vm_address_t)&_mig_multithreaded,
                 (vm_address_t)&zero, sizeof(zero));
    }

    /* Share channels via vm_remap */
    ret = flipc2_channel_share(fwd_ch, child_task, &child_fwd_addr);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: channel_share fwd failed %d\n", label, ret);
        task_terminate(child_task);
        flipc2_bufgroup_destroy(bg);
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    ret = flipc2_channel_share(rev_ch, child_task, &child_rev_addr);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: channel_share rev failed %d\n", label, ret);
        task_terminate(child_task);
        flipc2_bufgroup_destroy(bg);
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    /* Share buffer group via vm_remap */
    ret = flipc2_bufgroup_share(bg, child_task, &child_bg_addr);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: bufgroup_share failed %d\n", label, ret);
        task_terminate(child_task);
        flipc2_bufgroup_destroy(bg);
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    /* Patch child's copy of args with child-side addresses */
    {
        struct flipc2_child_args child_args;
        child_args.fwd_base       = child_fwd_addr;
        child_args.rev_base       = child_rev_addr;
        child_args.ring_offset    = fwd_ch->hdr->ring_offset;
        child_args.ring_mask      = fwd_ch->hdr->ring_mask;
        child_args.data_offset    = fwd_ch->hdr->data_offset;
        child_args.ring_entries   = fwd_ch->hdr->ring_entries;
        child_args.fwd_sem        = FLIPC2_CHILD_SEM_FWD;
        child_args.rev_sem        = FLIPC2_CHILD_SEM_REV;
        child_args.data_size      = data_size;
        child_args.bg_base        = child_bg_addr;
        child_args.bg_data_offset = bg->hdr->data_offset;
        child_args.bg_slot_stride = bg->hdr->slot_stride;
        child_args.bg_slot_count  = bg->hdr->slot_count;
        child_args.bg_next_offset = bg->hdr->next_offset;
        child_args.prod_tail_off =
            (uint32_t)((uint8_t *)fwd_ch->prod_tail - (uint8_t *)fwd_ch->hdr);
        child_args.cons_head_off =
            (uint32_t)((uint8_t *)fwd_ch->cons_head - (uint8_t *)fwd_ch->hdr);

        kr = vm_write(child_task,
                      (vm_address_t)&flipc2_child_args_storage,
                      (vm_address_t)&child_args,
                      sizeof(child_args));
        if (kr) {
            printf("  %s: vm_write args failed %d\n", label, kr);
            task_terminate(child_task);
            flipc2_bufgroup_destroy(bg);
            flipc2_channel_destroy(rev_ch);
            flipc2_channel_destroy(fwd_ch);
            return;
        }
    }

    /* Share semaphores with child */
    ret = flipc2_semaphore_share(fwd_sem, child_task, FLIPC2_CHILD_SEM_FWD);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: semaphore_share fwd failed %d\n", label, ret);
        task_terminate(child_task);
        flipc2_bufgroup_destroy(bg);
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    ret = flipc2_semaphore_share(rev_sem, child_task, FLIPC2_CHILD_SEM_REV);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: semaphore_share rev failed %d\n", label, ret);
        task_terminate(child_task);
        flipc2_bufgroup_destroy(bg);
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    /* Allocate child stack, create and start thread */
    child_stack = 0;
    kr = vm_allocate(child_task, &child_stack, FLIPC2_CHILD_STACK_SIZE, TRUE);
    if (kr) {
        printf("  %s: child stack failed %d\n", label, kr);
        task_terminate(child_task);
        flipc2_bufgroup_destroy(bg);
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    kr = thread_create(child_task, &child_thread);
    if (kr) {
        printf("  %s: thread_create failed %d\n", label, kr);
        task_terminate(child_task);
        flipc2_bufgroup_destroy(bg);
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    state_count = i386_THREAD_STATE_COUNT;
    thread_get_state(child_thread, i386_THREAD_STATE,
                     (thread_state_t)&state, &state_count);
    state.eip  = (unsigned int)flipc2_child_bg_echo_entry;
    state.uesp = (unsigned int)(child_stack + FLIPC2_CHILD_STACK_SIZE);
    thread_set_state(child_thread, i386_THREAD_STATE,
                     (thread_state_t)&state, i386_THREAD_STATE_COUNT);
    thread_resume(child_thread);
    thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);

    /*
     * Both parent and child use semaphore-based wait/signal.
     * The child can use MIG stubs because the parent resets
     * _mig_multithreaded=0 via vm_write, so mig_get_reply_port
     * falls back to the global reply port.
     */

    /* Warmup */
    for (i = 0; i < FLIPC2_WARMUP_ITERS; i++) {
        offset = flipc2_bufgroup_alloc(bg);

        d = flipc2_produce_reserve(fwd_ch);
        d->opcode = 1;
        d->cookie = i;
        d->flags = FLIPC2_FLAG_DATA_BUFGROUP;
        d->status = 0;
        d->data_offset = offset;
        d->data_length = data_size;
        memset(flipc2_bufgroup_data(bg, offset), 0xCC, data_size);

        flipc2_produce_commit(fwd_ch);

        flipc2_consume_wait(rev_ch, FLIPC2_BENCH_SPIN);
        flipc2_consume_release(rev_ch);
    }

    /* Timed run */
    flipc2_get_time(&t0);
    for (i = 0; i < iters; i++) {
        offset = flipc2_bufgroup_alloc(bg);

        d = flipc2_produce_reserve(fwd_ch);
        d->opcode = 1;
        d->cookie = i;
        d->flags = FLIPC2_FLAG_DATA_BUFGROUP;
        d->status = 0;
        d->data_offset = offset;
        d->data_length = data_size;
        memset(flipc2_bufgroup_data(bg, offset), 0xCC, data_size);

        flipc2_produce_commit(fwd_ch);

        /* Spin-wait for child reply with yield */
        while (*rev_ch->prod_tail == *rev_ch->cons_head)
            thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0);
        flipc2_consume_release(rev_ch);
    }
    flipc2_get_time(&t1);

    flipc2_print_result(label, flipc2_elapsed_ns(&t0, &t1), iters);

    /* Cleanup */
    task_terminate(child_task);
    mach_port_deallocate(mach_task_self(), child_thread);
    mach_port_deallocate(mach_task_self(), child_task);
    flipc2_bufgroup_destroy(bg);
    flipc2_channel_destroy(fwd_ch);
    flipc2_channel_destroy(rev_ch);
}
