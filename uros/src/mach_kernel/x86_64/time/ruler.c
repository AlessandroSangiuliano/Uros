/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * One way to measure a counter against a ruler (#508).  See ruler.h.
 */

#include <stdint.h>

#include <time/ruler.h>
#include <time/tsc.h>

/*
 * Wait for the ruler to change, sampling the subject on both sides of every
 * read.  On return *at is the new value and [*before, *after] brackets the
 * read that returned it.
 *
 * ⚠️ THE BRACKET IS THE READ THAT SAW THE NEW VALUE, NOT THE TWO READS AROUND
 * THE CHANGE.  The first version took the window from the read before, on
 * the picture of a slow ruler whose change falls between two fast reads.
 * Under an emulator it is the other way round: one read of the HPET exits to
 * the host and takes tens of microseconds, the 100 MHz counter moves
 * thousands of counts during it, and every read sees a new value.  The value
 * returned holds at an instant INSIDE its own read, so that read is the
 * bracket; widening it to the read before only added that read's duration,
 * which varies, and the measurement got noisier by exactly that (the HPET
 * under KVM moved 460 ppm).  Waiting for a change still matters for a ruler
 * slower than a read -- the 8254 on hardware -- because the sample then
 * falls just after a transition rather than anywhere within a count.
 */
static int wait_edge(const struct ruler *r, uint64_t (*subject)(void),
		     uint64_t start, uint64_t budget,
		     uint64_t *at, uint64_t *before, uint64_t *after)
{
	uint64_t from, s0, c, s1;

	from = r->read() & r->mask;
	for (;;) {
		s0 = subject();
		c = r->read() & r->mask;
		s1 = subject();
		if (c != from) {
			*at = c;
			*before = s0;
			*after = s1;
			return 1;
		}
		if (rdtsc() - start > budget)
			return 0;
	}
}

int ruler_measure(const struct ruler *r,
		  uint64_t (*subject)(void), uint64_t subject_mask,
		  uint64_t span, uint64_t budget, struct ruler_run *out)
{
	uint64_t start = rdtsc();
	uint64_t c0, c1, b0, a0, b1, a1, first, last, target;

	*out = (struct ruler_run){ 0 };

	/* The first edge: the interval starts on one, not on a read. */
	if (!wait_edge(r, subject, start, budget, &c0, &b0, &a0))
		return 0;

	/*
	 * Then the ruler is read until it has moved at least `span', and one
	 * more edge is waited for so that the end, too, is an edge.  The
	 * reads in between see no edge that matters and are only there to
	 * notice the span.
	 */
	target = c0;
	do {
		target = r->read() & r->mask;
		if (rdtsc() - start > budget)
			return 0;
	} while (((target - c0) & r->mask) < span);

	if (!wait_edge(r, subject, start, budget, &c1, &b1, &a1))
		return 0;

	/*
	 * Each end is taken at the middle of the read that saw it, so the error
	 * at each is at most half that read, and a reading cost that is the
	 * same at both ends cancels instead of adding.
	 */
	first = b0 + ((a0 - b0) & subject_mask) / 2;
	last = b1 + ((a1 - b1) & subject_mask) / 2;

	out->counts = (c1 - c0) & r->mask;
	out->subject = (last - first) & subject_mask;
	out->window = ((a0 - b0) & subject_mask) + ((a1 - b1) & subject_mask);
	if (out->counts == 0 || out->subject == 0)
		return 0;
	out->hz = out->subject * r->hz / out->counts;
	return 1;
}

uint64_t ruler_window_ppm(const struct ruler_run *run)
{
	return run->subject ? run->window * 1000000 / run->subject : 0;
}
