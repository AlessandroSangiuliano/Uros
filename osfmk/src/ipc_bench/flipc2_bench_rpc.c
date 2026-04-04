/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * MIT License — see flipc2_bench.h
 */

/*
 * flipc2_bench_rpc.c — Intra-task RPC benchmark (cthread echo, semaphore).
 */

#include "flipc2_bench.h"
#include <mach/thread_switch.h>
#include <stdio.h>
#include <string.h>

void
bench_flipc2_rpc(const char *label, int data_size, int iters)
{
    struct flipc2_pair p;
    struct flipc2_echo_args echo_args;
    pthread_t           echo_ct;
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
    pthread_create(&echo_ct, NULL, flipc2_echo_thread, &echo_args);
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

/*
 * Isolated channel intra-task RPC.
 * Same benchmark but with FLIPC2_CREATE_ISOLATED layout.
 * Intra-task has no vm_protect → verifies isolated header layout works.
 */
void
bench_flipc2_isolated_rpc(const char *label, int data_size, int iters)
{
    struct flipc2_pair p;
    struct flipc2_echo_args echo_args;
    pthread_t           echo_ct;
    struct flipc2_desc  *d;
    tvalspec_t          t0, t1;
    uint64_t            data_off = 0;
    int                 i;

    if (flipc2_pair_create_ex(&p, FLIPC2_CREATE_ISOLATED, label) != 0)
        return;

    echo_args.recv_ch  = p.fwd_peer;
    echo_args.send_ch  = p.rev_peer;
    echo_args.running  = 1;
    echo_args.data_size = data_size;
    pthread_create(&echo_ct, NULL, flipc2_echo_thread, &echo_args);
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
