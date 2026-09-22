/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY.
 */
/*
 *	File:	kern/rcu.c
 *
 *	Quiescent-state-based RCU (QSBR) write side, #331 step 2.
 *	The read side and the design rationale live in <kern/rcu.h>.
 */

#include <cpus.h>
#include <mach/machine.h>		/* machine_slot[].running */
#include <mach/kern_return.h>
#include <kern/rcu.h>
#include <kern/cpu_data.h>
#include <kern/misc_protos.h>		/* printf */
#include <kern/lock.h>			/* #566: the callback queue */

/*
 *	Full memory fence (SSE2 mfence).  Used once per grace period as the
 *	publish barrier: it makes the writer's unlink store globally visible
 *	before we snapshot the quiescent counters, closing the only StoreLoad
 *	gap TSO leaves open.  No reader started after this can observe the old
 *	(about-to-be-freed) object.
 */
#define	urmach_rcu_smp_mb()	__asm__ __volatile__("mfence" : : : "memory")

/*
 *	Spin-wait hint (rep;nop).  The "memory" clobber also forces the loop to
 *	re-read machine_slot[].running and rcu_qs_seq each iteration.
 */
#define	urmach_rcu_cpu_pause()	__asm__ __volatile__("pause" : : : "memory")

/*
 * 🔴 A RAW hw_lock AND NOT A simple_lock, AND THE REASON IS WHEN IT IS FIRST
 * TAKEN.  pmap_destroy() runs during the machine-dependent boot -- pv_selftest
 * destroys a pmap -- which is long before kern/startup.c initialises anything.
 * A simple_lock used before simple_lock_init() faulted there, in this exact
 * function, and the first version of this queue took the machine down to a
 * `no handler' halt with a backtrace through urmach_call_rcu().
 *
 * hw_lock_init() sets the word to zero and nothing else, and BSS is already
 * zero, so this one is usable from the first instruction of the kernel.  That
 * is not a trick: it is the property this queue needs and simple_lock does not
 * have.
 */
static hw_lock_data_t	rcu_cb_lock;

static struct urmach_rcu_head	*rcu_cb_list;
static unsigned int		 rcu_gp;
static unsigned int		 rcu_snap[NCPUS];
static int			 rcu_active;

/*
 * 🔴 THE GRACE PERIOD AT WHICH SOMEBODY LAST DRAINED, and it is what keeps the
 * idle loop from eating the machine.
 *
 * The first version had urmach_rcu_drain() take the queue's lock on every pass
 * of the idle loop.  Four processors idling is four processors taking one
 * global lock continuously -- and hw_lock_lock() masks interrupts while it is
 * held, so the clock tick was starved on all of them, urmach_rcu_advance()
 * never ran, no grace period ever completed, nothing was ever released and the
 * idle loops went on taking the lock.  A livelock built out of a reclamation
 * that could not reclaim.  The boot reached quiet_census and then said nothing
 * for sixty seconds.
 *
 * A callback can only become ready when rcu_gp advances, so there is nothing
 * to do until it does.  One unlocked read of a counter answers that, and the
 * idle loop pays a load instead of a lock.
 */
static unsigned int		 rcu_drained_gp;

/*
 *	#331 step 2 bring-up watchdog.  A correct grace period ends within a
 *	clock tick or two; if a CPU has not reported a quiescent state after
 *	this many spins something is wrong (a reader that never unlocks, a
 *	wedged checkpoint).  Rather than hang the box silently we print a
 *	diagnostic and keep waiting -- correctness is preserved, the symptom is
 *	surfaced.  Generous enough never to fire in normal operation.
 */
#define	URMACH_RCU_STALL_SPINS	100000000U

void
urmach_rcu_init(void)
{
	/*
	 *	rcu_read_depth / rcu_qs_seq live inside cpu_data[], which is in
	 *	BSS and therefore already zero at boot -- and so is the callback
	 *	queue's hw_lock, deliberately, because pmap_destroy() reaches it
	 *	before this function has ever run (#566).
	 */
}

