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
 * Here the word carries three states, and it is all the mutex there is:
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
 * ── No interlock, no count (#753) ────────────────────────────────────
 *
 * The slow path used to be the machine-independent one: a sleeper took the
 * interlock to announce itself and fall asleep as one step
 * (mutex_lock_wait(), thread_sleep_interlock()), and a release took it to
 * read the count of sleepers and wake one.  The release took it after the
 * exchange that freed the word -- after the moment another thread could own
 * the mutex, and free the object around it.  That is #753; mutex_unlock()
 * says what it did.
 *
 * Now a sleeper queues on the event before it announces, which makes the two
 * one step without a lock, and a release wakes by the event's address, so
 * neither side needs the interlock or the count.  The word's exchange is the
 * last access a release makes.
 *
 * ⚠️ mutex_t is machine-independent (kern/lock.h) and i386_lock.S reads it at
 * fixed offsets, so the structure is not ours to shrink: `interlock',
 * `waiters' and, under MUTEX_OWNER_TRACK, `ilk_thr' stay, and nothing on
 * this target uses them.  Worth revisiting when i386 goes.
 */

#include <kern/lock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>		/* #753: clear_wait() takes the thread lock */
#include <sync/atomic.h>
#include <sync/mutex.h>
#include <sync/mutex_trace.h>

/*
 * MUTEX_EVENT() is &m->locked, and it is unique to the mutex only because
 * that address is odd (sync/mutex.h).
 */
_Static_assert((__builtin_offsetof(mutex_t, locked) & 1) != 0,
	       "MUTEX_EVENT() must be an odd address: `locked' at an odd offset");

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
		MUTEX_TRACE(m, MTR_LOCK_FAST, 0);
		return;
	}

	for (;;) {
		/*
		 * Announce a waiter, and take the word if it happened to be
		 * free -- swap answers both questions at once.  Most contention
		 * ends here: the holder of a short section has usually let go.
		 *
		 * Announcing by writing 2 even when we then get the lock is
		 * deliberate: it can leave the word at 2 with nobody queued,
		 * which costs the next releaser a wakeup that finds no one.  It
		 * is also what keeps a chain of sleepers moving: whoever takes
		 * the word through here leaves 2 behind, so its own release
		 * wakes the next.
		 */
		state = atomic_swap8(&m->locked, MUTEX_WAIT);
		MUTEX_TRACE(m, MTR_ANNOUNCE, state);
		if (state == MUTEX_FREE) {
			MUTEX_NOTE_ACQUIRED(m);
			return;
		}

		/*
		 * Held by someone else.  Queue on the event, then announce
		 * AGAIN, with the same exchange: the 2 written above was
		 * written by a thread nobody could wake, and a release may
		 * already have consumed it.
		 *
		 * 🔑 QUEUE, THEN ANNOUNCE: THE ORDER IS THE PROTOCOL (#753).
		 * Once this thread is on the event's queue, a release that
		 * finds 2 wakes someone queued there.  A release before this
		 * exchange left the word without our 2, and the exchange finds
		 * it FREE; a release after it finds our 2, and we are already
		 * where its wakeup looks.  The interlock used to make "announce,
		 * then sleep" one step; declaring the sleep before the
		 * announcement makes it one step with nothing to take.
		 *
		 * 🔥 ANNOUNCE, NOT RE-READ (#558).  A read of FREE here would
		 * not be an announcement, and the exchange is what puts a
		 * queued thread behind a word that says 2.
		 *
		 * And nothing can take this thread off its processor between
		 * the two and leave it asleep unannounced: assert_wait() raises
		 * the preemption level before it writes TH_WAIT, and
		 * thread_block() or clear_wait() gives it back (#558, #599).
		 */
		assert_wait(MUTEX_EVENT(m), FALSE);
		state = atomic_swap8(&m->locked, MUTEX_WAIT);
		MUTEX_TRACE(m, MTR_ANNOUNCE, state);
		if (state == MUTEX_FREE) {
			/*
			 * Ours, so take the declared wait back.  A wakeup that
			 * already reached us was meant for somebody else, and
			 * the 2 we left will hand it on at our release.
			 */
			clear_wait(current_thread(), THREAD_AWAKENED, FALSE);
			MUTEX_NOTE_ACQUIRED(m);
			return;
		}
		MUTEX_NOTE_BLOCKING(m);
		MUTEX_TRACE(m, MTR_SLEEP, state);
		thread_block((void (*)(void)) 0);
		MUTEX_TRACE(m, MTR_WOKE, 0);
	}
}

void
mutex_unlock(mutex_t *m)
{
	uint8_t	was;

	MUTEX_NOTE_RELEASED(m);

	/*
	 * Clear and learn in one step, and touch the mutex no more.
	 *
	 * 🔥 THE EXCHANGE IS THE LAST ACCESS, AND IT HAS TO BE (#753).  From
	 * the instant the word reads FREE another thread can take it on the
	 * fast path, and a lock often decides the life of the object it sits
	 * in: task_deallocate() takes the task's lock, drops a reference and
	 * frees the task if that was the last.  This used to release the word
	 * and then go back for the interlock and the count of sleepers.  One
	 * boot's only processor was found spinning on the interlock of a task
	 * freed in between, its exchange having written 01 over the low byte
	 * of the free list's next pointer; with four processors the next
	 * zalloc() of a task meets that pointer and panics.
	 *
	 * So the wakeup is by address only.  thread_wakeup_one() looks the
	 * event up in the scheduler's own queues and never reads what it
	 * points to, and a wakeup that reaches a mutex freed and made again at
	 * the same address only sends one sleeper round its loop once more.
	 *
	 * The word alone says whether to wake.  #476 found a word of 1 hiding
	 * a sleeper whose announcement an earlier release had consumed, and
	 * kept a count beside it; #558 made a sleeper announce again at the
	 * moment it committed.  Here that moment comes after it has queued:
	 * every thread on the queue wrote 2 after queueing, and every release
	 * that consumes a 2 wakes one of them, who writes 2 again whether it
	 * then takes the word or goes back to sleep.  A word that is not 2 is
	 * owed no wakeup.
	 */
	was = atomic_swap8(&m->locked, MUTEX_FREE);
	MUTEX_TRACE(m, was == MUTEX_WAIT ? MTR_UNLOCK : MTR_UNLOCK_QUIET, was);
	if (was == MUTEX_WAIT)
		thread_wakeup_one(MUTEX_EVENT(m));
}
