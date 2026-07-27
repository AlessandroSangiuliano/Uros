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
