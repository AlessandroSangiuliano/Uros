/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 hardware lock (#410, MD contract 5/6).
 */

#include <stdint.h>

#include <cpu/regs.h>
#include <sync/atomic.h>
#include <sync/barrier.h>
#include <sync/lock.h>

void hw_lock_init(hw_lock_t l)
{
	*l = 0;
}

unsigned int hw_lock_try(hw_lock_t l)
{
	/*
	 * One exchange, and the answer is whatever was there: zero means the
	 * lock was free and is now ours.  The exchange is a full barrier, so
	 * nothing inside the section that follows can be observed before the
	 * lock is seen taken.
	 */
	return atomic_swap8(l, 1) == 0;
}

void hw_lock_lock(hw_lock_t l)
{
	for (;;) {
		if (atomic_swap8(l, 1) == 0)
			return;

		/*
		 * Spin on a plain read, not on the exchange.
		 *
		 * Retrying the exchange would take the cache line exclusively
		 * on every attempt, so a contended lock would have its waiters
		 * pulling the line away from each other and from the holder —
		 * who needs it to release.  Reading shares the line instead,
		 * and only when it changes does anyone try again.
		 */
		while (*l != 0)
			cpu_pause();
	}
}

void hw_lock_unlock(hw_lock_t l)
{
	/*
	 * A plain store is a correct release here, and the reason is the
	 * memory model rather than luck.
	 *
	 * Everything the section did must be visible before the lock reads
	 * free.  Under x86-TSO a store is not reordered ahead of an older
	 * store, nor ahead of an older load — so every read and write inside
	 * the section is already ordered before this one, and the locked
	 * instruction that a stronger release would emit buys nothing.
	 *
	 * What is not free is the compiler, which knows none of that: the
	 * barrier stops it sinking the section's accesses past the release.
	 * On a weak-memory architecture this line becomes a release store and
	 * the comment above it becomes wrong, which is why the claim is
	 * written out rather than left as "x86 is fine".
	 */
	barrier();
	*l = 0;
}

unsigned int hw_lock_held(hw_lock_t l)
{
	/*
	 * An ordinary read, as kern/lock.h requires of this operation.  It is
	 * a question about a moment already past — by the time the caller acts
	 * on the answer the lock may have changed hands — so it is only ever
	 * sound for a holder asking about its own lock, which is how the
	 * assertion in mutex_held() uses it.
	 */
	return *l != 0;
}

#include <kern/lock.h>	/* usimple_lock_t, for the definitions below */

/*
 * ── usimple_lock: the machine's own (#453) ────────────────────────────
 *
 * <kern/lock.h> documents that a machine may supply these, and this one
 * does.  Not for speed -- the portable version in kern/lock.c is a fine spin
 * lock -- but because that file's other half is the mutex slow path and the
 * read/write locks, which reach into the scheduler.  A machine that wants
 * only a spin lock should not have to take the scheduler with it.
 *
 * They are hw_lock underneath, which is what the portable version would end
 * up calling anyway on a machine with more than one processor.
 *
 * ⚠️ No preemption counting.  The machine-independent version bumps a
 * per-processor preemption level around the hold, so that a thread cannot be
 * descheduled while holding a spin lock -- which on a spin lock is not an
 * optimisation but a correctness property: a preempted holder makes every
 * spinner wait for a scheduling quantum instead of a critical section.  This
 * target has no preemption to disable yet, and the day it does, that is what
 * belongs here rather than a comment saying it does not.
 */

void
usimple_lock_init(usimple_lock_t l, etap_event_t event)
{
	(void) event;
	hw_lock_init(&l->interlock);
}

void
usimple_lock(usimple_lock_t l)
{
	hw_lock_lock(&l->interlock);
}

void
usimple_unlock(usimple_lock_t l)
{
	hw_lock_unlock(&l->interlock);
}

unsigned int
usimple_lock_try(usimple_lock_t l)
{
	return (unsigned int) hw_lock_try(&l->interlock);
}

/*
 * interlock_unlock: the same function under a second name (#453).
 *
 * thread_sleep_interlock() in <kern/sched_prim.h> drops a hardware lock as
 * part of going to sleep, and calls it by this name.  It is handed
 * `&m->interlock' -- a hw_lock_t, at offset zero of the mutex -- so what it
 * asks for is exactly hw_lock_unlock().
 *
 * On i386 the two are separate entry points in i386_lock.S, because the mutex
 * was written in assembly there and the C side needed a way in that did not
 * perform a whole mutex unlock.  This machine's mutex is C (x86_64/sync/
 * mutex.c), so the reason is gone and only the name is left.
 *
 * An alias rather than a forwarder: it costs nothing at run time, and it says
 * the true thing -- one function, two names -- where a wrapper would suggest
 * there was a difference worth a call.
 *
 * ⚠️ The prototype in <kern/lock.h> says hw_lock_t and so does hw_lock_unlock,
 * so the alias is type-checked rather than asserted.  If either side ever
 * changes shape, this stops compiling.
 */
void	interlock_unlock(hw_lock_t) __attribute__((alias("hw_lock_unlock")));
