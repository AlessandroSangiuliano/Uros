/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * MIT License — see flipc2_bench.h
 */

/*
 * flipc2_bench_common.c — Shared helpers: timing, print, pair, echo thread.
 */

#include "flipc2_bench.h"
#include <mach.h>
#include <mach/mach_host.h>
#include <mach/clock.h>
#include <mach/thread_switch.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 * Timing helpers
 * =================================================================== */

mach_port_t flipc2_clock_port;

void
flipc2_get_time(tvalspec_t *tv)
{
    clock_get_time(flipc2_clock_port, tv);
}

unsigned long
flipc2_elapsed_ns(const tvalspec_t *before, const tvalspec_t *after)
{
    unsigned long ns;
    ns  = (unsigned long)(after->tv_sec  - before->tv_sec)  * 1000000000UL;
    ns += (unsigned long)(after->tv_nsec - before->tv_nsec);
    return ns;
}

void
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

int
flipc2_pair_create(struct flipc2_pair *p, const char *label)
{
    return flipc2_pair_create_ex(p, FLIPC2_CREATE_ISOLATED, label);
}

int
flipc2_pair_create_ex(struct flipc2_pair *p, uint32_t flags, const char *label)
{
    flipc2_return_t ret;
    uint32_t cs = FLIPC2_BENCH_CHAN_SIZE;

    /* Isolated layout needs at least 16KB + ring */
    if (flags & FLIPC2_CREATE_ISOLATED)
        cs = (cs < FLIPC2_CHANNEL_SIZE_MIN_ISOLATED)
             ? FLIPC2_CHANNEL_SIZE_MIN_ISOLATED : cs;

    ret = flipc2_channel_create_ex(cs, FLIPC2_BENCH_RING_ENTRIES,
                                   flags, &p->fwd_ch, &p->fwd_sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  %s: fwd create failed %d\n", label, ret);
        return -1;
    }

    ret = flipc2_channel_create_ex(cs, FLIPC2_BENCH_RING_ENTRIES,
                                   flags, &p->rev_ch, &p->rev_sem);
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

void
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

void *
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
void
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
    semaphore_signal(fwd_ch->sem_port);

    cthread_join(ct);
}
