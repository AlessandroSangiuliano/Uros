/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The timestamp counter, and how fast it actually runs (#409).
 */

#include <stdint.h>

#include <cpu/regs.h>
#include <time/pit.h>
#include <time/tsc.h>

/*
 * How long each measurement runs.
 *
 * Long enough that the fixed costs — the port writes that start and stop the
 * ruler, the loop that polls it — are a rounding error against the interval,
 * and short enough to fit in one countdown of a sixteen-bit counter.  Two
 * runs of this are about sixty milliseconds of boot, which is worth it for a
 * number every later measurement depends on.
 */
#define CALIBRATE_US	30000u

/*
 * How far apart the two runs may be and still be believed.
 *
 * One part in sixty-four, a little under two percent.  Loose enough that a
 * system-management interrupt or an emulator losing the host processor for a
 * moment does not fail an honest calibration; tight enough that the failures
 * worth catching — a ruler that is not counting, an interval that was
 * interrupted for a long time — cannot pass.  A tolerance in the same units
 * as the thing measured, so it does not need revisiting on a faster machine.
 */
#define CALIBRATE_TOLERANCE	64

static uint64_t hz;
static uint64_t hz_run[2];

int tsc_is_invariant(void)
{
	uint32_t a, b, c, d;

	cpuid(0x80000000, &a, &b, &c, &d);
	if (a < 0x80000007)
		return 0;

	cpuid(0x80000007, &a, &b, &c, &d);
	return (d & (1U << 8)) != 0;		/* invariant TSC */
}

static uint64_t measure_once(void)
{
	uint64_t start, end;

	/*
	 * Ordered on both sides.  RDTSC may float past the instructions around
	 * it, and the two instructions it must not float past are the ones
	 * that start and stop the interval — an unordered pair could sample
	 * the counter before the gate opened and after it closed, or the
	 * reverse, and the error would be a bias rather than noise.
	 */
	start = rdtsc_ordered();
	if (!pit_delay_us(CALIBRATE_US))
		return 0;
	end = rdtsc_ordered();

	if (end <= start)
		return 0;

	return ((end - start) * 1000000u) / CALIBRATE_US;
}

int tsc_calibrate(void)
{
	uint64_t spread, allowed;

	hz = 0;
	hz_run[0] = measure_once();
	hz_run[1] = measure_once();

	if (hz_run[0] == 0 || hz_run[1] == 0)
		return 0;

	spread = hz_run[0] > hz_run[1] ? hz_run[0] - hz_run[1]
				       : hz_run[1] - hz_run[0];
	allowed = hz_run[0] / CALIBRATE_TOLERANCE;

	/*
	 * Two that agree are not a proof of accuracy — they share the same
	 * ruler, so a ruler that is wrong is wrong twice — but two that
	 * disagree are a proof that at least one is meaningless, and that is
	 * the case this can actually decide.
	 */
	if (spread > allowed)
		return 0;

	hz = (hz_run[0] + hz_run[1]) / 2;
	return 1;
}

uint64_t tsc_hz(void)
{
	return hz;
}

uint64_t tsc_hz_run(unsigned which)
{
	return which < 2 ? hz_run[which] : 0;
}
