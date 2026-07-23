/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * MIT License — see flipc2_bench.h
 */

/*
 * flipc2_bench_throughput.c — Single-thread throughput tests.
 *
 * Produce + consume in the same thread, no kernel involvement.
 * This measures pure ring buffer + memory bandwidth.
 */

#include "flipc2_bench.h"
#include <stdio.h>
#include <string.h>

void
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

void
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
