/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The TSC, watched while the system runs (#594).
 *
 * #508 checks the TSC twice: at boot, where the rulers vote, and once more a
 * second after the scheduler starts.  After that nothing looked.  This is the
 * vote run again, every second, for as long as the system runs: the TSC's
 * rate as each ruler that lasts a second measures it -- the HPET and the PM
 * timer, never the 8254, whose sixteen bits wrap every 54.9 ms -- compared
 * with the rate the TSC was given.
 *
 * ── THREE CLOCKS, SO THAT ONE CAN BE NAMED ────────────────────────────────
 *
 * Two clocks that disagree cannot say which of them is wrong.  Linux's own
 * watchdog compares the TSC with one reference and believes the reference;
 * on the development host it marks the TSC unstable in 82 boots of 84.  Here
 * the TSC and two rulers vote, window by window:
 *
 *   both rulers put the TSC outside the bound, and agree with each other
 *        -> the TSC is the one that moved;
 *   one ruler puts it outside, the other inside, and the rulers disagree
 *        -> that ruler is the one that moved -- the HPET that Linux turns
 *           off in PC10 on omen is exactly this case, if it happens here;
 *   anything else outside the bound -> no majority, and nothing is named.
 *
 * With one ruler left the disagreement is reported and nobody is named.
 *
 * ── THE BOUND, AND WHY AN NTP SLEW STAYS INSIDE IT ────────────────────────
 *
 * The same bound the exact sources are held to (time/exact.c): half the
 * widest bracket of the boot measurement plus 1000 ppm -- plus, here, the
 * window's own brackets.  The 1000 is two 500s, and they cover the two things
 * that move the rulers against a TSC that is right:
 *
 *   - when the rate was ADOPTED from an exact source, the rulers may be off by
 *     their own accuracy (500 ppm, IA-PC HPET 1.0a 2.4.1) and, under a
 *     hypervisor, by the host's NTP correction of the moment (Linux's
 *     MAXFREQ, 500 ppm);
 *   - when the rate was MEASURED against the rulers, their own error cancels,
 *     and what is left is how far the host's NTP correction has moved since
 *     -- from one end of MAXFREQ to the other at worst, 1000 ppm.
 *
 * #508 measured what that looks like: a host converging after a reboot moved
 * the rulers 136 ppm in four minutes, a settled one sat 8.5 ppm off.  A
 * broken TSC is not that: a counter that stops, or follows the core's clock,
 * is off by percents.
 *
 * ── THREE WINDOWS IN A ROW ────────────────────────────────────────────────
 *
 * A clock is named after three windows in a row that name it, because what
 * follows is not a line in a log: when the TSC is named the tick leaves it
 * (clock_event_leave_tsc()) and its rate is withdrawn (tsc_distrust()), and a
 * ruler named is never used again.  One window can be one accident the
 * brackets did not see -- a host that suspends counts nothing on its
 * monotonic clock while the guest's TSC may jump.  A window whose brackets
 * are wider than the bound itself, or long enough for a ruler to wrap, is
 * set aside rather than judged, and does not break a run.
 *
 * ── WHAT IS SAID ──────────────────────────────────────────────────────────
 *
 * A line when the watch starts; a summary every minute, with how far the TSC
 * has been from each ruler, signed, which is what shows an NTP slew for what
 * it is; and a WRONG line, which the harness counts, the moment a clock is
 * named or the clocks stop agreeing.
 *
 * ⚠️ Bound to the boot processor.  A window is two points, and a thread that
 * moved between them would read two processors' TSCs, whose phase nobody has
 * promised to be equal (#318) -- a rate error that is really an offset.
 */

#include <stdint.h>
#include <cpus.h>

#include <kern/thread.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/time_out.h>
#include <kern/misc_protos.h>

#include <time/clock_event.h>
#include <time/ruler.h>
#include <time/rulers.h>
#include <time/tsc.h>

#define WATCH_CONFIRM		3	/* windows in a row before naming */
#define WATCH_SUMMARY		60	/* windows between summary lines */
#define WATCH_RULERS_PPM	1000	/* between two rulers: 500 each */
#define WATCH_MAX		2	/* the HPET and the PM timer */

enum {
	SUSPECT_NONE,		/* everything inside the bound */
	SUSPECT_TSC,		/* both rulers against it, and they agree */
	SUSPECT_RULER,		/* one ruler against the other two */
	SUSPECT_PAIR,		/* one ruler left, and it disagrees */
	SUSPECT_SPLIT,		/* outside the bound, and no majority */
};

struct watched {
	unsigned		id;
	const char		*name;
	struct ruler		r;	/* a copy: an ablation may replace read */
	struct ruler_point	prev;
	int			live;	/* not named */
	int			moved;	/* this window */
	uint64_t		hz;
	int64_t			dev_ppm;
	uint64_t		win_ppm;
	int64_t			dev_min, dev_max, dev_sum;	/* per summary */
	unsigned		n;
};

static int watch_wakeup;	/* posted by nobody: the timeout ends the wait */

#if	ABLATE_594_TSC_SKEWS
/*
 * #594: from the second window on, the TSC as the watchdog reads it runs one
 * part in thirty-two fast, so both rulers put it outside the bound and the
 * path that names it -- and moves the tick off it -- runs.  Never on in a
 * kernel booted for anything else.
 */
static uint64_t skew_from;

static uint64_t subject_tsc(void)
{
	uint64_t t = rdtsc_ordered();

	if (skew_from != 0 && t > skew_from)
		t += (t - skew_from) / 32;
	return t;
}
#else
static uint64_t subject_tsc(void)
{
	return rdtsc_ordered();
}
#endif

#if	ABLATE_594_HPET_STOPS
/*
 * #594: from the second window on, the HPET as the watchdog reads it stops
 * counting -- the shape of an HPET that stops in a deep package C-state --
 * so the path that names a ruler runs.  Never on otherwise.
 */
static uint64_t hpet_frozen;

static uint64_t frozen_read(void)
{
	return hpet_frozen;
}
#endif

static void watch_sleep(void)
{
	assert_wait((event_t) &watch_wakeup, FALSE);
	thread_set_timeout(hz);
	thread_block((void (*)(void)) 0);
	reset_timeout_check(&current_thread()->timer);
}

/*
 * After the TSC is named: is there still a tick, on every processor that had
 * one?  A second asleep -- which needs this processor's tick to end at all --
 * timed by a ruler rather than by the counter just withdrawn, and every
 * processor's tick count before and after.  A processor that ticked before and
 * took none since has no clock, and that is WRONG: a move that left one
 * processor behind is worse than no move.
 */
static void tick_after_move(void)
{
	unsigned long		before[NCPUS], got, lo = ~0UL, hi = 0;
	unsigned		cpu, ticking = 0, silent = 0, first = NCPUS;
	int			id = rulers_long();
	struct kernel_ruler	*k;
	uint64_t		r0, r1, ms;

	if (id < 0) {
		printf("UrMach x86-64: the tick after the TSC was named: NOT "
		       "ASKED — no ruler left to time it by (#594)\n");
		return;
	}
	k = rulers_get((unsigned) id);
	for (cpu = 0; cpu < NCPUS; cpu++)
		before[cpu] = clock_event_ticks(cpu);
	r0 = k->r.read();
	watch_sleep();
	r1 = k->r.read();
	ms = ((r1 - r0) & k->r.mask) * 1000 / k->r.hz;

	for (cpu = 0; cpu < NCPUS; cpu++) {
		if (before[cpu] == 0)
			continue;	/* never ticked: not a processor here */
		ticking++;
		got = clock_event_ticks(cpu) - before[cpu];
		if (got == 0) {
			if (silent++ == 0)
				first = cpu;
			continue;
		}
		if (got < lo)
			lo = got;
		if (got > hi)
			hi = got;
	}

	if (silent != 0)
		printf("UrMach x86-64: the tick after the TSC was named — WRONG: "
		       "processor %u%s took no tick in %lu ms by the %s: it has "
		       "no clock (#594)\n", first,
		       silent > 1 ? " and others" : "", ms, k->name);
	else
		printf("UrMach x86-64: the tick after the TSC was named, on the "
		       "%s: %u processor%s took %lu..%lu ticks each in %lu ms by "
		       "the %s, at %u Hz (#594)\n", clock_event_name(), ticking,
		       ticking == 1 ? "" : "s", lo, hi, ms, k->name,
		       clock_event_hz());
}

static uint64_t ppm_apart(uint64_t a, uint64_t b)
{
	uint64_t spread = a > b ? a - b : b - a;

	return b ? spread * 1000000 / b : 0;
}

/*
 * One window of one ruler: 1 if it can be judged.  Not judged: a ruler that
 * would have wrapped (the sleep lasted too long -- a host that stopped the
 * guest), or brackets wider than the bound, which could decide the verdict
 * by themselves.  A ruler that did not move at all IS judged: that is what a
 * stopped ruler looks like.
 */
static int watch_window(struct watched *w, uint64_t rate, uint64_t bound)
{
	struct ruler_point	cur;
	struct ruler_run	run;
	uint64_t		elapsed, counts;

	ruler_point(&w->r, subject_tsc, &cur);
	elapsed = cur.subject - w->prev.subject;
	counts = elapsed / rate * w->r.hz + (elapsed % rate) * w->r.hz / rate;

	w->moved = ruler_rate(&w->r, ~0ULL, &w->prev, &cur, &run);
	w->prev = cur;
	if (counts >= w->r.mask - w->r.mask / 16)
		return 0;
	if (!w->moved) {
		w->hz = 0;
		w->win_ppm = 0;
		w->dev_ppm = 0;
		return 1;
	}
	w->hz = run.hz;
	w->win_ppm = ruler_window_ppm(&run);
	w->dev_ppm = ((int64_t) run.hz - (int64_t) rate) * 1000000
		     / (int64_t) rate;
	return w->win_ppm <= bound;
}

static int outside(const struct watched *w, uint64_t bound)
{
	uint64_t d = w->dev_ppm < 0 ? (uint64_t) -w->dev_ppm
				    : (uint64_t) w->dev_ppm;

	return !w->moved || d > bound + w->win_ppm;
}

/*
 * One printf per line, whatever the line holds (#578): two calls can be
 * separated by another processor's output.
 */
static void summary(struct watched *w, unsigned n, unsigned windows,
		    unsigned aside, uint64_t bound)
{
	struct watched	*a = 0, *b = 0;
	unsigned	i;

	for (i = 0; i < n; i++)
		if (w[i].live && w[i].n != 0) {
			if (a == 0)
				a = &w[i];
			else
				b = &w[i];
		}

	if (b != 0)
		printf("UrMach x86-64: the TSC watchdog, %u windows: from the "
		       "%s %ld..%ld ppm (mean %ld), from the %s %ld..%ld ppm "
		       "(mean %ld); the bound %lu ppm, %u set aside (#594)\n",
		       windows, a->name, (long) a->dev_min, (long) a->dev_max,
		       (long) (a->dev_sum / (int64_t) a->n), b->name,
		       (long) b->dev_min, (long) b->dev_max,
		       (long) (b->dev_sum / (int64_t) b->n), bound, aside);
	else if (a != 0)
		printf("UrMach x86-64: the TSC watchdog, %u windows: from the "
		       "%s %ld..%ld ppm (mean %ld); the bound %lu ppm, %u set "
		       "aside (#594)\n", windows, a->name, (long) a->dev_min,
		       (long) a->dev_max, (long) (a->dev_sum / (int64_t) a->n),
		       bound, aside);
	else
		printf("UrMach x86-64: the TSC watchdog, %u windows: none "
		       "judged; %u set aside (#594)\n", windows, aside);

	for (i = 0; i < n; i++) {
		w[i].n = 0;
		w[i].dev_sum = 0;
	}
}

void tsc_watch(void)
{
	static const unsigned	ids[WATCH_MAX] = { RULER_HPET, RULER_PM };
	struct watched		w[WATCH_MAX];
	unsigned		n = 0, live, i, windows = 0, aside = 0;
	unsigned		streak = 0, since = 0;
	int			last = SUSPECT_NONE, last_who = -1;
	uint64_t		rate = tsc_hz();
	uint64_t		bound = tsc_source()->bound_ppm;

	if (rate == 0) {
		printf("UrMach x86-64: the TSC watchdog: NOT ASKED — the TSC "
		       "has no rate to watch (#594, #586)\n");
		return;
	}
	for (i = 0; i < WATCH_MAX; i++) {
		struct kernel_ruler *k = rulers_get(ids[i]);

		if (!k->present || k->tsc.hz == 0 || k->dissents)
			continue;
		w[n] = (struct watched){ .id = ids[i], .name = k->name,
					 .r = k->r, .live = 1 };
		n++;
	}
	if (n == 0) {
		printf("UrMach x86-64: the TSC watchdog: NOT ASKED — no ruler "
		       "that lasts a second answered the vote (#594)\n");
		return;
	}

#if	NCPUS > 1
	thread_bind(current_thread(), master_processor);
	thread_block((void (*)(void)) 0);	/* runs next where it is bound */
#endif

	printf("UrMach x86-64: the TSC watchdog: every second, the TSC at %lu "
	       "kHz against %s%s%s; a clock is named after %u windows in a "
	       "row, beyond %lu ppm plus the window's brackets (#594)\n",
	       rate / 1000, w[0].name, n > 1 ? " and " : "",
	       n > 1 ? w[1].name : "", WATCH_CONFIRM, bound);

	for (i = 0; i < n; i++)
		ruler_point(&w[i].r, subject_tsc, &w[i].prev);

	for (;;) {
		int	judged = 1, suspect = SUSPECT_NONE;
		int	who = -1;

		watch_sleep();
		windows++;
		since++;

#if	ABLATE_594_TSC_SKEWS
		if (windows == 2)
			skew_from = rdtsc_ordered();
#endif
#if	ABLATE_594_HPET_STOPS
		if (windows == 2)
			for (i = 0; i < n; i++)
				if (w[i].id == RULER_HPET) {
					hpet_frozen = w[i].r.read();
					w[i].r.read = frozen_read;
				}
#endif

		live = 0;
		for (i = 0; i < n; i++) {
			if (!w[i].live)
				continue;
			live++;
			judged &= watch_window(&w[i], rate, bound);
		}
		if (live == 0)
			return;		/* only a ruler can be named, and one is left */
		if (!judged) {
			aside++;
			continue;
		}

		for (i = 0; i < n; i++) {
			if (!w[i].live || !w[i].moved)
				continue;
			if (w[i].n == 0 || w[i].dev_ppm < w[i].dev_min)
				w[i].dev_min = w[i].dev_ppm;
			if (w[i].n == 0 || w[i].dev_ppm > w[i].dev_max)
				w[i].dev_max = w[i].dev_ppm;
			w[i].dev_sum += w[i].dev_ppm;
			w[i].n++;
		}

		if (live == 1) {
			for (i = 0; !w[i].live; i++)
				;
			if (outside(&w[i], bound)) {
				suspect = SUSPECT_PAIR;
				who = (int) i;
			}
		} else {
			int	off0 = outside(&w[0], bound);
			int	off1 = outside(&w[1], bound);
			int	agree = w[0].moved && w[1].moved
				&& ppm_apart(w[0].hz, w[1].hz)
				   <= (w[0].win_ppm + w[1].win_ppm) / 2
				      + WATCH_RULERS_PPM;

			if (off0 && off1 && agree)
				suspect = SUSPECT_TSC;
			else if (off0 != off1 && !agree) {
				suspect = SUSPECT_RULER;
				who = off0 ? 0 : 1;
			} else if (off0 || off1)
				suspect = SUSPECT_SPLIT;
		}

		/*
		 * The same suspect, window after window, is a run; the run's
		 * WATCH_CONFIRM-th window is where it is said, once.  A window
		 * set aside above never reaches here, so it neither extends a
		 * run nor breaks one.
		 */
		streak = suspect != SUSPECT_NONE && suspect == last
			 && who == last_who ? streak + 1 : 1;
		last = suspect;
		last_who = who;

		if (suspect != SUSPECT_NONE && streak == WATCH_CONFIRM) {
			if (suspect == SUSPECT_TSC) {
				int tick = clock_event_leave_tsc();

				/*
				 * The rate is withdrawn only when the tick no
				 * longer needs it.  Left on the TSC, the tick
				 * re-arms from tsc_hz(), and zero there is a
				 * processor that never ticks again.
				 */
				if (tick != CLOCK_EVENT_NOWHERE_TO_GO)
					tsc_distrust();
				printf("UrMach x86-64: the TSC watchdog — WRONG: "
				       "the TSC ran %ld ppm from its rate by the "
				       "%s and %ld by the %s, which agree with "
				       "each other, for %u windows in a row; "
				       "%s (#594)\n", (long) w[0].dev_ppm,
				       w[0].name, (long) w[1].dev_ppm,
				       w[1].name, WATCH_CONFIRM,
				       tick == CLOCK_EVENT_LEFT_TSC
				       ? "it is no longer trusted: the tick "
					 "moved to the local APIC timer, and "
					 "the clock no longer interpolates with "
					 "it"
				       : tick == CLOCK_EVENT_NOT_ON_TSC
				       ? "it is no longer trusted: the tick "
					 "was not on it, and the clock no "
					 "longer interpolates with it"
				       : "and the tick STAYS on it: the local "
					 "APIC timer has no rate, and there is "
					 "no third backend (#593)");
				tick_after_move();
				return;
			}
			if (suspect == SUSPECT_RULER) {
				struct watched *x = &w[who];

				x->live = 0;
				rulers_distrust(x->id);
				if (x->moved)
					printf("UrMach x86-64: the TSC watchdog "
					       "— WRONG: the %s put the TSC %ld "
					       "ppm from its rate, against the "
					       "TSC and the %s, which agree, "
					       "for %u windows in a row; it is "
					       "no longer used as a ruler "
					       "(#594)\n", x->name,
					       (long) x->dev_ppm,
					       w[1 - who].name, WATCH_CONFIRM);
				else
					printf("UrMach x86-64: the TSC watchdog "
					       "— WRONG: the %s did not move "
					       "while the TSC and the %s agreed, "
					       "for %u windows in a row; it is "
					       "no longer used as a ruler "
					       "(#594)\n", x->name,
					       w[1 - who].name, WATCH_CONFIRM);
			} else if (suspect == SUSPECT_PAIR) {
				printf("UrMach x86-64: the TSC watchdog — WRONG: "
				       "the TSC and the %s, the only ruler left, "
				       "disagree by %ld ppm for %u windows in a "
				       "row, and two clocks cannot say which "
				       "one is wrong (#594)\n", w[who].name,
				       (long) w[who].dev_ppm, WATCH_CONFIRM);
			} else {
				printf("UrMach x86-64: the TSC watchdog — WRONG: "
				       "the TSC is %ld ppm from its rate by the "
				       "%s and %ld by the %s for %u windows in a "
				       "row, and no two of the three agree: "
				       "nothing is named (#594)\n",
				       (long) w[0].dev_ppm, w[0].name,
				       (long) w[1].dev_ppm, w[1].name,
				       WATCH_CONFIRM);
			}
		}

		if (since >= WATCH_SUMMARY) {
			summary(w, n, windows, aside, bound);
			since = 0;
		}
	}
}
