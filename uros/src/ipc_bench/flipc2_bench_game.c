/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * MIT License — see flipc2_bench.h
 */

/*
 * flipc2_bench_game.c — Game simulation benchmarks over FLIPC v2.
 *
 * Simulates realistic game workloads:
 *   - Draw commands: 60 small descriptors batched (one frame @60fps)
 *   - Texture streaming: 16 KB chunks (texture upload)
 *   - Audio buffers: 4 KB chunks (PCM audio frames)
 *   - Mixed frame: draw batch + texture + audio in one frame
 *   - Mixed frame inter-task: same across two tasks (vm_remap)
 */

#include "flipc2_bench.h"
#include <stdio.h>
#include <string.h>

/* ===================================================================
 * Draw command batch: 60 null descriptors per frame, batch-committed.
 * Single-thread throughput — no kernel.
 * =================================================================== */

void
bench_game_draw_commands(void)
{
    flipc2_channel_t    ch;
    flipc2_channel_t    consumer;
    mach_port_t         sem;
    flipc2_return_t     ret;
    struct flipc2_desc  *d;
    tvalspec_t          t0, t1;
    int                 frame, cmd;

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &ch, &sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  game draw: create failed %d\n", ret);
        return;
    }

    ret = flipc2_channel_attach((vm_address_t)ch->hdr, &consumer);
    if (ret != FLIPC2_SUCCESS) {
        flipc2_channel_destroy(ch);
        return;
    }

    /* Warmup */
    for (frame = 0; frame < 10; frame++) {
        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            d = flipc2_produce_reserve(ch);
            d->opcode = 0x10;  /* DRAW_TRIANGLES */
            d->cookie = cmd;
            d->param[0] = 1000 + cmd;  /* vertex count */
            d->param[1] = 0;           /* texture id */
            d->data_offset = 0;
            d->data_length = 0;
            d->flags = (cmd < GAME_DRAW_CMDS_PER_FRAME - 1)
                       ? FLIPC2_FLAG_BATCH : 0;
            d->status = 0;
        }
        flipc2_produce_commit_n(ch, GAME_DRAW_CMDS_PER_FRAME);

        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            flipc2_consume_peek(consumer);
            flipc2_consume_release(consumer);
        }
    }

    /* Timed run */
    flipc2_get_time(&t0);
    for (frame = 0; frame < GAME_FRAMES; frame++) {
        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            d = flipc2_produce_reserve(ch);
            d->opcode = 0x10;
            d->cookie = cmd;
            d->param[0] = 1000 + cmd;
            d->param[1] = frame;
            d->data_offset = 0;
            d->data_length = 0;
            d->flags = (cmd < GAME_DRAW_CMDS_PER_FRAME - 1)
                       ? FLIPC2_FLAG_BATCH : 0;
            d->status = 0;
        }
        flipc2_produce_commit_n(ch, GAME_DRAW_CMDS_PER_FRAME);

        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            flipc2_consume_peek(consumer);
            flipc2_consume_release(consumer);
        }
    }
    flipc2_get_time(&t1);

    {
        unsigned long total_ns = flipc2_elapsed_ns(&t0, &t1);
        unsigned long ns_per_frame = total_ns / GAME_FRAMES;
        unsigned long us_frame = ns_per_frame / 1000;
        unsigned long us_frac  = (ns_per_frame % 1000) / 10;

        printf("  draw cmds (60/frame, batched)       "
               "%5lu.%02lu us/frame (%d frames, %lu us total)\n",
               us_frame, us_frac, GAME_FRAMES, total_ns / 1000);
        flipc2_print_result("  per draw command",
                            total_ns, GAME_FRAMES * GAME_DRAW_CMDS_PER_FRAME);
    }

    flipc2_channel_detach(consumer);
    flipc2_channel_destroy(ch);
}

/* ===================================================================
 * Texture streaming: 16 KB chunks with memset (simulates DMA fill).
 * =================================================================== */

