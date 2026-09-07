/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 hardware lock (#410, MD contract 5/6).
 */

#include <stdint.h>

#include <kern/cpu_data.h>	/* #461: the preemption level */
#include <cpu/percpu.h>		/* #528: the interrupt-masked section */
#include <cpu/regs.h>
#include <sync/atomic.h>
#include <sync/barrier.h>
#include <sync/lock.h>

/*
 * ── Interrupts are masked for the length of the hold (#528) ───────────
 *
 * Preemption counting (#461) stops the scheduler taking a holder off its
 * processor.  It says nothing about an INTERRUPT, which lands on the holder's
 * own processor and runs a handler that may want the same lock -- and on one
 * processor the holder is the interrupted path, so nothing can ever release
 * it.  #522 met exactly that: run_queue_enqueue() holding the run queue lock,
 * the tick landing inside the section, and the tick's handler arriving back at
 * run_queue_enqueue() for the same lock.
 *
 * #522 closed it by moving the timer into a class splsched() defers.  That is
 * a fix for one interrupt; this is the fix for the mechanism, and it is what
 * every kernel that takes a spin lock from interrupt context does --
 * spin_lock_irqsave in Linux, the IPL raised inside the lock in the BSDs.
 *
 * 🔑 THE PAIRING IS STRUCTURAL.  The state to restore lives in the processor's
 * own block as a count plus the flag as it was at the outermost entry, so no
 * caller carries a token it could drop, and a lock taken inside a handler
 * leaves interrupts off when it is dropped because that is what the count
 * says.  See <cpu/percpu.h>.
 *
 * ⚠️ AND THE SPIN RUNS WITH THE CALLER'S OWN INTERRUPT STATE, not with them
 * masked.  A waiter is not a holder: it has nothing to protect, and masking
 * while it waits is what would turn this into a worse deadlock than the one it
 * closes.  ipi_call_others() takes this very lock and then stands still until
 * every other processor has answered -- so a second processor spinning here
 * with interrupts masked would never take the message the first is waiting
 * for, and the first would panic saying a processor never answered.  Masking
 * belongs to the section, and a spin is not one.
 */

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
	 *
	 * Preemption is raised before the attempt and dropped again if the
	 * attempt fails, so that the successful path leaves exactly what
	 * hw_lock_lock() leaves and the caller cannot tell the two apart (#461).
	 * Interrupts likewise (#528): a conditional acquisition that succeeded
	 * is a hold, and a hold is a masked section.
	 */
	disable_preemption();
	percpu_intr_disable();

	if (atomic_swap8(l, 1) == 0)
		return 1;

	percpu_intr_enable();
	enable_preemption_no_check();
	return 0;
}

void hw_lock_lock(hw_lock_t l)
{
	/*
	 * Preemption off for the whole hold (#461).
	 *
	 * Before the acquire, not after: between a successful exchange and the
	 * increment there is a window in which this thread holds the lock and
	 * can be taken off the processor, which is the exact failure being
	 * closed.  Raising it first costs nothing if the acquire fails -- the
	 * spin below is a fine place not to be preempted either.
	 *
	 * On a spin lock this is not an optimisation.  A preempted holder does
	 * not merely make the spinners wait a quantum: the spinners may be in
	 * interrupt context, where the gate has already cleared IF, and then no
	 * processor is left to schedule the holder at all.
	 */
	disable_preemption();

	for (;;) {
		percpu_intr_disable();

		if (atomic_swap8(l, 1) == 0)
			return;

		/*
		 * Not ours, so this processor is a waiter and not a holder —
		 * and a waiter must be able to answer (#528, see above).
		 */
		percpu_intr_enable();

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

	/*
	 * And preemption back, after the release rather than before it (#461).
	 *
	 * The other order would leave a window in which the lock is still held
	 * and this thread can be taken off the processor -- the failure the
	 * increment exists to prevent, moved to the end of the section instead
	 * of the beginning.
	 *
	 * ⚠️ No AST is taken here.  A processor that has just dropped a lock is
	 * about to reach a trap return or another preemption point of its own
	 * accord, and switching from inside the lock package would mean every
	 * unlock in the kernel is a place a thread can vanish -- including the
	 * ones called with interrupts off.
	 */
	/*
	 * Interrupts back first, then preemption: the mirror of the acquire,
	 * where preemption is the outer of the two (#528).  Whether they
	 * actually come back is the count's answer, not this function's -- a
	 * lock taken inside a handler, or inside another lock, leaves them
	 * masked, which is the whole point of counting.
	 */
	percpu_intr_enable();
	enable_preemption_no_check();
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
 * Preemption is counted, in hw_lock_lock() and hw_lock_unlock() underneath,
 * so every direct user of a hardware lock is covered and not only these.
 *
 * ⚠️ This comment used to say there was no preemption counting, because "this
 * target has no preemption to disable yet, and the day it does, that is what
 * belongs here".  #459 was that day and nobody came back here (#461).
 *
 * What made it hard to notice is that the machine-independent kernel appears
 * to do it already: kern/lock.c's usimple_lock calls disable_preemption(), and
 * printf and half the scheduler call it too.  With MACH_RT off -- which it is
 * on both of this kernel's targets -- <kern/cpu_data.h> defines that as
 * nothing and get_preemption_level() as the constant 0.  So the property was
 * asserted everywhere and held nowhere, and the bill arrived as a deadlock:
 * a thread preempted holding a spin lock, the other processors reaching for it
 * from interrupt context with IF already clear, and nothing left able to
 * schedule the holder.
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
