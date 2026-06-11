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
#include <mach/urmach_futex.h>		/* #324 futex microbench */
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
                 pthread_t ct)
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

    pthread_join(ct, NULL);
}

/* ===================================================================
 * #324 futex microbench: ping-pong round-trip, urmach_futex vs Mach
 * semaphore.  Two threads in the same task hand a token back and forth
 * over a shared word (futex) or a pair of Mach semaphores.  Measures
 * the pure block+wake round-trip latency that FLIPC's sleep path pays.
 * =================================================================== */

static volatile unsigned int fx_turn;	/* 0 = main's turn, 1 = peer's turn */
static volatile int fx_running;		/* cleared by main to stop the peer  */
static int          fx_iters;
static mach_port_t  fx_sem_a, fx_sem_b;

static void *
fx_futex_peer(void *arg)
{
    (void)arg;
    while (fx_running) {
        fx_turn = 0;			/* hand the turn to main */
        FLIPC2_WRITE_FENCE();
        /* wake main + block (with direct hand-off) while it stays main's turn */
        while (fx_turn == 0 && fx_running)
            urmach_futex((unsigned int *)&fx_turn, URMACH_FUTEX_WAKE_WAIT,
                         0, 0, (unsigned int *)&fx_turn);
    }
    return (void *)0;
}

static void *
fx_sem_peer(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < fx_iters; i++) {
        semaphore_wait(fx_sem_b);
        semaphore_signal(fx_sem_a);
    }
    return (void *)0;
}

void
bench_futex_pingpong(void)
{
    pthread_t       ct;
    tvalspec_t      t0, t1;
    int             i;
    kern_return_t   kr;

    fx_iters = FLIPC2_BENCH_ITERS;

    printf("\n--- #324 futex vs Mach semaphore ping-pong (block+wake round-trip) ---\n");

    /* ---- urmach_futex ping-pong ---- */
    fx_turn = 0;
    fx_running = 1;
    if (pthread_create(&ct, NULL, fx_futex_peer, NULL) != 0) {
        printf("  futex: pthread_create failed\n");
        return;
    }
    flipc2_get_time(&t0);
    for (i = 0; i < fx_iters; i++) {
        fx_turn = 1;			/* hand the turn to the peer */
        FLIPC2_WRITE_FENCE();
        /* wake peer + block (with direct hand-off) while it stays peer's turn */
        while (fx_turn == 1)
            urmach_futex((unsigned int *)&fx_turn, URMACH_FUTEX_WAKE_WAIT,
                         1, 0, (unsigned int *)&fx_turn);
    }
    flipc2_get_time(&t1);
    /* stop the peer: clear the flag, then unstick it from any park */
    fx_running = 0;
    fx_turn = 1;			/* peer's while(turn==0) condition goes false */
    FLIPC2_WRITE_FENCE();
    urmach_futex((unsigned int *)&fx_turn, URMACH_FUTEX_WAKE, 8, 0,
                 (unsigned int *)0);
    pthread_join(ct, NULL);
    flipc2_print_result("futex WAKE_WAIT ping-pong", flipc2_elapsed_ns(&t0, &t1), fx_iters);

    /* ---- Mach semaphore ping-pong (A/B baseline) ---- */
    kr = semaphore_create(mach_task_self(), &fx_sem_a, SYNC_POLICY_FIFO, 0);
    if (kr == KERN_SUCCESS)
        kr = semaphore_create(mach_task_self(), &fx_sem_b, SYNC_POLICY_FIFO, 0);
    if (kr != KERN_SUCCESS) {
        printf("  semaphore: create failed %d\n", kr);
        return;
    }
    if (pthread_create(&ct, NULL, fx_sem_peer, NULL) != 0) {
        printf("  semaphore: pthread_create failed\n");
        return;
    }
    flipc2_get_time(&t0);
    for (i = 0; i < fx_iters; i++) {
        semaphore_signal(fx_sem_b);
        semaphore_wait(fx_sem_a);
    }
    flipc2_get_time(&t1);
    pthread_join(ct, NULL);
    flipc2_print_result("semaphore ping-pong", flipc2_elapsed_ns(&t0, &t1), fx_iters);
}