void
bench_game_texture_stream(void)
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
        printf("  game texture: create failed %d\n", ret);
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
        d->opcode = 0x20;  /* TEXTURE_UPLOAD */
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = data_off;
        d->data_length = GAME_TEXTURE_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, data_off), 0xDD, GAME_TEXTURE_CHUNK_SIZE);
        flipc2_produce_commit(ch);
        flipc2_consume_peek(consumer);
        flipc2_consume_release(consumer);
    }

    flipc2_get_time(&t0);
    for (i = 0; i < FLIPC2_BENCH_ITERS; i++) {
        d = flipc2_produce_reserve(ch);
        d->opcode = 0x20;
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = data_off;
        d->data_length = GAME_TEXTURE_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, data_off), 0xDD, GAME_TEXTURE_CHUNK_SIZE);
        flipc2_produce_commit(ch);
        flipc2_consume_peek(consumer);
        flipc2_consume_release(consumer);
    }
    flipc2_get_time(&t1);

    flipc2_print_result("texture 16KB chunk",
                        flipc2_elapsed_ns(&t0, &t1), FLIPC2_BENCH_ITERS);

    flipc2_channel_detach(consumer);
    flipc2_channel_destroy(ch);
}

/* ===================================================================
 * Audio buffer: 4 KB PCM frames.
 * =================================================================== */

void
bench_game_audio_buffer(void)
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
        printf("  game audio: create failed %d\n", ret);
        return;
    }

    ret = flipc2_channel_attach((vm_address_t)ch->hdr, &consumer);
    if (ret != FLIPC2_SUCCESS) {
        flipc2_channel_destroy(ch);
        return;
    }

    for (i = 0; i < FLIPC2_WARMUP_ITERS; i++) {
        d = flipc2_produce_reserve(ch);
        d->opcode = 0x30;  /* AUDIO_PCM */
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = data_off;
        d->data_length = GAME_AUDIO_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, data_off), 0x80, GAME_AUDIO_CHUNK_SIZE);
        flipc2_produce_commit(ch);
        flipc2_consume_peek(consumer);
        flipc2_consume_release(consumer);
    }

    flipc2_get_time(&t0);
    for (i = 0; i < FLIPC2_BENCH_ITERS; i++) {
        d = flipc2_produce_reserve(ch);
        d->opcode = 0x30;
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = data_off;
        d->data_length = GAME_AUDIO_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, data_off), 0x80, GAME_AUDIO_CHUNK_SIZE);
        flipc2_produce_commit(ch);
        flipc2_consume_peek(consumer);
        flipc2_consume_release(consumer);
    }
    flipc2_get_time(&t1);

    flipc2_print_result("audio 4KB PCM frame",
                        flipc2_elapsed_ns(&t0, &t1), FLIPC2_BENCH_ITERS);

    flipc2_channel_detach(consumer);
    flipc2_channel_destroy(ch);
}

/* ===================================================================
 * Mixed frame: per-frame batch of 60 draw cmds + 1 texture + 1 audio.
 * Simulates one complete game frame submission (intra-task).
 * =================================================================== */

