/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * One way to measure a counter against a ruler (#508).
 *
 * A ruler is any counter that is read, not programmed, and whose rate is
 * known: the 8254 read back, the ACPI PM timer, the HPET.  A subject is the
 * counter whose rate is wanted: the TSC, or the LAPIC timer.  The interval
 * runs from one edge of the ruler to a later one, both seen by reading it, so
 * nothing is programmed inside the interval -- the cost of doing so was what
 * made the 8254 calibration read 0.37% fast under KVM.
 *
 * 🔑 EACH END CARRIES ITS OWN UNCERTAINTY.  An edge is seen as a change
 * between two consecutive reads, so it happened somewhere in the window from
 * the first of those reads to the second.  The subject is sampled on both
 * sides of every read, and the two windows are returned with the result: an
 * interval whose ends were interrupted is then a measured fact, not a
 * statistical guess from comparing it with another interval.
 */

#ifndef _X86_64_TIME_RULER_H_
#define _X86_64_TIME_RULER_H_

#include <stdint.h>

struct ruler {
	uint64_t	(*read)(void);	/* counts UP; a down-counter is inverted */
	uint64_t	mask;		/* its width */
	uint64_t	hz;		/* its rate, known without measuring */
};

struct ruler_run {
	uint64_t	counts;		/* ruler counts from edge to edge */
	uint64_t	subject;	/* subject counts over the same interval */
	uint64_t	window;		/* subject counts the two edges could lie in */
	uint64_t	hz;		/* the subject's rate this implies */
};

/*
 * Measure `subject' (reading up, `subject_mask' wide) over at least `span'
 * counts of `r', from an edge to an edge.  Bounded by `budget' TSC counts,
 * because the TSC is the one counter known to be running here; returns 0 if
 * the ruler did not reach the span within it.
 */
int ruler_measure(const struct ruler *r,
		  uint64_t (*subject)(void), uint64_t subject_mask,
		  uint64_t span, uint64_t budget, struct ruler_run *out);

/* The window as parts per million of the interval: how far the ends can be. */
uint64_t ruler_window_ppm(const struct ruler_run *run);

#endif	/* _X86_64_TIME_RULER_H_ */
