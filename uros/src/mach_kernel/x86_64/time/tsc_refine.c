/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The TSC's rate, measured again over a second (#508, phase 4).
 *
 * The boot calibration spends thirty milliseconds a run, because nothing can
 * sleep yet: its brackets, a few hundred ppm of that interval, are what limit
 * it.  Once the scheduler runs, a thread can take one point against a ruler,
 * sleep a second, and take another -- the same brackets divided by a second
 * instead of thirty milliseconds, and nothing spun in between.  The answer
 * replaces the boot value if it agrees with it within the tolerance the rule
 * uses everywhere; if it does not, one of the two measurements is wrong and
 * the line says so.
 *
 * ⚠️ A SLEEP, NOT A YIELD (#584).  thread_switch() with depression returns at
 * once when nothing else is runnable, and a "second" built on it measured the
 * scheduler.  assert_wait() with a timeout blocks until the tick says the
 * second is over.
 *
 * When the rate came from an exact source (tsc.c), this is a check and not a
 * refinement: the exact source stands, for the reason it was adopted -- it is
 * the same number every boot, where this measurement carries the host's NTP
 * of the moment -- and the line says how far the second's measurement is
 * from it.
 *
 * The ruler is the one rulers_long() names: the HPET or the PM timer, never
 * the 8254, whose sixteen bits wrap every 54.9 ms.  How long the sleep really
 * lasted is read off the TSC, and a sleep long enough for the ruler to have
 * wrapped is refused rather than read.
 */

#include <stdint.h>

#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/time_out.h>
#include <kern/misc_protos.h>

#include <time/ruler.h>
#include <time/rulers.h>
#include <time/tsc.h>

static int refine_wakeup;	/* an event nobody posts: the timeout ends the wait */

static uint64_t subject_tsc(void)
{
	return rdtsc_ordered();
}

static void tsc_refine_thread(void)
{
	int			id = rulers_long();
	uint64_t		boot = tsc_hz();
	struct kernel_ruler	*k;
	struct ruler_point	a, b;
	struct ruler_run	run;
	uint64_t		elapsed, spread, ppm, counts;
	int			adopted;

	if (boot == 0) {
		printf("UrMach x86-64: the TSC refined: NOT ASKED — it was not "
		       "calibrated at boot, so there is nothing to refine "
		       "(#508, #586)\n");
		thread_terminate_self();
	}
	if (id < 0) {
		printf("UrMach x86-64: the TSC refined: NOT ASKED — no ruler that "
		       "lasts a second answered the vote (#508)\n");
		thread_terminate_self();
	}
	k = rulers_get((unsigned)id);

	ruler_point(&k->r, subject_tsc, &a);
	assert_wait((event_t) &refine_wakeup, FALSE);
	thread_set_timeout(hz);
	thread_block((void (*)(void)) 0);
	reset_timeout_check(&current_thread()->timer);
	ruler_point(&k->r, subject_tsc, &b);

	/*
	 * How long it really slept, by the TSC and the boot rate, and how many
	 * counts of the ruler that is.  A difference taken within the ruler's
	 * width is right for any interval shorter than one wrap; one that comes
	 * within a sixteenth of a wrap is refused rather than trusted -- the
	 * boot rate is only good to a few hundred ppm, and the margin is what
	 * keeps that from deciding the answer.
	 */
	elapsed = b.subject - a.subject;
	counts = elapsed / boot * k->r.hz
	       + (elapsed % boot) * k->r.hz / boot;
	if (counts >= k->r.mask - k->r.mask / 16) {
		printf("UrMach x86-64: the TSC refined against the %s: NOT ASKED "
		       "— the sleep lasted %lu ms, long enough for the ruler "
		       "to wrap (#508)\n", k->name, elapsed * 1000 / boot);
		thread_terminate_self();
	}

	if (!ruler_rate(&k->r, ~0ULL, &a, &b, &run)) {
		printf("UrMach x86-64: the TSC refined against the %s — WRONG, "
		       "the ruler did not move in a second (#508)\n", k->name);
		thread_terminate_self();
	}

	spread = run.hz > boot ? run.hz - boot : boot - run.hz;
	ppm = spread * 1000000 / boot;

	if (tsc_source()->adopted >= 0) {
		printf("UrMach x86-64: the TSC checked against the %s over %lu ms: "
		       "%lu kHz, %lu ppm from %s's value, the ends within %lu ppm"
		       "%s\n", k->name, elapsed * 1000 / boot, run.hz / 1000, ppm,
		       freq_exact_name((unsigned)tsc_source()->adopted),
		       ruler_window_ppm(&run),
		       spread <= boot / RULER_TOLERANCE
		       ? " — the exact source stands"
		       : " — WRONG, more than one part in 64 from an exact "
			 "source");
		thread_terminate_self();
	}

	adopted = spread <= boot / RULER_TOLERANCE;
	if (adopted)
		tsc_refined(run.hz);

	/*
	 * One printf for the whole line: two calls can be separated by another
	 * processor's output (#578).
	 */
	printf("UrMach x86-64: the TSC refined against the %s over %lu ms: "
	       "%lu kHz, %lu ppm from the boot value, the ends within %lu ppm"
	       "%s\n", k->name, elapsed * 1000 / boot, run.hz / 1000, ppm,
	       ruler_window_ppm(&run),
	       adopted ? " — adopted"
		       : " — WRONG, more than one part in 64 from the boot "
			 "value, so one of the two measurements is wrong; the "
			 "boot value is kept");
	thread_terminate_self();
}

void tsc_refine_start(void)
{
	(void) kernel_thread(kernel_task, tsc_refine_thread, (char *) 0);
}
