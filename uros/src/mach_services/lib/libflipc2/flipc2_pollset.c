/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * flipc2_pollset.c — multi-channel wait for FLIPC v2 consumers (#122).
 *
 * A pollset owns a single Mach semaphore.  Channels added to the
 * pollset are reconfigured so producers signal the pollset's shared
 * semaphore instead of their per-channel wakeup_sem.  flipc2_poll
 * therefore waits on one semaphore and returns whichever channels
 * have ready descriptors when it wakes up.
 *
 * Constraints:
 *   - Intra-task: add channels to the pollset any time before calling
 *     flipc2_poll.
 *   - Inter-task: add channels BEFORE the remote peer attaches.  We
 *     rewrite both ch->sem_port (local handle) and hdr->wakeup_sem
 *     (which the peer reads on attach) so future attaches see the
 *     pollset's semaphore.  A peer that attached earlier still
 *     signals the original semaphore and won't drive the pollset —
 *     accepted limitation for 0.1; the typical use case (a server
 *     opening its endpoints, then entering its poll loop) sets up
 *     pollset membership at startup.
 *
 * Spurious wakeups: each producer signal increments the shared
 * semaphore.  If we wake and find no channel ready (someone signalled
 * before our cons_head advance was visible, or a producer signaled
 * a channel that was simultaneously drained), flipc2_poll just
 * returns 0 events; the caller will call again.  No DoS counter
 * here — at the pollset granularity it's the wrong scope.
 */

#include <mach.h>
#include <stdlib.h>
#include <string.h>
#include <flipc2.h>

#define FLIPC2_POLLSET_MAX_CHANNELS	16

struct flipc2_pollset_entry {
	flipc2_channel_t	ch;
	mach_port_t		orig_sem;	/* restore on remove/destroy */
};

struct flipc2_pollset {
	mach_port_t			shared_sem;
	uint32_t			n_channels;
	struct flipc2_pollset_entry	entries[FLIPC2_POLLSET_MAX_CHANNELS];
};

flipc2_return_t
flipc2_pollset_create(flipc2_pollset_t *out)
{
	struct flipc2_pollset *ps;
	kern_return_t kr;

	if (out == NULL)
		return FLIPC2_ERR_INVALID_ARGUMENT;

	ps = (struct flipc2_pollset *)malloc(sizeof(*ps));
	if (ps == NULL)
		return FLIPC2_ERR_RESOURCE_SHORTAGE;

	memset(ps, 0, sizeof(*ps));
	kr = semaphore_create(mach_task_self(), &ps->shared_sem,
			      SYNC_POLICY_FIFO, 0);
	if (kr != KERN_SUCCESS) {
		free(ps);
		return FLIPC2_ERR_KERNEL;
	}

	*out = ps;
	return FLIPC2_SUCCESS;
}

void
flipc2_pollset_destroy(flipc2_pollset_t ps)
{
	uint32_t i;

	if (ps == NULL)
		return;

	/* Restore each channel's original semaphore so it keeps working
	 * standalone after the pollset is gone. */
	for (i = 0; i < ps->n_channels; i++) {
		struct flipc2_pollset_entry *e = &ps->entries[i];
		e->ch->sem_port = e->orig_sem;
		e->ch->hdr->wakeup_sem = e->orig_sem;
	}

	if (ps->shared_sem != MACH_PORT_NULL)
		semaphore_destroy(mach_task_self(), ps->shared_sem);
	free(ps);
}

flipc2_return_t
flipc2_pollset_add(flipc2_pollset_t ps, flipc2_channel_t ch)
{
	struct flipc2_pollset_entry *e;
	uint32_t i;

	if (ps == NULL || ch == NULL)
		return FLIPC2_ERR_INVALID_ARGUMENT;
	if (ps->n_channels >= FLIPC2_POLLSET_MAX_CHANNELS)
		return FLIPC2_ERR_RESOURCE_SHORTAGE;

	/* Reject duplicate adds — caller bug, not graceful re-bind. */
	for (i = 0; i < ps->n_channels; i++) {
		if (ps->entries[i].ch == ch)
			return FLIPC2_ERR_ALREADY_CONNECTED;
	}

	e = &ps->entries[ps->n_channels++];
	e->ch       = ch;
	e->orig_sem = ch->sem_port;

	/* Redirect producer wakeups: both the local handle and the
	 * shared header.  Producers attaching after this point will
	 * read the pollset semaphore from the header. */
	ch->sem_port        = ps->shared_sem;
	ch->hdr->wakeup_sem = ps->shared_sem;
	return FLIPC2_SUCCESS;
}

flipc2_return_t
flipc2_pollset_remove(flipc2_pollset_t ps, flipc2_channel_t ch)
{
	uint32_t i;

	if (ps == NULL || ch == NULL)
		return FLIPC2_ERR_INVALID_ARGUMENT;

	for (i = 0; i < ps->n_channels; i++) {
		if (ps->entries[i].ch == ch) {
			ch->sem_port        = ps->entries[i].orig_sem;
			ch->hdr->wakeup_sem = ps->entries[i].orig_sem;
			/* Shift tail down — order isn't significant. */
			ps->entries[i] = ps->entries[ps->n_channels - 1];
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
		flipc2_channel_t ch = ps->entries[i].ch;
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
	int n;
	uint32_t i;
	kern_return_t kr;
	tvalspec_t ts;

	if (ps == NULL || events == NULL || max_events <= 0)
		return FLIPC2_ERR_INVALID_ARGUMENT;

	/* Fast path: any channel already ready? */
	n = flipc2_pollset_scan(ps, events, max_events);
	if (n > 0)
		return n;

	if (timeout_ms == 0)
		return 0;	/* non-blocking, nothing ready */

	/* Slow path: arm all channels for wakeup, re-scan to avoid
	 * losing a producer that committed between the fast scan and
	 * the cons_sleeping store. */
	for (i = 0; i < ps->n_channels; i++)
		*ps->entries[i].ch->cons_sleeping = 1;
	FLIPC2_WRITE_FENCE();

	n = flipc2_pollset_scan(ps, events, max_events);
	if (n > 0) {
		for (i = 0; i < ps->n_channels; i++)
			*ps->entries[i].ch->cons_sleeping = 0;
		return n;
	}

	if (timeout_ms == (uint32_t)-1) {
		kr = semaphore_wait(ps->shared_sem);
	} else {
		ts.tv_sec  = timeout_ms / 1000;
		ts.tv_nsec = (timeout_ms % 1000) * 1000000;
		kr = semaphore_timedwait(ps->shared_sem, ts);
	}

	for (i = 0; i < ps->n_channels; i++)
		*ps->entries[i].ch->cons_sleeping = 0;

	if (kr == KERN_OPERATION_TIMED_OUT)
		return 0;
	if (kr != KERN_SUCCESS)
		return FLIPC2_ERR_KERNEL;

	/* Final scan: who fired the doorbell? */
	return flipc2_pollset_scan(ps, events, max_events);
}
