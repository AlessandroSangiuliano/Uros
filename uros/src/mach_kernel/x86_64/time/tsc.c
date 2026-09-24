/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The timestamp counter, and how fast it actually runs (#409, #508).
 */

#include <stdint.h>

#include <cpu/regs.h>
#include <time/pit.h>
#include <time/ruler.h>
#include <time/tsc.h>

/*
 * How long each run lasts, in 8254 counts: thirty milliseconds.
 *
 * Long enough that reading the ruler at the two ends is a small part of the
 * interval, and short enough that the 8254, read back, does not wrap inside
 * it (65536 counts, 54.9 ms).
 */
#define CALIBRATE_SPAN		(PIT_HZ * 3u / 100u)

/*
 * How far two runs may be apart and still be believed: one part in
 * sixty-four, a little under two percent.
 *
 * What it is for, and what #508 found it was not enough for.  It is tight
 * enough that a ruler which is not counting, or an interval interrupted for
 * a long time, cannot pass.  It was written to be "loose enough that an
 * emulator losing the host processor for a moment does not fail an honest
 * calibration", and it was not: over 2,573 boots ten calibrations failed on
 * it, and in every one a single run was the high one, by 49 to 212 MHz.  The
 * tolerance stays where it was; what changed is that one disagreeing run no
 * longer decides the boot (see the rule below).
 *
 * #508 also found why run 0 was high in every TCG boot and why both runs were
 * 0.37% high under KVM: the old measurement programmed the 8254 inside its
 * own interval.  It no longer does.
 */
#define CALIBRATE_TOLERANCE	64

/*
 * How many runs, and how many times to ask (#508, the way #464 already did
 * for the LAPIC timer).
 *
 * THREE RUNS, AND THE MEDIAN.  Two runs can only disagree; they cannot say
 * which of them is wrong.  With three, the median survives one outlier on
 * either side -- and there are low readings as well as high ones in the
 * record, which is why it is not the minimum.  A run is accepted if the
 * median agrees with at least one other run within the tolerance.
 *
 * A RUN WHOSE ENDS COULD BE OFF BY MORE THAN THE TOLERANCE IS NOT A RUN.
 * Each end of a run is bracketed (time/ruler.c); when the two brackets
 * together are wider than the tolerance allows, the run cannot be judged
 * right or wrong, and it is set aside before the vote rather than outvoted
 * in it.
 *
 * FOUR ATTEMPTS OF THREE, because the failure being defended against is
 * interference in a window; four failed attempts is a machine that is not
 * going to calibrate, and tsc_hz() then stays zero and every consumer says
 * NOT ASKED (#586).
 */
#define CALIBRATE_RUNS		3
#define CALIBRATE_ATTEMPTS	4

static uint64_t hz;
static uint64_t hz_run[CALIBRATE_RUNS];
static uint64_t window_run[CALIBRATE_RUNS];
static unsigned attempts;
static int set_aside;

int tsc_is_invariant(void)
{
	uint32_t a, b, c, d;

	cpuid(0x80000000, &a, &b, &c, &d);
	if (a < 0x80000007)
		return 0;

	cpuid(0x80000007, &a, &b, &c, &d);
	return (d & (1U << 8)) != 0;		/* invariant TSC */
}

static uint64_t read_pit_up(void)
{
	return (uint16_t)~pit_ruler_read();
}

static uint64_t subject_tsc(void)
{
	return rdtsc_ordered();
}

static const struct ruler pit_ruler = { read_pit_up, 0xffff, PIT_HZ };

/*
 * One run: the TSC against the 8254 read back, edge to edge.  Zero if the
 * ruler never reached the span, or if the run's ends are too uncertain to be
 * judged; its bracket is kept either way, for the boot line.
 */
static uint64_t measure_once(unsigned i)
{
	struct ruler_run run;

	window_run[i] = 0;
	if (!ruler_measure(&pit_ruler, subject_tsc, ~0ULL, CALIBRATE_SPAN,
			   1ULL << 34, &run))
		return 0;

	window_run[i] = ruler_window_ppm(&run);
	if (run.window * CALIBRATE_TOLERANCE > run.subject)
		return 0;

	return run.hz;
}

static int agree(uint64_t a, uint64_t b)
{
	uint64_t spread = a > b ? a - b : b - a;

	return spread <= a / CALIBRATE_TOLERANCE;
}

/*
 * The rule: the median of the runs that counted, believed when at least one
 * other run agrees with it.  Returns the median or zero; *out is the index of
 * the run the median is not, and that disagrees with it -- the one set aside
 * -- or -1 if none was.
 */
static uint64_t decide(int *out)
{
	unsigned idx[CALIBRATE_RUNS], n = 0, i, j, t;
	uint64_t m;
	int agreed = 0;

	*out = -1;
	for (i = 0; i < CALIBRATE_RUNS; i++)
		if (hz_run[i] != 0)
			idx[n++] = i;
	if (n < 2)
		return 0;

	for (i = 1; i < n; i++)
		for (j = i; j > 0 && hz_run[idx[j]] < hz_run[idx[j - 1]]; j--) {
			t = idx[j];
			idx[j] = idx[j - 1];
			idx[j - 1] = t;
		}

	/* Two runs have no middle; they must simply agree. */
	if (n == 2)
		return agree(hz_run[idx[0]], hz_run[idx[1]])
			? (hz_run[idx[0]] + hz_run[idx[1]]) / 2 : 0;

	m = hz_run[idx[1]];
	for (i = 0; i < n; i += 2) {
		if (agree(m, hz_run[idx[i]]))
			agreed = 1;
		else
			*out = (int)idx[i];
	}
	return agreed ? m : 0;
}

int tsc_calibrate(void)
{
	unsigned i;

	hz = 0;
	set_aside = -1;
	pit_ruler_start();

	for (attempts = 1; attempts <= CALIBRATE_ATTEMPTS; attempts++) {
		for (i = 0; i < CALIBRATE_RUNS; i++)
			hz_run[i] = measure_once(i);
#if	ABLATE_508_ONE_RUN_OUT
		/*
		 * #508: one run reads one part in thirty-two high, the shape of
		 * every refusal the old calibration took on its own, so the
		 * median can be seen setting it aside.
		 */
		hz_run[0] += hz_run[0] / 32;
#endif
#if	ABLATE_586_TSC_DISAGREE
		/*
		 * #586, reshaped by #508: every run of every attempt reads a
		 * different amount high, one part in thirty-two apart, so no
		 * two ever agree and the calibration declines on purpose --
		 * which is how every consumer of tsc_hz() is seen answering a
		 * machine that could not calibrate.  With one run out of
		 * three, the median would simply set it aside.
		 */
		for (i = 0; i < CALIBRATE_RUNS; i++)
			hz_run[i] += hz_run[i] * i / 32;
#endif
		hz = decide(&set_aside);
		if (hz != 0)
			break;
	}

	pit_ruler_stop();
	if (hz == 0) {
		attempts = CALIBRATE_ATTEMPTS;
		return 0;
	}
	return 1;
}

uint64_t tsc_hz(void)
{
	return hz;
}

uint64_t tsc_hz_run(unsigned which)
{
	return which < CALIBRATE_RUNS ? hz_run[which] : 0;
}

uint64_t tsc_window_ppm(unsigned which)
{
	return which < CALIBRATE_RUNS ? window_run[which] : 0;
}

unsigned tsc_calibrate_runs(void)
{
	return CALIBRATE_RUNS;
}

unsigned tsc_calibrate_attempts(void)
{
	return attempts;
}

int tsc_set_aside(void)
{
	return set_aside;
}
