/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What #593's two report windows share: the HPET broadcast's own
 * (hpet_event.c) and every processor's ticks (clock_event.c).  Their lengths,
 * the PM timer's total added up a step at a time, and the handing of a closed
 * window from the interrupt that closes it to the thread that prints it.
 *
 * Written once because these are the delicate parts.  Two copies had to be
 * kept alike by hand, and they already needed the same fix once: the PM
 * timer's three states, told apart in both.  A barrier or a wrap fixed in one
 * copy is a fix the other would not have (#593's review).
 */

#ifndef _X86_64_TIME_WINDOW_H_
#define _X86_64_TIME_WINDOW_H_

#include <stdint.h>

#include <sync/barrier.h>
#include <time/pmtimer.h>

/*
 * A second, then ten, then a minute: the first says early whether the rate
 * is right at all, the second is long enough to catch what a second hides,
 * and after that one a minute.
 */
static inline unsigned window_seconds(unsigned index)
{
	return index == 0 ? 1 : index == 1 ? 10 : 60;
}

/*
 * The PM timer's time over a window, added up one step at a time.  At 24
 * bits it wraps every 4.7 s, so a step longer than four seconds -- by the
 * caller's own clock, which is why the caller says so -- makes the total no
 * longer a time, and it is marked rather than printed as one.
 */
struct pm_total {
	uint32_t	last;
	uint64_t	sum;
	int		gap;
};

static inline void pm_total_open(struct pm_total *p)
{
	p->sum = 0;
	p->gap = 0;
	if (pmtimer_present())
		p->last = pmtimer_read();
}

static inline void pm_total_step(struct pm_total *p, int too_long)
{
	if (too_long)
		p->gap = 1;
	if (pmtimer_present()) {
		uint32_t pm = pmtimer_read();

		p->sum += pmtimer_delta(p->last, pm);
		p->last = pm;
	}
}

/* A time: there is a PM timer, and no step outran its wrap. */
static inline int pm_total_valid(const struct pm_total *p)
{
	return pmtimer_present() && !p->gap;
}

/*
 * A closed window handed to the thread that prints it through one flag: 0
 * free, 1 filled, 2 being printed.
 *
 * The side that closes windows fills the report only while the flag is 0,
 * then publishes it.  A window that closes while the flag is not 0 is
 * dropped, and the caller counts it.  The printing side claims a filled
 * report by compare-and-swap -- the idle loop drains on every processor,
 * and two of them finding one report would print it twice -- then reads it
 * and releases it.
 */
static inline int handoff_free(volatile int *flag)
{
	return *flag == 0;
}

static inline void handoff_publish(volatile int *flag)
{
	smp_wmb();		/* the report before the flag that says so */
	*flag = 1;
}

static inline int handoff_claim(volatile int *flag)
{
	if (*flag != 1 || !__sync_bool_compare_and_swap(flag, 1, 2))
		return 0;
	smp_rmb();		/* the flag before the report it vouches for */
	return 1;
}

/*
 * The reads of the report before the store that frees it.  TSO does not
 * move a load after a later store, so a compiler barrier is the whole of it
 * (<sync/barrier.h>) -- the ordering smp_wmb() would name is not this one.
 */
static inline void handoff_release(volatile int *flag)
{
	barrier();
	*flag = 0;
}

#endif	/* _X86_64_TIME_WINDOW_H_ */
