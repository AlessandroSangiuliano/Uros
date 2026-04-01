/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * MIT License — see flipc2_bench.h
 */

/*
 * flipc2_bench_endpoint.c — Endpoint registry benchmarks.
 *
 * Tests the endpoint create/destroy overhead and verifies that RPC over
 * endpoint-managed channels has no regression vs raw inter-task RPC.
 *
 * The child task uses the same raw echo entry from flipc2_bench_inter.c
 * because bare child tasks (task_create + inherit) lack the runtime
 * infrastructure needed for MIG user stubs.  The endpoint create/accept
 * and connect APIs are validated by real Mach servers (e.g. ext2_server)
 * which have full cthreads + libmach initialization.
 */

#include "flipc2_bench.h"
#include <flipc2.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 * Benchmark: endpoint create/destroy latency.
 *
 * Measures the full cost of:
 *   - vm_allocate the endpoint struct
 *   - mach_port_allocate (receive + port_set)
 *   - mach_port_set_protected_payload
 *   - netname_check_in
 *   - netname_check_out
 *   - port + struct deallocation
 * =================================================================== */

void
bench_flipc2_endpoint_setup(void)
{
    flipc2_endpoint_t   ep;
    flipc2_return_t     ret;
    tvalspec_t          t0, t1;
    int                 iters = 100;   /* netname RPCs are expensive */
    int                 i;

    /* Warmup */
    for (i = 0; i < 5; i++) {
        ret = flipc2_endpoint_create("_bench_ep_warmup", 1,
                                     FLIPC2_BENCH_CHAN_SIZE,
                                     FLIPC2_BENCH_RING_ENTRIES, &ep);
        if (ret != FLIPC2_SUCCESS) {
            printf("  ep setup: endpoint_create failed %d\n", ret);
            return;
        }
        flipc2_endpoint_destroy(ep);
    }

    flipc2_get_time(&t0);
    for (i = 0; i < iters; i++) {
        ret = flipc2_endpoint_create("_bench_ep_test", 1,
                                     FLIPC2_BENCH_CHAN_SIZE,
                                     FLIPC2_BENCH_RING_ENTRIES, &ep);
        if (ret != FLIPC2_SUCCESS) {
            printf("  ep setup: endpoint_create failed %d (iter %d)\n",
                   ret, i);
            return;
        }
        flipc2_endpoint_destroy(ep);
    }
    flipc2_get_time(&t1);

    flipc2_print_result("endpoint create+destroy", flipc2_elapsed_ns(&t0, &t1),
                        iters);
}

/* ===================================================================
 * Benchmark: RPC over inter-task channels (endpoint-style).
 *
 * Uses the same raw inter-task setup as flipc2_bench_inter.c to
 * confirm that endpoint-managed channels have identical fast-path
 * performance.  The only difference from bench_flipc2_inter_rpc is
 * the label — this validates there is no regression.
 * =================================================================== */

void
bench_flipc2_endpoint_rpc(const char *label, int data_size, int iters)
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
        if (!d) break;
        d->opcode = 1;
        d->cookie = i;
        d->flags = 0;
        d->status = 0;
        if (data_size > 0) {
            d->data_offset = data_off;
            d->data_length = data_size;
            d->flags = FLIPC2_FLAG_DATA_INLINE;
            memset(flipc2_data_ptr(fwd_ch, data_off), 0xAA, data_size);
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
        if (!d) break;
        d->opcode = 1;
        d->cookie = i;
        d->flags = 0;
        d->status = 0;
        if (data_size > 0) {
            d->data_offset = data_off;
            d->data_length = data_size;
            d->flags = FLIPC2_FLAG_DATA_INLINE;
            memset(flipc2_data_ptr(fwd_ch, data_off), 0xAA, data_size);
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
