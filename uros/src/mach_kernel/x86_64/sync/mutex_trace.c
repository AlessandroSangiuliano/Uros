/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Every transition one mutex goes through (#476).  See sync/mutex_trace.h for
 * why this exists; this is the ring and the report.
 */

#include <stdint.h>

#include <kern/lock.h>
#include <kern/thread.h>
#include <kern/cpu_number.h>
#include <kern/misc_protos.h>
#include <sync/atomic.h>
#include <time/tsc.h>
#include <sync/mutex_trace.h>

#if	MUTEX_TRACE_ON

/*
 * 256 entries, and the number is chosen against the thing being looked for
 * rather than against memory.  A lost wakeup is three or four operations by
 * two or three threads; what has to fit is everything that touched the lock
 * from the boot until the wedge, and on this target that is the handful of
 * page-wire and page-fault paths that run before userland exists.
 */
#define	MTR_ENTRIES	256

struct mtr_entry {
	uint64_t	tsc;
	void		*thread;
	uint64_t	arg;
	uint8_t		what;
	uint8_t		cpu;
	uint8_t		locked;		/* the word as this operation left it */
	int16_t		waiters;
};

static void			*mtr_target;
static struct mtr_entry		mtr_ring[MTR_ENTRIES];
static volatile uint32_t	mtr_next;
static volatile uint32_t	mtr_lost;

void
mutex_trace_watch(void *m)
{
	mtr_target = m;
}

void
mutex_trace(void *m, int what, uint64_t arg)
{
	mutex_t		*mu = (mutex_t *) m;
	struct mtr_entry *e;
	uint32_t	slot;

	if (m != mtr_target)
		return;

	/*
	 * ⚠️ An atomic claim on the slot, because this is written from every
	 * processor at once and that is the whole point: a trace of a race,
	 * recorded racily, would invent interleavings that never happened.
	 *
	 * 🔥 IT WRAPS, and the first version saturated instead.
	 *
	 * The reasoning for saturating was that a wrapping ring is overwritten
	 * by everything that happens after the wedge, so the entries that
	 * mattered would be gone.  That is true of a machine that keeps
	 * working; it is exactly backwards here.  When this stops, it STOPS --
	 * the lock is never touched again, so nothing overwrites anything, and
	 * what has to survive is the LAST few operations before the silence.
	 *
	 * The saturating version reported it itself: "256 entries, 1712
	 * dropped", and all 256 were lock-fast/unlock-quiet pairs from
	 * vm_page_bootstrap with th=0, because current_thread() does not exist
	 * yet that early.  The counter that admits what it discarded is what
	 * made the wrong choice visible.
	 */
	slot = atomic_add32(&mtr_next, 1);
	if (slot >= MTR_ENTRIES)
		atomic_add32(&mtr_lost, 1);

	e = &mtr_ring[slot % MTR_ENTRIES];
	e->tsc     = rdtsc();
	e->thread  = (void *) current_thread();
	e->arg     = arg;
	e->what    = (uint8_t) what;
	e->cpu     = (uint8_t) cpu_number();
	e->locked  = (uint8_t) mu->locked;
	e->waiters = (int16_t) mu->waiters;
}

static const char *
mtr_name(int what)
{
	switch (what) {
	case MTR_LOCK_FAST:	return "lock-fast   ";
	case MTR_ANNOUNCE:	return "announce    ";
	case MTR_ILK_FREE:	return "ilk-saw-free";
	case MTR_SLEEP:		return "sleep       ";
	case MTR_WOKE:		return "woke        ";
	case MTR_UNLOCK:	return "unlock      ";
	case MTR_WAKEUP:	return "wakeup-one  ";
	case MTR_UNLOCK_QUIET:	return "unlock-quiet";
	default:		return "?           ";
	}
}

void
mutex_trace_report(void)
{
	uint32_t	total = mtr_next;
	uint32_t	n, first, k;

	if (mtr_target == 0) {
		printf("mutex_trace: no mutex was being watched\n");
		return;
	}

	/*
	 * The last MTR_ENTRIES, oldest first.
	 *
	 * ⚠️ Oldest first and not newest first, because what is being read is a
	 * SEQUENCE: an interleaving reads backwards about as well as a sentence
	 * does.  The index printed is the absolute one, so a reader can see at
	 * a glance how much came before the window.
	 */
	n = (total < MTR_ENTRIES) ? total : MTR_ENTRIES;
	first = total - n;

	printf("mutex_trace: mutex %p, %u operations in all, showing the last %u\n",
	       mtr_target, total, n);

	for (k = 0; k < n; k++) {
		uint32_t	 abs = first + k;
		struct mtr_entry *e = &mtr_ring[abs % MTR_ENTRIES];

		printf("mutex_trace: %5u cpu%u th=%p %s locked=%d waiters=%d arg=%lu\n",
		       abs, e->cpu, e->thread, mtr_name(e->what),
		       e->locked, e->waiters, (unsigned long) e->arg);
	}
}

#endif	/* MUTEX_TRACE_ON */
