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
 */

#include "flipc2_bench.h"
#include <mach/thread_switch.h>
#include <cthreads.h>
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
    cthread_t                   ct;
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

    ct = cthread_fork((cthread_fn_t)flipc2_bg_echo_thread, (void *)&args);
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
    cthread_join(ct);

    flipc2_bufgroup_destroy(bg);
    flipc2_pair_destroy(&pair);
}
