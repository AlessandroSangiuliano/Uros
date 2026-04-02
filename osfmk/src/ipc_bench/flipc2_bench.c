/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * MIT License — see flipc2_bench.h
 */

/*
 * flipc2_bench.c — FLIPC v2 benchmark entry point.
 *
 * Calls all benchmark suites:
 *   1. Single-thread throughput (flipc2_bench_throughput.c)
 *   2. Intra-task RPC (flipc2_bench_rpc.c)
 *   3. Inter-task RPC + batch (flipc2_bench_inter.c)
 *   4. Game simulation (flipc2_bench_game.c)
 */

#include "flipc2_bench.h"
#include <stdio.h>

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

    /* --- Endpoint benchmarks --- */
    printf("\n--- FLIPC2 endpoint benchmarks ---\n");
    bench_flipc2_endpoint_setup();
    bench_flipc2_endpoint_rpc("null RPC (endpoint)",    0, FLIPC2_BENCH_ITERS);
    bench_flipc2_endpoint_rpc("128B RPC (endpoint)",  128, FLIPC2_BENCH_ITERS);

    /* --- Buffer group benchmarks --- */
    printf("\n--- FLIPC2 buffer group benchmarks ---\n");
    bench_flipc2_bufgroup_alloc_free();
    bench_flipc2_bufgroup_rpc("256B RPC (bufgroup)", 256, FLIPC2_BENCH_ITERS);
    bench_flipc2_bufgroup_inter_rpc("256B RPC (bufgroup inter)", 256,
                                     FLIPC2_BENCH_ITERS);

    /* --- Isolated channel benchmarks (page-protected layout) --- */
    printf("\n--- FLIPC2 isolated channel benchmarks ---\n");
    bench_flipc2_isolated_rpc("null RPC (isolated intra)",    0,
                               FLIPC2_BENCH_ITERS);
    bench_flipc2_isolated_rpc("128B RPC (isolated intra)",  128,
                               FLIPC2_BENCH_ITERS);
    bench_flipc2_isolated_inter_rpc("null RPC (isolated inter)",    0,
                                     FLIPC2_BENCH_ITERS);
    bench_flipc2_isolated_inter_rpc("128B RPC (isolated inter)",  128,
                                     FLIPC2_BENCH_ITERS);

    printf("\n");
}
