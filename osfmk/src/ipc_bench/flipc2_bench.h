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

#ifndef FLIPC2_BENCH_H
#define FLIPC2_BENCH_H

#include <flipc2.h>
#include <mach/port.h>
#include <mach/clock_types.h>
#include <cthreads.h>

/* ------------------------------------------------------------------ */
/*  Configuration                                                      */
/* ------------------------------------------------------------------ */

#define FLIPC2_WARMUP_ITERS     100
#define FLIPC2_BENCH_ITERS      10000
#define FLIPC2_BENCH_SPIN       0   /* cthreads are cooperative: spin is useless */
#define FLIPC2_BENCH_CHAN_SIZE  (64 * 1024)
#define FLIPC2_BENCH_RING_ENTRIES 256
#define FLIPC2_CHILD_STACK_SIZE (64 * 1024)

/* Well-known child port names (must not collide with ipc_bench's) */
#define FLIPC2_CHILD_SEM_FWD   ((mach_port_t) 0x2503)
#define FLIPC2_CHILD_SEM_REV   ((mach_port_t) 0x2603)

/* Game simulation constants */
#define GAME_DRAW_CMDS_PER_FRAME  60
#define GAME_TEXTURE_CHUNK_SIZE   16384   /* 16 KB */
#define GAME_AUDIO_CHUNK_SIZE     4096    /* 4 KB */
#define GAME_FRAMES               1000

/* Buffer group benchmark constants */
#define FLIPC2_BENCH_BG_POOL_SIZE   (64 * 1024)   /* 64 KB pool */
#define FLIPC2_BENCH_BG_SLOT_SIZE   256            /* 256 bytes/slot */

/* ------------------------------------------------------------------ */
/*  Shared helpers (flipc2_bench_common.c)                             */
/* ------------------------------------------------------------------ */

extern mach_port_t flipc2_clock_port;

void flipc2_get_time(tvalspec_t *tv);
unsigned long flipc2_elapsed_ns(const tvalspec_t *before,
                                const tvalspec_t *after);
void flipc2_print_result(const char *label, unsigned long total_ns, int iters);

/* Channel pair: fwd + rev with peer handles */
struct flipc2_pair {
    flipc2_channel_t    fwd_ch;
    flipc2_channel_t    rev_ch;
    flipc2_channel_t    fwd_peer;
    flipc2_channel_t    rev_peer;
    mach_port_t         fwd_sem;
    mach_port_t         rev_sem;
};

int  flipc2_pair_create(struct flipc2_pair *p, const char *label);
void flipc2_pair_destroy(struct flipc2_pair *p);

/* Echo thread for intra-task RPC */
struct flipc2_echo_args {
    flipc2_channel_t        recv_ch;
    flipc2_channel_t        send_ch;
    volatile int            running;
    int                     data_size;
};

void *flipc2_echo_thread(void *arg);
void  flipc2_stop_echo(struct flipc2_echo_args *args, flipc2_channel_t fwd_ch,
                        cthread_t ct);

/* ------------------------------------------------------------------ */
/*  Inter-task helpers (flipc2_bench_inter.c)                          */
/* ------------------------------------------------------------------ */

/* Args struct for child echo task (shared via inherited memory) */
struct flipc2_child_args {
    vm_address_t    fwd_base;
    vm_address_t    rev_base;
    uint32_t        ring_offset;
    uint32_t        ring_mask;
    uint32_t        data_offset;
    uint32_t        ring_entries;
    mach_port_t     fwd_sem;
    mach_port_t     rev_sem;
    int             data_size;
};

extern volatile struct flipc2_child_args flipc2_child_args_storage;

/* Child entry points (noreturn, used from inter-task setup) */
void flipc2_child_echo_entry(void);
void flipc2_child_batch_echo_entry(void);

int  flipc2_inter_setup(flipc2_channel_t fwd_ch, flipc2_channel_t rev_ch,
                         mach_port_t fwd_sem, mach_port_t rev_sem,
                         int data_size, void (*child_entry)(void),
                         const char *label,
                         mach_port_t *out_task, mach_port_t *out_thread);
void flipc2_inter_cleanup(mach_port_t child_task, mach_port_t child_thread,
                           flipc2_channel_t fwd_ch, flipc2_channel_t rev_ch);

/* ------------------------------------------------------------------ */
/*  Benchmark functions                                                */
/* ------------------------------------------------------------------ */

/* Throughput (flipc2_bench_throughput.c) */
void bench_flipc2_throughput(const char *label, int batch_size, int iters);
void bench_flipc2_data_throughput(const char *label, int data_size, int iters);

/* Intra-task RPC (flipc2_bench_rpc.c) */
void bench_flipc2_rpc(const char *label, int data_size, int iters);

/* Inter-task RPC + batch (flipc2_bench_inter.c) */
void bench_flipc2_inter_rpc(const char *label, int data_size, int iters);
void bench_flipc2_inter_batch(const char *label, int batch_size, int iters);

/* Game simulation (flipc2_bench_game.c) */
void bench_game_draw_commands(void);
void bench_game_texture_stream(void);
void bench_game_audio_buffer(void);
void bench_game_mixed_frame(void);
void bench_game_mixed_frame_inter(void);

/* Endpoint benchmarks (flipc2_bench_endpoint.c) */
void bench_flipc2_endpoint_setup(void);
void bench_flipc2_endpoint_rpc(const char *label, int data_size, int iters);

/* Buffer group benchmarks (flipc2_bench_bufgroup.c) */
void bench_flipc2_bufgroup_alloc_free(void);
void bench_flipc2_bufgroup_rpc(const char *label, int data_size, int iters);

/* Main entry point */
void bench_flipc2_run(mach_port_t clock_port);

#endif /* FLIPC2_BENCH_H */