void
urmach_synchronize_rcu(void)
{
	unsigned int	snap[NCPUS];
	int		c;
	int		me = cpu_number();

	/*
	 * ABLATE_566_NO_GRACE makes every grace period return at once, which
	 * is UNSAFE -- a reader may still hold what the caller is about to
	 * free -- and exists to answer one question: is the wait what the
	 * #548 trap sweep pays a clock tick for on more than one processor?
	 *
	 * 🔑 HERE AND NOT AT A CALL SITE.  The first version of this ablation
	 * removed the call in pmap_destroy() alone and changed nothing, which
	 * read as "the grace period is innocent" -- wrongly: pmap_collect()
	 * takes two more per batch (x86_64/pmap/vminit.c), and the sampler
	 * was showing a processor inside THIS function a quarter of the time.
	 * An ablation of one caller answers about that caller; the question
	 * was about the wait.
	 */
#if	ABLATE_566_NO_GRACE
	return;
#endif

	/*
	 *	#336 LANDMINE, DISARMED: `me` used to be read without disabling
	 *	preemption, safe only while the kernel could not preempt in kernel
	 *	mode.  x86-64 can since #459/#463 -- an AST on the way out of any
	 *	trap -- so an involuntary preempt and migrate between here and the
	 *	wait loop would make `me` name a CPU we left; its last quiescent
	 *	state may predate the publish barrier below, so skipping it would
	 *	end the grace period while a reader there still holds the old
	 *	pointer -- a use-after-free.  The remedy this comment already named
	 *	is taken: preemption is off for the whole function, which is also
	 *	what makes the "never migrates" claim above true rather than hoped.
	 *
	 *	⚠️ It is off across a spin that can last several clock ticks.  That
	 *	is a deliberate trade and not an oversight: this processor was
	 *	going to spin here either way, and being moved off it in the middle
	 *	would not have made the wait shorter -- it would have made the
	 *	answer wrong.
	 */
	disable_preemption();

	/*
	 *	Publish the unlink before sampling: after this fence no CPU can
	 *	still load the old pointer out of the structure, so any CPU that
	 *	advances its counter past the snapshot has truly finished every
	 *	read section that could have seen it.
	 */
	urmach_rcu_smp_mb();

	for (c = 0; c < NCPUS; c++)
		snap[c] = cpu_data[c].rcu_qs_seq;

	for (c = 0; c < NCPUS; c++) {
		unsigned int spins = 0;

		/*
		 *	The caller is a writer holding no read reference, so the
		 *	current CPU is quiescent by definition -- and since this
		 *	function spins (never context-switches) the current CPU
		 *	would otherwise only ever advance via its own clock tick,
		 *	so waiting on it risks self-deadlock if interrupts are
		 *	masked here.  Skip it.  (Non-preemptive kernel: any earlier
		 *	reader on this CPU finished before the writer ran.)
		 */
		if (c == me)
			continue;

		/*
		 *	Done with CPU c once it has either reported a quiescent
		 *	state past the snapshot (ticked / context-switched) or is
		 *	currently idle (idle == quiescent; an idle CPU may be HLTed
		 *	and never tick, so we must not wait on its counter).
		 */
		while (machine_slot[c].running &&
		       !cpu_data[c].rcu_cpu_idle &&
		       cpu_data[c].rcu_qs_seq == snap[c]) {
			/*
			 *	A CPU spinning here is itself a writer holding no
			 *	read reference, i.e. quiescent -- so report our own
			 *	quiescent state while we wait.  Without this, two
			 *	CPUs that both enter synchronize_rcu (e.g. two
			 *	concurrent table grows) deadlock waiting on each
			 *	other: a spinning CPU is not idle, does not
			 *	context-switch, and may have interrupts masked so no
			 *	clock tick advances its counter.
			 */
			urmach_rcu_quiescent_state();
			urmach_rcu_cpu_pause();
			if (++spins >= URMACH_RCU_STALL_SPINS) {
				printf("urmach_rcu: WARNING cpu %d not quiescent "
				       "(qs_seq=%u snap=%u depth=%d idle=%d "
				       "running=%d), still waiting\n",
				       c, cpu_data[c].rcu_qs_seq, snap[c],
				       cpu_data[c].rcu_read_depth,
				       (int) cpu_data[c].rcu_cpu_idle,
				       (int) machine_slot[c].running);
				spins = 0;
			}
		}
	}

	enable_preemption();
}

/*
 * ── Deferred reclamation (#566) ──────────────────────────────────────────
 *
 * The state that makes a grace period observable WITHOUT blocking in one.
 *
 *	rcu_gp		how many grace periods have completed.
 *	rcu_active	one is in flight, and rcu_snap holds its snapshot.
 *
 * 🔑 WHY A CALLBACK IS STAMPED gp+2 WHILE ONE IS IN FLIGHT.  The grace period
 * running now took its snapshot BEFORE this object was unlinked, so a
 * processor may have reported quiescent already and still be holding the
 * pointer it read a moment earlier.  That grace period says nothing about this
 * object.  The next one does, because its snapshot will be taken after the
 * unlink -- so the callback waits for two completions, not one.  With none in
 * flight the next snapshot is already after the unlink and one is enough.
 *
 * ⚠️ One lock and one list, not one per processor.  This is the RARE path by
 * construction -- objects being reclaimed, not objects being read -- and a
 * per-processor queue is what to build when a measurement says this lock is
 * contended.  Nothing says that yet, and #455's own rule is that granularity
 * is decided with a number in front of it.
 */

/* How many are waiting, and how many have been handed back.  Read by whoever
 * wants to know whether the queue is draining rather than growing. */
unsigned int	urmach_rcu_queued;
unsigned int	urmach_rcu_retired;

