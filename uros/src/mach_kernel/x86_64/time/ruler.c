/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * One way to measure a counter against a ruler (#508).  See ruler.h.
 */

#include <stdint.h>

#include <time/ruler.h>
#include <time/tsc.h>

#if	ABLATE_508_END_DELAYED
/*
 * #508: the end of the first run of every attempt is read with the host
 * "taking the processor away" for 2^23 TSC counts (about 3 ms) inside the
 * read, so its bracket is wider than the tolerance and the run must be set
 * aside before the vote.  Never on in a kernel booted for anything else.
 */
static int delay_this_end, end_delayed;
#endif

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
#if	ABLATE_508_END_DELAYED
		if (delay_this_end) {
			uint64_t t = rdtsc();

			delay_this_end = 0;
			while (rdtsc() - t < (1ULL << 23))
				;
		}
#endif
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

#if	ABLATE_508_END_DELAYED
	delay_this_end = end_delayed;
#endif
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

/*
 * The narrowest of three reads: a read the host interrupted has a wide
 * bracket, and one of three is enough to find a read it did not.
 */
void ruler_point(const struct ruler *r, uint64_t (*subject)(void),
		 struct ruler_point *p)
{
	uint64_t s0, c, s1;
	unsigned i;

	p->bracket = ~0ULL;
	for (i = 0; i < 3; i++) {
		s0 = subject();
		c = r->read() & r->mask;
		s1 = subject();
		if (s1 - s0 < p->bracket) {
			p->ruler = c;
			p->subject = s0 + (s1 - s0) / 2;
			p->bracket = s1 - s0;
		}
	}
}

int ruler_rate(const struct ruler *r, uint64_t subject_mask,
	       const struct ruler_point *a, const struct ruler_point *b,
	       struct ruler_run *out)
{
	*out = (struct ruler_run){ 0 };
	out->counts = (b->ruler - a->ruler) & r->mask;
	out->subject = (b->subject - a->subject) & subject_mask;
	out->window = a->bracket + b->bracket;
	if (out->counts == 0 || out->subject == 0)
		return 0;
	out->hz = out->subject * r->hz / out->counts;
	return 1;
}

/*
 * ── The rule ────────────────────────────────────────────────────────────
 *
 * ONE PART IN SIXTY-FOUR between runs, a little under two percent.  Tight
 * enough that a ruler which is not counting, or an interval interrupted for
 * a long time, cannot pass.  It was once written to be "loose enough that an
 * emulator losing the host processor for a moment does not fail an honest
 * calibration", and for the TSC it was not: over 2,573 boots ten failed on
 * it, a single run high by 49 to 212 MHz each time (#508).  The number
 * stayed; the rule around it changed.
 *
 * THREE RUNS, AND THE MEDIAN.  Two runs can only disagree; they cannot say
 * which of them is wrong.  With three, the median survives one outlier on
 * either side -- the record has low readings as well as high ones, which is
 * why it is not the minimum -- and it is believed when at least one other
 * run agrees with it.
 *
 * A RUN WHOSE ENDS COULD BE OFF BY MORE THAN THE TOLERANCE IS NOT A RUN.
 * When its two brackets together are wider than one part in sixty-four of
 * the interval, it cannot be judged right or wrong, and it is set aside
 * before the vote rather than outvoted in it.
 *
 * FOUR ATTEMPTS, because the failure being defended against is interference
 * in a window, and the answer to a transient is to measure again.  #464
 * learned it for the LAPIC timer, whose single disagreeing pair had cost one
 * boot in fifteen with a panic naming the wrong thing ("no usable timer
 * backend"); the TSC kept giving up the boot until #508.  Four failed
 * attempts is a machine that is not going to calibrate.
 */
static int agree(uint64_t a, uint64_t b)
{
	uint64_t spread = a > b ? a - b : b - a;

	return spread <= a / RULER_TOLERANCE;
}

/*
 * The median of the runs that counted, or zero; *set_aside is the run the
 * median is not and that disagreed with it, or -1.
 */
static uint64_t decide(const uint64_t run_hz[RULER_RUNS], int *set_aside)
{
	unsigned idx[RULER_RUNS], n = 0, i, j, t;
	uint64_t m;
	int agreed = 0;

	*set_aside = -1;
	for (i = 0; i < RULER_RUNS; i++)
		if (run_hz[i] != 0)
			idx[n++] = i;
	if (n < 2)
		return 0;

	for (i = 1; i < n; i++)
		for (j = i; j > 0 && run_hz[idx[j]] < run_hz[idx[j - 1]]; j--) {
			t = idx[j];
			idx[j] = idx[j - 1];
			idx[j - 1] = t;
		}

	/* Two runs have no middle; they must simply agree. */
	if (n == 2)
		return agree(run_hz[idx[0]], run_hz[idx[1]])
			? (run_hz[idx[0]] + run_hz[idx[1]]) / 2 : 0;

	m = run_hz[idx[1]];
	for (i = 0; i < n; i += 2) {
		if (agree(m, run_hz[idx[i]]))
			agreed = 1;
		else
			*set_aside = (int)idx[i];
	}
	return agreed ? m : 0;
}

int ruler_calibrate(const struct ruler *r,
		    uint64_t (*subject)(void), uint64_t subject_mask,
		    uint64_t span, uint64_t budget,
		    void (*ablate)(uint64_t run_hz[RULER_RUNS]),
		    struct ruler_calibration *out)
{
	struct ruler_run run;
	unsigned i;

	*out = (struct ruler_calibration){ .set_aside = -1 };

	for (out->attempts = 1; out->attempts <= RULER_ATTEMPTS;
	     out->attempts++) {
		for (i = 0; i < RULER_RUNS; i++) {
			out->run_hz[i] = 0;
			out->run_ppm[i] = 0;
#if	ABLATE_508_END_DELAYED
			end_delayed = (i == 0);
#endif
			/*
			 * ⚠️ NOT RETRIED, the distinction #464 drew and the
			 * retry must not blur.  A ruler that never reached the
			 * span, or a subject that never moved, is a fact about
			 * the machine and not about this window, and asking
			 * again would turn a broken counter into a slower boot
			 * -- twelve budgets of seconds each -- with the same
			 * ending.
			 */
			if (!ruler_measure(r, subject, subject_mask, span,
					   budget, &run)) {
				out->hz = 0;
				return 0;
			}
			out->run_ppm[i] = ruler_window_ppm(&run);
			if (run.window * RULER_TOLERANCE > run.subject)
				continue;	/* cannot be judged */
			out->run_hz[i] = run.hz;
		}
		if (ablate)
			ablate(out->run_hz);
		out->hz = decide(out->run_hz, &out->set_aside);
		if (out->hz != 0)
			return 1;
	}

	out->attempts = RULER_ATTEMPTS;
	return 0;
}