void
bench_game_mixed_frame(void)
{
    flipc2_channel_t    ch;
    flipc2_channel_t    consumer;
    mach_port_t         sem;
    flipc2_return_t     ret;
    struct flipc2_desc  *d;
    tvalspec_t          t0, t1;
    int                 frame, cmd;
    uint64_t            tex_off = 0;
    uint64_t            audio_off = GAME_TEXTURE_CHUNK_SIZE;
    int                 total_descs = GAME_DRAW_CMDS_PER_FRAME + 2;

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &ch, &sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  game mixed: create failed %d\n", ret);
        return;
    }

    ret = flipc2_channel_attach((vm_address_t)ch->hdr, &consumer);
    if (ret != FLIPC2_SUCCESS) {
        flipc2_channel_destroy(ch);
        return;
    }

    /* Warmup */
    for (frame = 0; frame < 10; frame++) {
        /* 60 draw commands */
        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            d = flipc2_produce_reserve(ch);
            d->opcode = 0x10;
            d->cookie = cmd;
            d->param[0] = 1000 + cmd;
            d->data_offset = 0;
            d->data_length = 0;
            d->flags = FLIPC2_FLAG_BATCH;
            d->status = 0;
        }
        /* texture upload */
        d = flipc2_produce_reserve(ch);
        d->opcode = 0x20;
        d->flags = FLIPC2_FLAG_DATA_INLINE | FLIPC2_FLAG_BATCH;
        d->data_offset = tex_off;
        d->data_length = GAME_TEXTURE_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, tex_off), 0xDD, GAME_TEXTURE_CHUNK_SIZE);
        /* audio buffer */
        d = flipc2_produce_reserve(ch);
        d->opcode = 0x30;
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = audio_off;
        d->data_length = GAME_AUDIO_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, audio_off), 0x80, GAME_AUDIO_CHUNK_SIZE);

        flipc2_produce_commit_n(ch, total_descs);

        for (cmd = 0; cmd < total_descs; cmd++) {
            flipc2_consume_peek(consumer);
            flipc2_consume_release(consumer);
        }
    }

    /* Timed run */
    flipc2_get_time(&t0);
    for (frame = 0; frame < GAME_FRAMES; frame++) {
        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            d = flipc2_produce_reserve(ch);
            d->opcode = 0x10;
            d->cookie = cmd;
            d->param[0] = 1000 + cmd;
            d->param[1] = frame;
            d->data_offset = 0;
            d->data_length = 0;
            d->flags = FLIPC2_FLAG_BATCH;
            d->status = 0;
        }
        d = flipc2_produce_reserve(ch);
        d->opcode = 0x20;
        d->flags = FLIPC2_FLAG_DATA_INLINE | FLIPC2_FLAG_BATCH;
        d->data_offset = tex_off;
        d->data_length = GAME_TEXTURE_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, tex_off), 0xDD, GAME_TEXTURE_CHUNK_SIZE);
        d = flipc2_produce_reserve(ch);
        d->opcode = 0x30;
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = audio_off;
        d->data_length = GAME_AUDIO_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(ch, audio_off), 0x80, GAME_AUDIO_CHUNK_SIZE);

        flipc2_produce_commit_n(ch, total_descs);

        for (cmd = 0; cmd < total_descs; cmd++) {
            flipc2_consume_peek(consumer);
            flipc2_consume_release(consumer);
        }
    }
    flipc2_get_time(&t1);

    {
        unsigned long total_ns = flipc2_elapsed_ns(&t0, &t1);
        unsigned long ns_per_frame = total_ns / GAME_FRAMES;
        unsigned long us_frame = ns_per_frame / 1000;
        unsigned long us_frac  = (ns_per_frame % 1000) / 10;
        unsigned long budget_us = 16666; /* 16.67 ms @ 60fps */
        unsigned long pct = (ns_per_frame / 10) / (budget_us / 10);

        printf("  mixed frame (60 draw+tex+audio)     "
               "%5lu.%02lu us/frame (%d frames)\n",
               us_frame, us_frac, GAME_FRAMES);
        printf("  frame budget @ 60fps: %lu.%02lu us / 16666 us = %lu.%lu%%\n",
               us_frame, us_frac,
               pct / 10, pct % 10);
    }

    flipc2_channel_detach(consumer);
    flipc2_channel_destroy(ch);
}

/* ===================================================================
 * Mixed frame inter-task: engine → renderer pipeline across tasks.
 * Uses batch echo child (vm_remap shared memory).
 * =================================================================== */

