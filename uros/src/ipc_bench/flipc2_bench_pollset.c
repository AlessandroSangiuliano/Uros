/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * flipc2_bench_pollset.c — smoke test for flipc2_poll (#122).
 *
 * Not a throughput benchmark: pollset correctness is the point.
 * Builds three intra-task channels, registers them with a pollset,
 * commits descriptors on a subset, and checks flipc2_poll reports
 * exactly the producing channels.
 *
 *   case 1 — fast path: produce on ch1 before poll, no sleep
 *   case 2 — slow path: poll with no producer, expect timeout=0
 *   case 3 — wake path: produce after the consumer arms cons_sleeping
 *            (not directly testable single-threaded; we lean on the
 *            slow-path re-scan to cover this).
 */

#include "flipc2_bench.h"
#include <flipc2.h>
#include <stdio.h>
#include <string.h>

#define POLLSET_CHANNEL_SIZE	(16 * 1024)
#define POLLSET_RING_ENTRIES	16

static int
make_channel(flipc2_channel_t *out)
{
	mach_port_t sem;
	if (flipc2_channel_create(POLLSET_CHANNEL_SIZE,
				  POLLSET_RING_ENTRIES,
				  out, &sem) != FLIPC2_SUCCESS)
		return -1;
	return 0;
}

static void
produce_one(flipc2_channel_t ch)
{
	struct flipc2_desc *d = flipc2_produce_wait(ch, 0);
	if (d == 0)
		return;
	memset(d, 0, sizeof(*d));
	d->flags = 0;
	flipc2_produce_commit(ch);
}

static void
drain_one(flipc2_channel_t ch)
{
	struct flipc2_desc *d = flipc2_consume_peek(ch);
	if (d != 0)
		flipc2_consume_release(ch);
}

void
bench_flipc2_pollset(void)
{
	flipc2_channel_t chs[3];
	flipc2_pollset_t ps = (flipc2_pollset_t)0;
	flipc2_event_t events[3];
	int i, n, pass = 1;

	printf("\n--- FLIPC2 pollset smoke test (#122) ---\n");

	for (i = 0; i < 3; i++) {
		if (make_channel(&chs[i]) < 0) {
			printf("  pollset: channel %d alloc failed\n", i);
			return;
		}
	}

	if (flipc2_pollset_create(&ps) != FLIPC2_SUCCESS) {
		printf("  pollset: create failed\n");
		return;
	}
	for (i = 0; i < 3; i++) {
		if (flipc2_pollset_add(ps, chs[i]) != FLIPC2_SUCCESS) {
			printf("  pollset: add[%d] failed\n", i);
			pass = 0;
		}
	}

	/* Case 1: produce on channel 1 only, expect exactly 1 event for ch1. */
	produce_one(chs[1]);
	n = flipc2_poll(ps, events, 3, 0);
	if (n != 1 || events[0].channel != chs[1]) {
		printf("  pollset: case1 n=%d expected 1 on ch1\n", n);
		pass = 0;
	} else {
		printf("  pollset: case1 ready=1 (ch1) OK\n");
	}
	drain_one(chs[1]);

	/* Case 2: nothing pending, non-blocking poll → 0. */
	n = flipc2_poll(ps, events, 3, 0);
	if (n != 0) {
		printf("  pollset: case2 expected 0 got %d\n", n);
		pass = 0;
	} else {
		printf("  pollset: case2 empty OK\n");
	}

	/* Case 3: produce on ch0 and ch2, blocking poll → 2 events. */
	produce_one(chs[0]);
	produce_one(chs[2]);
	n = flipc2_poll(ps, events, 3, 100 /* 100ms timeout */);
	if (n != 2) {
		printf("  pollset: case3 expected 2 got %d\n", n);
		pass = 0;
	} else {
		printf("  pollset: case3 ready=2 OK\n");
	}
	drain_one(chs[0]);
	drain_one(chs[2]);

	flipc2_pollset_destroy(ps);

	printf("  pollset: %s\n", pass ? "PASS" : "FAIL");
}
