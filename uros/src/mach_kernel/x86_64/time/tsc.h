/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The timestamp counter, and how fast it actually runs (#409).
 *
 * Until this file the x86-64 kernel could say whether something happened and
 * never how long it took.  Every proof so far has been a conjunction of
 * facts; none of them was a duration, because there was nothing to measure
 * one with.
 *
 * ⚠️ **A measured frequency under emulation is not a frequency.**  QEMU's
 * notion of how fast the 8254 counts and how fast the timestamp counter
 * advances are two separate fictions, and their ratio is not the machine's.
 * What the boot-time check below establishes is that the *mechanism* works —
 * the counter advances, the ruler counts down, the arithmetic is right, and
 * two independent measurements agree with each other.  What the number
 * *means* is a bare-metal question, and the project already knows that
 * (bare metal is the truth; emulators lie about time and are faithful about
 * control flow).  Saying which of the two a green line proves is the
 * difference between a measurement and a decoration.
 */

#ifndef _X86_64_TIME_TSC_H_
#define _X86_64_TIME_TSC_H_

#include <stdint.h>

#include <time/exact.h>

/*
 * Read the counter.
 *
 * RDTSC is not ordered against anything: the processor may execute it before
 * instructions written above it and after instructions written below.  For
 * calibration that is harmless — the error is a handful of cycles against an
 * interval of tens of milliseconds — and for timing a short region it is
 * not, which is why the fenced form exists separately rather than this one
 * quietly becoming slower for every caller.
 */
static inline uint64_t rdtsc(void)
{
	uint32_t lo, hi;

	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

/*
 * The same reading, with the instructions before it complete.
 *
 * LFENCE is what serialises RDTSC on both vendors now; the older recipe used
 * CPUID, which is far more expensive and clobbers four registers.  Use this
 * one to time a region, and the bare one to sample a clock.
 */
static inline uint64_t rdtsc_ordered(void)
{
	__asm__ volatile("lfence" ::: "memory");
	return rdtsc();
}

/*
 * Whether the counter runs at a constant rate regardless of what the core is
 * doing about frequency and sleep states.
 *
 * This matters more than it sounds.  An early timestamp counter incremented
 * once per core clock, so it slowed down when the core did and stopped in
 * deep sleep — which makes it a cycle counter and not a clock.  The
 * invariant form is a clock, and a kernel that used the non-invariant one
 * for elapsed time would report intervals that shrink under load.
 *
 * ⚠️ The answer being no is not fatal here — calibration still works, and
 * the counter is still fine for counting cycles in a tight region — but it
 * means the counter must not become the timebase.  #318 is where that
 * decision lives.
 */
int tsc_is_invariant(void);

/*
 * Measure the counter against the 8254 and remember the answer.
 *
 * Three runs, each from an edge of the 8254 read back to a later edge, and
 * the median, believed when another run agrees with it; up to four attempts
 * (#508, tsc.c says why each number is what it is).  A single measurement
 * cannot tell a frequency from an accident -- an interrupt inside the
 * interval, an emulator scheduling the host away -- and two can only
 * disagree; three can say which one is wrong.
 *
 * Returns zero if no attempt produced a median another run agreed with, in
 * which case tsc_hz() stays zero rather than holding a number nobody should
 * use.
 */
int tsc_calibrate(void);

/*
 * The measured rate, or zero if calibration has not run or did not agree.
 *
 * Zero rather than a plausible default, because a default here is a wrong
 * answer that propagates silently into every interval the kernel reports.
 */
uint64_t tsc_hz(void);

/*
 * The runs of the last attempt, for reporting: each one's rate in hertz
 * (zero if it was set aside before the vote) and how far its two ends could
 * be, in parts per million.  Kept so the check can show what it compared
 * rather than only its verdict: a spread is the interesting part of a
 * calibration, and a pass/fail hides exactly the number worth seeing.
 */
uint64_t tsc_hz_run(unsigned which);
uint64_t tsc_window_ppm(unsigned which);
unsigned tsc_calibrate_runs(void);

/* Which attempt produced the answer, or the last one if none did. */
unsigned tsc_calibrate_attempts(void);

/* The run the median did not agree with, or -1. */
int tsc_set_aside(void);

/*
 * Where the rate came from (#508, phase 3): the exact sources the machine
 * offers, each checked against what the rulers measured, and the one adopted
 * -- by the rule time/exact.c states.
 */
const struct exact_choice *tsc_source(void);

/*
 * The refinement (#508, phase 4): a kernel thread that measures the TSC
 * against a ruler that lasts a second -- the HPET or the PM timer -- from two
 * points taken a second apart, asleep in between, and adopts the answer if it
 * agrees with the boot value.  Started once the scheduler runs.
 */
void tsc_refine_start(void);
void tsc_refined(uint64_t rate);	/* for the refinement only */

#endif	/* _X86_64_TIME_TSC_H_ */
