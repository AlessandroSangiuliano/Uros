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

/*
 * A point: the ruler's value and the subject at one instant, give or take
 * half the bracket (#508, phase 4).  Two points taken far apart give a rate
 * whose error is the two brackets divided by the distance -- and the time
 * between them can be spent asleep, where ruler_measure() spins.  The ruler
 * must not wrap between the two.
 */
struct ruler_point {
	uint64_t	ruler;
	uint64_t	subject;	/* at the middle of the bracket */
	uint64_t	bracket;	/* subject counts the read took */
};

void ruler_point(const struct ruler *r, uint64_t (*subject)(void),
		 struct ruler_point *p);

/* The rate between two points, as ruler_measure() reports one; 0 if none. */
int ruler_rate(const struct ruler *r, uint64_t subject_mask,
	       const struct ruler_point *a, const struct ruler_point *b,
	       struct ruler_run *out);

/*
 * The calibration rule, one for every subject (#508): the TSC and the LAPIC
 * timer are held to it alike, which #464 asked for and which had drifted --
 * the LAPIC timer asked again on a disagreement, the TSC gave up the boot.
 * ruler.c says why each number is what it is.
 */
#define RULER_RUNS		3	/* per attempt; the median of these */
#define RULER_ATTEMPTS		4	/* then the machine is not calibrating */
#define RULER_TOLERANCE		64	/* one part in this, between runs */

struct ruler_calibration {
	uint64_t	hz;			/* the median believed, or 0 */
	uint64_t	run_hz[RULER_RUNS];	/* the last attempt's runs; 0 =
						   set aside before the vote */
	uint64_t	run_ppm[RULER_RUNS];	/* each run's bracket, in ppm */
	unsigned	attempts;		/* the one that answered, or the
						   last one tried */
	int		set_aside;		/* the run the median disagreed
						   with, or -1 */
};

/*
 * Calibrate `subject' against `r' by the rule above, each run over `span' of
 * the ruler and bounded by `budget' TSC counts.  `ablate', when not null, is
 * handed each attempt's runs before the vote: it is how an ablation build
 * makes a run disagree on purpose, and it is null in any kernel booted for
 * anything else.  Returns 1 with out->hz set, or 0 with out->hz zero.
 */
int ruler_calibrate(const struct ruler *r,
		    uint64_t (*subject)(void), uint64_t subject_mask,
		    uint64_t span, uint64_t budget,
		    void (*ablate)(uint64_t run_hz[RULER_RUNS]),
		    struct ruler_calibration *out);

#endif	/* _X86_64_TIME_RULER_H_ */
