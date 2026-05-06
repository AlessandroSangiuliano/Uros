/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * gpu_server/text_render.c — Text rendering pipeline (#196).
 *
 * Pulls bytes off a bounded queue and feeds them to the active
 * back-end module's text_puts hook.  Exists to enforce the two
 * rules from docs/gpu_server_design.md §11.3:
 *
 *   rule 2: gpu_text_puts is a MIG simpleroutine — producer never
 *           blocks on VGA paint.  This file's queue absorbs the
 *           latency: enqueue costs ~one mutex+memcpy (≪ µs), the
 *           paint happens later on the worker thread.
 *
 *   rule 4: Single rendering thread + bounded queue.  Concurrent
 *           producers (gpu_text_puts handler today; panic path /
 *           log_forwarder later) serialise naturally on the queue;
 *           the cursor lives inside the vga module and is touched
 *           by exactly one thread, so no race.
 *
 * The queue is implemented as a fixed array of fixed-size chunks.
 * Static allocation keeps it out of the malloc fast-path (which
 * doesn't really exist in libsa_mach yet) and bounds the worst-case
 * drop budget at compile time.  At gpu_text_render_drops() the
 * caller can read how many chunks were lost since boot.
 */

#include <mach.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include <mach/gpu_server_types.h>		/* GPU_BUF_MAX */
#include "gpu_server.h"

/* ============================================================
 * Queue layout
 *
 * Depth picked to absorb a typical kernel boot dump (~5 KB ≈ two
 * chunks at GPU_BUF_MAX) plus headroom for verbose runtime logging.
 * Bumping this trades worst-case drop tolerance for static memory:
 * 16 × 4 KB = 64 KB BSS, acceptable for 0.1.0.
 * ============================================================ */

#define TEXT_QUEUE_DEPTH	16u

struct text_chunk {
	size_t	len;
	char	data[GPU_BUF_MAX];
};

static struct text_chunk text_queue[TEXT_QUEUE_DEPTH];
static unsigned int	q_head;	/* producer cursor (writes) */
static unsigned int	q_tail;	/* consumer cursor (reads)  */
static unsigned int	q_drops;	/* total chunks lost since boot */
static pthread_mutex_t	q_mutex;
static pthread_cond_t	q_not_empty;
static int		q_initialised;

/* ============================================================
 * Worker thread
 *
 * Detached pthread; the loop is intentionally trivial — every
 * smarter thing (priority, batching, vsync alignment) belongs to a
 * later milestone where the data actually justifies the complexity.
 * ============================================================ */

static void *
text_render_worker(void *arg)
{
	(void)arg;

	for (;;) {
		struct text_chunk local;

		pthread_mutex_lock(&q_mutex);
		while (q_head == q_tail)
			pthread_cond_wait(&q_not_empty, &q_mutex);

		local = text_queue[q_tail % TEXT_QUEUE_DEPTH];
		q_tail++;
		pthread_mutex_unlock(&q_mutex);

		/* Off the lock for the actual paint — VGA writes are
		 * 80 ns/byte on KVM (~325 µs for a full 80×25 redraw)
		 * and 150 ns/byte on real hardware.  Holding the queue
		 * mutex across those would needlessly serialise
		 * enqueue with VRAM I/O. */
		gpu_core_text_puts(local.data, local.len);
	}

	/* unreachable */
	return NULL;
}

/* ============================================================
 * Public API
 * ============================================================ */

int
gpu_text_render_init(void)
{
	pthread_t	tid;
	pthread_attr_t	attr;
	int		rc;

	if (q_initialised)
		return 0;

	rc = pthread_mutex_init(&q_mutex, NULL);
	if (rc != 0) {
		printf("text_render: mutex_init failed (rc=%d)\n", rc);
		return -1;
	}
	rc = pthread_cond_init(&q_not_empty, NULL);
	if (rc != 0) {
		printf("text_render: cond_init failed (rc=%d)\n", rc);
		return -1;
	}

	q_head = 0;
	q_tail = 0;
	q_drops = 0;
	q_initialised = 1;

	rc = pthread_attr_init(&attr);
	if (rc == 0) {
		(void)pthread_attr_setdetachstate(&attr,
				PTHREAD_CREATE_DETACHED);
	}

	rc = pthread_create(&tid, (rc == 0) ? &attr : NULL,
			    text_render_worker, NULL);
	(void)pthread_attr_destroy(&attr);

	if (rc != 0) {
		printf("text_render: pthread_create failed (rc=%d)\n", rc);
		q_initialised = 0;
		return -1;
	}

	printf("text_render: worker thread started "
	       "(queue depth=%u, chunk=%u bytes)\n",
	       TEXT_QUEUE_DEPTH, (unsigned)GPU_BUF_MAX);
	return 0;
}

void
gpu_text_render_enqueue(const char *buf, size_t len)
{
	struct text_chunk *slot;
	size_t copy;

	if (buf == NULL || len == 0)
		return;

	if (!q_initialised) {
		/* Render thread not up yet — fall through to direct
		 * paint so early-boot output (gpu_server's own
		 * startup printf) still reaches the screen.  This path
		 * runs single-threaded (we are pre-init), no race. */
		gpu_core_text_puts(buf, len);
		return;
	}

	copy = (len > GPU_BUF_MAX) ? GPU_BUF_MAX : len;

	pthread_mutex_lock(&q_mutex);
	if (q_head - q_tail >= TEXT_QUEUE_DEPTH) {
		q_drops++;
		pthread_mutex_unlock(&q_mutex);
		return;
	}

	slot = &text_queue[q_head % TEXT_QUEUE_DEPTH];
	slot->len = copy;
	memcpy(slot->data, buf, copy);
	q_head++;

	pthread_cond_signal(&q_not_empty);
	pthread_mutex_unlock(&q_mutex);
}

unsigned int
gpu_text_render_drops(void)
{
	unsigned int v;

	if (!q_initialised)
		return 0;

	pthread_mutex_lock(&q_mutex);
	v = q_drops;
	pthread_mutex_unlock(&q_mutex);
	return v;
}