void
bench_game_mixed_frame_inter(void)
{
    flipc2_channel_t    fwd_ch, rev_ch;
    mach_port_t         fwd_sem, rev_sem;
    mach_port_t         child_task, child_thread;
    flipc2_return_t     ret;
    struct flipc2_desc  *d;
    tvalspec_t          t0, t1;
    int                 frame, cmd;
    uint64_t            tex_off = 0;
    uint64_t            audio_off = GAME_TEXTURE_CHUNK_SIZE;
    int                 total_descs = GAME_DRAW_CMDS_PER_FRAME + 2;

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &fwd_ch, &fwd_sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  game inter: fwd create failed %d\n", ret);
        return;
    }

    ret = flipc2_channel_create(FLIPC2_BENCH_CHAN_SIZE,
                                FLIPC2_BENCH_RING_ENTRIES,
                                &rev_ch, &rev_sem);
    if (ret != FLIPC2_SUCCESS) {
        printf("  game inter: rev create failed %d\n", ret);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    if (flipc2_inter_setup(fwd_ch, rev_ch, fwd_sem, rev_sem,
                           0, flipc2_child_batch_echo_entry,
                           "game inter",
                           &child_task, &child_thread) != 0) {
        flipc2_channel_destroy(rev_ch);
        flipc2_channel_destroy(fwd_ch);
        return;
    }

    /* Warmup */
    for (frame = 0; frame < 10; frame++) {
        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            d = flipc2_produce_reserve(fwd_ch);
            d->opcode = 0x10;
            d->cookie = cmd;
            d->param[0] = 1000 + cmd;
            d->data_offset = 0;
            d->data_length = 0;
            d->flags = FLIPC2_FLAG_BATCH;
            d->status = 0;
        }
        d = flipc2_produce_reserve(fwd_ch);
        d->opcode = 0x20;
        d->flags = FLIPC2_FLAG_DATA_INLINE | FLIPC2_FLAG_BATCH;
        d->data_offset = tex_off;
        d->data_length = GAME_TEXTURE_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(fwd_ch, tex_off), 0xDD,
               GAME_TEXTURE_CHUNK_SIZE);
        d = flipc2_produce_reserve(fwd_ch);
        d->opcode = 0x30;
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = audio_off;
        d->data_length = GAME_AUDIO_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(fwd_ch, audio_off), 0x80,
               GAME_AUDIO_CHUNK_SIZE);

        flipc2_produce_commit_n(fwd_ch, total_descs);

        /* Wait for child to process batch and reply */
        for (cmd = 0; cmd < total_descs; cmd++) {
            flipc2_consume_wait(rev_ch, FLIPC2_BENCH_SPIN);
            flipc2_consume_release(rev_ch);
        }
    }

    /* Timed run */
    flipc2_get_time(&t0);
    for (frame = 0; frame < GAME_FRAMES; frame++) {
        for (cmd = 0; cmd < GAME_DRAW_CMDS_PER_FRAME; cmd++) {
            d = flipc2_produce_reserve(fwd_ch);
            d->opcode = 0x10;
            d->cookie = cmd;
            d->param[0] = 1000 + cmd;
            d->param[1] = frame;
            d->data_offset = 0;
            d->data_length = 0;
            d->flags = FLIPC2_FLAG_BATCH;
            d->status = 0;
        }
        d = flipc2_produce_reserve(fwd_ch);
        d->opcode = 0x20;
        d->flags = FLIPC2_FLAG_DATA_INLINE | FLIPC2_FLAG_BATCH;
        d->data_offset = tex_off;
        d->data_length = GAME_TEXTURE_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(fwd_ch, tex_off), 0xDD,
               GAME_TEXTURE_CHUNK_SIZE);
        d = flipc2_produce_reserve(fwd_ch);
        d->opcode = 0x30;
        d->flags = FLIPC2_FLAG_DATA_INLINE;
        d->data_offset = audio_off;
        d->data_length = GAME_AUDIO_CHUNK_SIZE;
        d->status = 0;
        memset(flipc2_data_ptr(fwd_ch, audio_off), 0x80,
               GAME_AUDIO_CHUNK_SIZE);

        flipc2_produce_commit_n(fwd_ch, total_descs);

        for (cmd = 0; cmd < total_descs; cmd++) {
            flipc2_consume_wait(rev_ch, FLIPC2_BENCH_SPIN);
            flipc2_consume_release(rev_ch);
        }
    }
    flipc2_get_time(&t1);

    {
        unsigned long total_ns = flipc2_elapsed_ns(&t0, &t1);
        unsigned long ns_per_frame = total_ns / GAME_FRAMES;
        unsigned long us_frame = ns_per_frame / 1000;
        unsigned long us_frac  = (ns_per_frame % 1000) / 10;
        unsigned long budget_us = 16666;
        unsigned long pct = (ns_per_frame / 10) / (budget_us / 10);

        printf("  mixed frame INTER-TASK (62 desc)     "
               "%5lu.%02lu us/frame (%d frames)\n",
               us_frame, us_frac, GAME_FRAMES);
        printf("  frame budget @ 60fps: %lu.%02lu us / 16666 us = %lu.%lu%%\n",
               us_frame, us_frac,
               pct / 10, pct % 10);
    }

    flipc2_inter_cleanup(child_task, child_thread, fwd_ch, rev_ch);
}
