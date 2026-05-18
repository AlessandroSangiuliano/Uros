/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * flipc2_bench_mpsc.c — MPSC channel smoke test (#126).
 *
 * Spawns N producer pthreads pushing M descriptors each onto a single
 * MPSC channel; the main thread consumes the union and verifies:
 *   - exactly N*M descriptors are delivered
 *   - every (producer_id, seq) pair appears exactly once
 *
 * This validates the two-counter discipline (prod_reserved + prod_tail)
 * under genuine concurrent producers.
 */

#include "flipc2_bench.h"
#include <flipc2.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define MPSC_PRODUCERS		3
#define MPSC_ITERS_PER_PROD	256
#define MPSC_TOTAL		(MPSC_PRODUCERS * MPSC_ITERS_PER_PROD)
#define MPSC_RING		64
#define MPSC_CHANNEL_SIZE	(64 * 1024)

struct mpsc_msg {
	uint32_t producer_id;
	uint32_t seq;
};

struct mpsc_arg {
	flipc2_channel_t ch;
	uint32_t producer_id;
};

static void *
mpsc_producer(void *p)
{
	struct mpsc_arg *a = (struct mpsc_arg *)p;
	uint32_t i;

	for (i = 0; i < MPSC_ITERS_PER_PROD; i++) {
		uint32_t idx;
		struct mpsc_msg m = { a->producer_id, i };
		struct flipc2_desc *d =
			flipc2_produce_wait_mpsc(a->ch, 1024, &idx);
		d->cookie = ((uint64_t)m.producer_id << 32) | m.seq;
		flipc2_produce_commit_mpsc(a->ch, idx);
	}
	return (void *)0;
}

void
bench_flipc2_mpsc(void)
{
	flipc2_channel_t ch = (flipc2_channel_t)0;
	mach_port_t sem;
	pthread_t producers[MPSC_PRODUCERS];
	struct mpsc_arg args[MPSC_PRODUCERS];
	/* Bit i in seen[p] tracks whether (p, i) was received. */
	uint8_t seen[MPSC_PRODUCERS][MPSC_ITERS_PER_PROD];
	int got = 0;
	int dup = 0;
	int ok = 1;
	int i;

	printf("\n--- FLIPC2 MPSC smoke test (#126) ---\n");

	if (flipc2_channel_create_ex(MPSC_CHANNEL_SIZE, MPSC_RING,
				     FLIPC2_CREATE_MPSC, &ch, &sem)
	    != FLIPC2_SUCCESS) {
		printf("  mpsc: channel_create_ex(MPSC) failed\n");
		return;
	}

	memset(seen, 0, sizeof(seen));

	for (i = 0; i < MPSC_PRODUCERS; i++) {
		args[i].ch          = ch;
		args[i].producer_id = (uint32_t)i;
		if (pthread_create(&producers[i], NULL, mpsc_producer,
				   &args[i]) != 0) {
			printf("  mpsc: pthread_create[%d] failed\n", i);
			flipc2_channel_destroy(ch);
			return;
		}
	}

	/* Drain MPSC_TOTAL descriptors. */
	while (got < MPSC_TOTAL) {
		struct flipc2_desc *d = flipc2_consume_wait(ch, 4096);
		struct mpsc_msg m;
		if (d == (struct flipc2_desc *)0) {
			printf("  mpsc: consume_wait returned NULL "
			       "(got=%d)\n", got);
			ok = 0;
			break;
		}
		m.producer_id = (uint32_t)(d->cookie >> 32);
		m.seq         = (uint32_t)(d->cookie & 0xFFFFFFFFu);
		if (m.producer_id >= MPSC_PRODUCERS ||
		    m.seq >= MPSC_ITERS_PER_PROD) {
			printf("  mpsc: bogus msg p=%u seq=%u\n",
			       m.producer_id, m.seq);
			ok = 0;
		} else if (seen[m.producer_id][m.seq]) {
			dup++;
		} else {
			seen[m.producer_id][m.seq] = 1;
		}
		flipc2_consume_release(ch);
		got++;
	}

	for (i = 0; i < MPSC_PRODUCERS; i++)
		pthread_join(producers[i], NULL);

	/* Coverage check. */
	{
		int missing = 0;
		int p, s;
		for (p = 0; p < MPSC_PRODUCERS; p++)
			for (s = 0; s < MPSC_ITERS_PER_PROD; s++)
				if (!seen[p][s])
					missing++;
		if (missing) {
			printf("  mpsc: %d missing (producer,seq) pairs\n",
			       missing);
			ok = 0;
		}
	}
	if (dup) {
		printf("  mpsc: %d duplicate deliveries\n", dup);
		ok = 0;
	}

	printf("  mpsc: %d producers x %d iters -> %d msgs delivered: %s\n",
	       MPSC_PRODUCERS, MPSC_ITERS_PER_PROD, got,
	       ok ? "PASS" : "FAIL");

	flipc2_channel_destroy(ch);
}