/*
 * 🔴 NO splhigh() HERE, AND THAT COST A SECOND BOOT.  The obvious way to keep
 * the clock tick out of this section is an spl pair -- and pmap_destroy() runs
 * during pv_selftest, in the machine-dependent boot, where splx() is not usable
 * yet: the machine halted with `no handler' and a backtrace whose innermost
 * frame was inside splx().  Asked of the disassembly rather than guessed:
 * `mov 0x38(%r13)' with r13 holding BIOS garbage.
 *
 * What keeps the tick out instead is that the tick does not insist.
 * urmach_rcu_advance() TRIES the lock and returns if it is held, so there is
 * no section for an interrupt to deadlock against -- on a uniprocessor either,
 * which is the case an spl pair is usually there for.
 */
void
urmach_call_rcu(struct urmach_rcu_head *h, void (*f)(struct urmach_rcu_head *))
{
	int	c, me = cpu_number();

	h->func = f;

	/*
	 * 🔴 WITH NOBODY ELSE RUNNING THERE IS NO GRACE PERIOD TO WAIT FOR, and
	 * deferring would be worse than useless: it would leave the object
	 * queued until something drains, and during the machine-dependent boot
	 * nothing does -- there is no clock tick yet and no idle loop.  The
	 * first version deferred unconditionally and the boot ran the pool of
	 * boot pmaps dry, then faulted in pmap_enter on a space whose root had
	 * been cleared.
	 *
	 * The condition is the same one urmach_synchronize_rcu() skips on: a
	 * processor that is not running cannot be holding a read reference, and
	 * this one is the writer.  So the callback is already safe, and running
	 * it here is what synchronize_rcu() would have done in no time at all.
	 */
	for (c = 0; c < NCPUS; c++)
		if (c != me && machine_slot[c].running)
			break;
	if (c == NCPUS) {
		f(h);
		return;
	}

	hw_lock_lock(&rcu_cb_lock);
	h->gp = rcu_gp + (rcu_active ? 2 : 1);
	h->next = rcu_cb_list;
	rcu_cb_list = h;
	urmach_rcu_queued++;
	hw_lock_unlock(&rcu_cb_lock);
}

/*
 * One step, from the clock tick.  Cheap and non-blocking: with nothing queued
 * it does not even take the lock, which is the ordinary case on a machine that
 * is not destroying address spaces.
 */
void
urmach_rcu_advance(void)
{
	int	c, done = 1;
	int	me = cpu_number();

	if (rcu_cb_list == 0 && !rcu_active)
		return;

	/*
	 * Tried, not taken: this runs from the clock tick, and a tick that
	 * insisted on a lock a thread on this same processor already holds
	 * would never come back.  A step skipped is a step taken on the next
	 * tick; nothing is lost but ten milliseconds of one reclamation.
	 */
	if (!hw_lock_try(&rcu_cb_lock))
		return;

	if (!rcu_active) {
		/*
		 * Start one.  The fence is the same requirement
		 * urmach_synchronize_rcu() has: no processor may still load a
		 * pointer unlinked before this, so the snapshot must not be
		 * taken before the unlink is visible.
		 */
		urmach_rcu_smp_mb();
		for (c = 0; c < NCPUS; c++)
			rcu_snap[c] = cpu_data[c].rcu_qs_seq;
		rcu_active = 1;
		hw_lock_unlock(&rcu_cb_lock);
		return;
	}

	for (c = 0; c < NCPUS; c++) {
		if (c == me)
			continue;
		if (machine_slot[c].running &&
		    !cpu_data[c].rcu_cpu_idle &&
		    cpu_data[c].rcu_qs_seq == rcu_snap[c]) {
			done = 0;
			break;
		}
	}

	if (done) {
		rcu_gp++;
		rcu_active = 0;
	}

	hw_lock_unlock(&rcu_cb_lock);
}

void
urmach_rcu_drain(void)
{
	struct urmach_rcu_head	*ready = 0, *keep = 0, *h, *next;
	unsigned int		 gp;

	if (rcu_cb_list == 0 || rcu_gp == rcu_drained_gp)
		return;

	hw_lock_lock(&rcu_cb_lock);
	gp = rcu_gp;
	rcu_drained_gp = gp;
	h = rcu_cb_list;
	rcu_cb_list = 0;
	while (h != 0) {
		next = h->next;
		if (h->gp <= gp) {
			h->next = ready;
			ready = h;
			urmach_rcu_retired++;
		} else {
			h->next = keep;
			keep = h;
		}
		h = next;
	}
	rcu_cb_list = keep;
	hw_lock_unlock(&rcu_cb_lock);

	/*
	 * ⚠️ Outside the lock, and that is not a convenience: a callback frees
	 * memory and takes the allocator's own lock, and holding two locks in
	 * an order nothing else obeys is how a deadlock gets written.  The
	 * list is private by now -- it was detached above.
	 */
	while (ready != 0) {
		next = ready->next;
		ready->func(ready);
		ready = next;
	}
}
