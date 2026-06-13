/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * flipc2_pollset.c — multi-channel wait for FLIPC v2 consumers (#122, #325).
 *
 * A pollset waits on N channels at once and returns whichever have ready
 * descriptors when it wakes.  Since #325 it blocks with urmach_futex_waitv
 * (Linux futex_waitv style): the consumer parks on every channel's
 * `prod_tail` word, and any producer's ordinary FUTEX_WAKE (already issued
 * by produce_commit when cons_sleeping is armed, since #324) wakes it in
 * microseconds — no shared Mach doorbell semaphore, no per-pollset cross-task
 * contended state.  Before #325 the per-channel Mach semaphores were already
 * dead (producers wake via futex), so the pollset had degraded to a 10ms
 * idle-poll; waitv restores immediate wakeups, purely in userspace memory.
 *
 * Constraints:
 *   - Intra-task: add channels any time before flipc2_poll.
 *   - Inter-task: add channels BEFORE the remote peer attaches; the shared
 *     prod_tail word lives in the vm_remap'd ring, so the producer and this
 *     consumer resolve it to the same SHARED futex key automatically.
 *
 * Spurious wakeups: a producer may wake us for a channel that was drained
 * concurrently, or the lost-wakeup recheck may return early.  flipc2_poll
 * just rescans and returns 0 events; the caller calls again.
 */

#include <mach.h>
#include <stdlib.h>
#include <string.h>
#include <flipc2.h>

#define FLIPC2_POLLSET_MAX_CHANNELS	16

struct flipc2_pollset {
	uint32_t		n_channels;
	flipc2_channel_t	channels[FLIPC2_POLLSET_MAX_CHANNELS];
};

flipc2_return_t
flipc2_pollset_create(flipc2_pollset_t *out)
{
	struct flipc2_pollset *ps;

	if (out == NULL)
		return FLIPC2_ERR_INVALID_ARGUMENT;

	ps = (struct flipc2_pollset *)malloc(sizeof(*ps));
	if (ps == NULL)
		return FLIPC2_ERR_RESOURCE_SHORTAGE;

	memset(ps, 0, sizeof(*ps));
	*out = ps;
	return FLIPC2_SUCCESS;
}

void
flipc2_pollset_destroy(flipc2_pollset_t ps)
{
	if (ps == NULL)
		return;
	free(ps);
}

flipc2_return_t
flipc2_pollset_add(flipc2_pollset_t ps, flipc2_channel_t ch)
{
	uint32_t i;

	if (ps == NULL || ch == NULL)
		return FLIPC2_ERR_INVALID_ARGUMENT;
	if (ps->n_channels >= FLIPC2_POLLSET_MAX_CHANNELS)
		return FLIPC2_ERR_RESOURCE_SHORTAGE;

	/* Reject duplicate adds — caller bug, not graceful re-bind. */
	for (i = 0; i < ps->n_channels; i++) {
		if (ps->channels[i] == ch)
			return FLIPC2_ERR_ALREADY_CONNECTED;
	}

	ps->channels[ps->n_channels++] = ch;
	return FLIPC2_SUCCESS;
}

flipc2_return_t
flipc2_pollset_remove(flipc2_pollset_t ps, flipc2_channel_t ch)
{
	uint32_t i;

	if (ps == NULL || ch == NULL)
		return FLIPC2_ERR_INVALID_ARGUMENT;

	for (i = 0; i < ps->n_channels; i++) {
		if (ps->channels[i] == ch) {
			/* Shift tail down — order isn't significant. */
			ps->channels[i] = ps->channels[ps->n_channels - 1];
			ps->n_channels--;
			return FLIPC2_SUCCESS;
		}
	}
	return FLIPC2_ERR_NOT_CONNECTED;
}

/*
 * Walk channels, fill events with those that have ready descriptors.
 * Returns the number of events filled.  Helper for flipc2_poll.
 */
static int
flipc2_pollset_scan(flipc2_pollset_t ps,
		    flipc2_event_t *events, int max_events)
{
	int n = 0;
	uint32_t i;

	for (i = 0; i < ps->n_channels && n < max_events; i++) {
		flipc2_channel_t ch = ps->channels[i];
		uint32_t head = *ch->cons_head;
		uint32_t tail;

		FLIPC2_READ_FENCE();
		tail = *ch->prod_tail;
		if (tail != head) {
			events[n].channel = ch;
			n++;
		}
	}
	return n;
}

int
flipc2_poll(flipc2_pollset_t ps,
	    flipc2_event_t *events, int max_events,
	    uint32_t timeout_ms)
{
	struct urmach_futexv waiters[FLIPC2_POLLSET_MAX_CHANNELS];
	unsigned int woken_index;
	uint32_t i;
	int n;

	if (ps == NULL || events == NULL || max_events <= 0)
		return FLIPC2_ERR_INVALID_ARGUMENT;

	/* Fast path: any channel already ready? */
	n = flipc2_pollset_scan(ps, events, max_events);
	if (n > 0)
		return n;

	if (timeout_ms == 0 || ps->n_channels == 0)
		return 0;	/* non-blocking, or nothing to wait on */

	/* Slow path: arm all channels for wakeup, then re-scan so we don't
	 * lose a producer that committed between the fast scan and the
	 * cons_sleeping store. */
	for (i = 0; i < ps->n_channels; i++)
		*ps->channels[i]->cons_sleeping = 1;
	FLIPC2_WRITE_FENCE();

	n = flipc2_pollset_scan(ps, events, max_events);
	if (n > 0) {
		for (i = 0; i < ps->n_channels; i++)
			*ps->channels[i]->cons_sleeping = 0;
		return n;
	}

	/* Block on every channel's prod_tail at once.  val = the value we
	 * just saw armed (nothing ready, so prod_tail == cons_head); a
	 * producer that advances any prod_tail wakes us — and the kernel's
	 * per-word recheck catches one that advanced during arming. */
	for (i = 0; i < ps->n_channels; i++) {
		waiters[i].uaddr = (unsigned int *)ps->channels[i]->prod_tail;
		waiters[i].val   = *ps->channels[i]->prod_tail;
	}

	/* timeout_ms == (uint32_t)-1 means "forever"; the futex trap encodes
	 * forever as 0. */
	(void) urmach_futex_waitv(waiters, ps->n_channels,
				  timeout_ms == (uint32_t)-1 ? 0 : timeout_ms,
				  &woken_index);

	for (i = 0; i < ps->n_channels; i++)
		*ps->channels[i]->cons_sleeping = 0;

	/* Final scan: who fired?  (Any return — woken, timeout, or early
	 * recheck — funnels here; the caller polls again if nothing's ready.) */
	return flipc2_pollset_scan(ps, events, max_events);
}
