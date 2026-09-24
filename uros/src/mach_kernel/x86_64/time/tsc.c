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
static uint64_t tsc_rate;	/* not `hz': that is the kernel's ticks per second */
static struct tsc_source source = { .adopted = -1 };

/*
 * ── The exact sources, and which side wins (#508) ─────────────────────────
 *
 * Every source that states a rate is checked against the rulers, in the order
 * <time/freq_source.h> gives -- the processor's own statement, then the
 * hypervisor's, then the one inferred from KVM's clock -- and the first that
 * agrees with them is ADOPTED.  The reason the exact source wins over the
 * measurement it agrees with: it is the same number every boot, where the
 * measurement carries whatever the host's NTP was doing at that moment (#508
 * measured the rulers climbing 136 ppm in four minutes on a host still
 * converging, and sitting 8.5 ppm off on one that had settled).  The distance
 * is printed either way.
 *
 * AGREES WITH THE RULERS: no further from the measured value than half the
 * widest bracket behind it, plus 500 ppm for the rulers' own accuracy (IA-PC
 * HPET 1.0a 2.4.1) and 500 ppm on the other side -- under a hypervisor the
 * rulers run on the host's clock, which Linux's NTP may steer by up to
 * MAXFREQ, 500 ppm.  A source further than that CONTRADICTS the rulers, and
 * that is WRONG: one of two things that cannot both be wrong by so much is.
 *
 * TWO EXACT SOURCES CANNOT DIFFER.  Each is exact to well under a ppm (a
 * leaf in kHz, a 32-bit multiplier), so two more than 10 ppm apart are not
 * both exact, and the later one is NOT USED.  #508 found the case on a real
 * configuration, not an ablation: under QEMU's `tsc-frequency=' the timing
 * leaf said 2994000 kHz, the rulers agreed with it, and KVM's clock still
 * said the host's 2994656 -- 219 ppm, well inside the rulers' window, so only
 * the precedence caught it.
 *
 * NOTHING MEASURED, NOTHING ADOPTED.  With no ruler that answered, a source
 * cannot be checked, and it is not believed unchecked: tsc_hz() stays zero
 * and the consumers say NOT ASKED (#586).
 */
#define EXACT_AGREE_PPM		10
#define EXACT_BOUND_PPM		1000	/* 500 for the rulers, 500 for NTP */

static uint64_t ppm_apart(uint64_t a, uint64_t b)
{
	uint64_t spread = a > b ? a - b : b - a;

	return b ? spread * 1000000 / b : 0;
}

static void exact_decide(void)
{
	struct freq_exact	e;
	uint64_t		adopted_hz = 0;
	unsigned		id;

	freq_exact_read(&e);
	source = (struct tsc_source){ .adopted = -1 };
	source.measured = tsc_rate;
	source.bound_ppm = rulers_bracket_ppm() / 2 + EXACT_BOUND_PPM;

	for (id = 0; id < FREQ_EXACT; id++) {
		source.hz[id] = e.tsc_hz[id];
		if (e.tsc_hz[id] == 0) {
			source.verdict[id] = TSC_ABSENT;
			continue;
		}
		if (source.measured == 0) {
			source.verdict[id] = TSC_UNCHECKED;
			continue;
		}
		source.ppm[id] = ppm_apart(e.tsc_hz[id], source.measured);
		if (source.ppm[id] > source.bound_ppm) {
			source.verdict[id] = TSC_CONTRADICTS;
			continue;
		}
		if (source.adopted < 0) {
			source.adopted = (int)id;
			adopted_hz = e.tsc_hz[id];
			source.verdict[id] = TSC_ADOPTED;
			continue;
		}
		source.apart_ppm[id] = ppm_apart(e.tsc_hz[id], adopted_hz);
		source.verdict[id] = source.apart_ppm[id] <= EXACT_AGREE_PPM
				     ? TSC_AGREES : TSC_NOT_USED;
	}

	if (source.adopted >= 0)
		tsc_rate = adopted_hz;
}

const struct tsc_source *tsc_source(void)
{
	return &source;
}

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
	tsc_rate = v.hz;
	exact_decide();
	return tsc_rate != 0;
}

uint64_t tsc_hz(void)
{
	return tsc_rate;
}

/*
 * The refinement's answer replaces the boot value once (time/tsc_refine.c).
 * One aligned 64-bit store: a reader sees the old rate or the new one.
 */
void tsc_refined(uint64_t rate)
{
	tsc_rate = rate;
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
