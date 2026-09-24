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
#include <time/rulers.h>
#include <time/tsc.h>

/*
 * Measured against the 8254 read back, by the rule every calibration shares
 * (time/ruler.c: three runs, the median, four attempts).
 *
 * What #508 found here, so it is not found again.  The old measurement read
 * the TSC, then programmed the 8254, then waited for it: six port accesses
 * inside its own interval.  Under KVM BOTH runs read 0.37% fast -- what
 * 111 us of 30 ms would do; under TCG the first run paid for translating the
 * code as well, and was the high one in 207 of 207 recorded boots.  The
 * interval now runs from an edge of the ruler to an edge, with nothing
 * programmed inside it.  And two runs that disagreed gave up the boot, about
 * one in 260; now the median of three decides, and a failed attempt is asked
 * again.
 */
static uint64_t hz;

int tsc_is_invariant(void)
{
	uint32_t a, b, c, d;

	cpuid(0x80000000, &a, &b, &c, &d);
	if (a < 0x80000007)
		return 0;

	cpuid(0x80000007, &a, &b, &c, &d);
	return (d & (1U << 8)) != 0;		/* invariant TSC */
}

static uint64_t subject_tsc(void)
{
	return rdtsc_ordered();
}

#if	ABLATE_508_ONE_RUN_OUT || ABLATE_586_TSC_DISAGREE
static void ablate_runs(uint64_t run_hz[RULER_RUNS])
{
	unsigned i;

	(void)i;
#if	ABLATE_508_ONE_RUN_OUT
	/*
	 * #508: one run reads one part in thirty-two high, the shape of every
	 * refusal the old calibration took on its own, so the median can be
	 * seen setting it aside.
	 */
	run_hz[0] += run_hz[0] / 32;
#endif
#if	ABLATE_586_TSC_DISAGREE
	/*
	 * #586, reshaped by #508: every run of every attempt reads a different
	 * amount high, one part in thirty-two apart, so no two ever agree and
	 * the calibration declines on purpose -- which is how every consumer of
	 * tsc_hz() is seen answering a machine that could not calibrate.  With
	 * one run out of three, the median would simply set it aside.
	 */
	for (i = 0; i < RULER_RUNS; i++)
		run_hz[i] += run_hz[i] * i / 32;
#endif
}
#define	TSC_ABLATE	ablate_runs
#else
#define	TSC_ABLATE	0
#endif

/*
 * Against every ruler the machine has, and then they vote (time/rulers.h).
 * The ablations reach every ruler's runs alike: #586's has to leave the
 * machine with no ruler that answered, or the vote would carry on without the
 * 8254 and the NOT ASKED paths it exists to run would not run.
 */
int tsc_calibrate(void)
{
	struct rulers_verdict	v;
	struct kernel_ruler	*k;
	unsigned		id;

	rulers_find();
	for (id = 0; id < RULERS; id++) {
		k = rulers_get(id);
		if (!k->present)
			continue;
		rulers_start(id);
		(void) ruler_calibrate(&k->r, subject_tsc, ~0ULL, k->span,
				       1ULL << 34, TSC_ABLATE, &k->tsc);
		rulers_stop(id);
	}

	rulers_vote(&v);
	hz = v.hz;
	return hz != 0;
}

uint64_t tsc_hz(void)
{
	return hz;
}

uint64_t tsc_hz_run(unsigned which)
{
	return which < RULER_RUNS ? rulers_get(RULER_8254)->tsc.run_hz[which] : 0;
}

uint64_t tsc_window_ppm(unsigned which)
{
	return which < RULER_RUNS ? rulers_get(RULER_8254)->tsc.run_ppm[which] : 0;
}

unsigned tsc_calibrate_runs(void)
{
	return RULER_RUNS;
}

unsigned tsc_calibrate_attempts(void)
{
	return rulers_get(RULER_8254)->tsc.attempts;
}

int tsc_set_aside(void)
{
	return rulers_get(RULER_8254)->tsc.set_aside;
}
