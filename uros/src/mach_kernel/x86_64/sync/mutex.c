/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The x86-64 kernel mutex (#452).
 *
 * Designed rather than ported.  i386's is the 1990 Mach mutex: a hardware
 * interlock guarding both the mutex word and its wait queue, taken and
 * released on every operation whether or not anyone is waiting.  That was a
 * reasonable trade when the alternative was a wider atomic nobody had.
 *
 * Here the word carries three states, and the interlock is touched only by a
 * thread that is actually about to block:
 *
 *	MUTEX_FREE   0	nobody holds it
 *	MUTEX_HELD   1	held, and no one is queued
 *	MUTEX_WAIT   2	held, and at least one thread is asleep on it
 *
 * The uncontended pair is two atomics and nothing else:
 *
 *	acquire		cmpxchg 0 -> 1
 *	release		xchg -> 0, and the answer says whether to wake
 *
 * ⚠️ The release does need its atomic, and I first wrote that it did not.
 * Reading the word and then storing zero leaves a window: a waiter can write
 * 2 between the two, and then the store erases the announcement and the
 * wakeup is lost for good.  The exchange reads and clears in one step, so
 * the answer it returns is a fact about the moment the word was cleared
 * rather than about a moment just before it.
 *
 * The win over i386 is not the count of atomics -- it is that the interlock
 * and the wait queue leave the common path entirely.  There, both acquire
 * and release take and drop the interlock *around* their work, so an
 * uncontended pair is two atomics for the interlock plus the operations it
 * guards.  Here it is two atomics, full stop, and no second cache line.
 *
 * 253 call sites in the machine-independent tree ride on that pair.
 *
 * ── Ordering ─────────────────────────────────────────────────────────
 *
 * Acquire: no access inside the critical section may be observed before the
 * word is seen taken.  The `lock cmpxchg` is a full barrier, so this costs
 * nothing extra here.
 *
 * Release: every access inside the critical section must be observed before
 * the word is seen free.  The exchange is a full barrier, so this too costs
 * nothing extra — and on a machine where the release could be a plain store,
 * x86-TSO would still order it correctly, because a store is not reordered
 * ahead of an older store or an older load.  ⚠️ That last part is a property
 * of this family, not of hardware.  Stated as what it orders rather than as
 * which instruction it emits — the discipline #410 set in <sync/barrier.h> —
 * so the claim survives the day this kernel meets a weak-memory machine.
 *
 * ── What the interlock is still for ──────────────────────────────────
 *
 * The machine-independent slow path is built on it: mutex_lock_wait() calls
 * thread_sleep_interlock(), which releases the interlock as part of going to
 * sleep.  That atomicity is what makes the sleep race-free -- a waiter that
 * released it first could sleep after the wakeup it was waiting for.
 *
 * So the interlock is kept, and kept off the fast path.  Nothing about it
 * changes; what changes is who pays for it.
 *
 * ⚠️ mutex_t is machine-independent (kern/lock.h) and i386_lock.S reads it at
 * fixed offsets, so the structure is not ours to shrink.  `interlock` is used
 * only as described above and `waiters` is maintained by the MI helpers;
 * three bytes per mutex go unused on this target.  Worth revisiting when
 * i386 goes, not worth breaking i386's assembly for now.
 */

#include <kern/lock.h>
#include <kern/sched_prim.h>
#include <sync/atomic.h>
#include <sync/mutex.h>

void
mutex_init(mutex_t *m, etap_event_t event)
{
	hw_lock_init(&m->interlock);
	m->locked  = MUTEX_FREE;
	m->waiters = 0;
#if	MACH_LDEBUG
	m->type   = MUTEX_TAG;
	m->pc     = 0;
	m->thread = 0;
#endif
	/*
	 * No ETAP hook: the trace facility is off on this target (etap.h
	 * generates ETAP 0), and ETAPCALL is local to kern/lock.c rather than
	 * a header anyone may use.  When ETAP arrives it arrives here, with
	 * its macro somewhere both files can see.
	 */
	(void) event;
}

/*
 * Try once.  Answers TRUE only if this call is what took it.
 *
 * A failure writes nothing: the word keeps whatever state its holder left,
 * and the line stays in that processor's cache instead of bouncing here to
 * be handed back.
 */
boolean_t
_mutex_try(mutex_t *m)
{
	if (atomic_cmpxchg8(&m->locked, MUTEX_FREE, MUTEX_HELD) != MUTEX_FREE)
		return FALSE;

	MUTEX_NOTE_ACQUIRED(m);
	return TRUE;
}

void
_mutex_lock(mutex_t *m)
{
	uint8_t	state;

	/*
	 * The uncontended case, and the only one most callers ever see.
	 */
	state = atomic_cmpxchg8(&m->locked, MUTEX_FREE, MUTEX_HELD);
	if (state == MUTEX_FREE) {
		MUTEX_NOTE_ACQUIRED(m);
		return;
	}

	for (;;) {
		/*
		 * Announce a waiter before sleeping, and take the word if it
		 * happened to be free -- swap answers both questions at once.
		 *
		 * Announcing by writing 2 even when we then get the lock is
		 * deliberate: it can leave the word at 2 with nobody queued,
		 * which costs the *next* releaser one trip through the slow
		 * path and a wakeup that finds no one.  The alternative is a
		 * releaser that reads 1 while a waiter is on its way to
		 * sleeping, and that one loses the wakeup for good.  A rare
		 * wasted wakeup against a possible lost one is not a close
		 * call.
		 */
		if (atomic_swap8(&m->locked, MUTEX_WAIT) == MUTEX_FREE) {
			MUTEX_NOTE_ACQUIRED(m);
			return;
		}

		/*
		 * Held by someone else and marked as having waiters.  Take
		 * the interlock and re-read under it: the holder may have
		 * released between the swap above and here, and a waiter that
		 * slept on that would not be woken by anyone.
		 */
		hw_lock_lock(&m->interlock);
		if (m->locked == MUTEX_FREE) {
			hw_lock_unlock(&m->interlock);
			continue;
		}
		MUTEX_NOTE_BLOCKING(m);
		mutex_lock_wait(m);		/* releases the interlock */
	}
}

void
mutex_unlock(mutex_t *m)
{
	MUTEX_NOTE_RELEASED(m);

	/*
	 * Clear and learn in one step.  A previous value of 1 means nobody
	 * announced themselves, so there is nothing to wake and no interlock
	 * to take -- which is the whole of the release for the common case.
	 */
	if (atomic_swap8(&m->locked, MUTEX_FREE) == MUTEX_HELD)
		return;

	/*
	 * Somebody announced.  They are either asleep, or between announcing
	 * and sleeping and holding the interlock.  Either way the interlock
	 * is what serialises us against them, and mutex_unlock_wakeup wants
	 * it held.
	 */
	hw_lock_lock(&m->interlock);
	if (m->waiters)
		mutex_unlock_wakeup(m);
	hw_lock_unlock(&m->interlock);
}
