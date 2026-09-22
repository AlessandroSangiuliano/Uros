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
 *	File:	kern/rcu.h
 *
 *	Quiescent-state-based RCU (QSBR), #331 step 2.
 *
 *	The capability table's read side wants to run lock-free.  QSBR gives
 *	readers an effectively free read_lock/unlock and pushes all the cost to
 *	the (rare) writer, which waits a grace period after unlinking an object
 *	before freeing it -- by then no reader can still hold a pointer to it.
 *
 *	Quiescent state.  A CPU is "quiescent" when it holds no RCU read
 *	reference (rcu_read_depth == 0).  Each CPU reports quiescent states by
 *	bumping a per-CPU counter (rcu_qs_seq) at three points where it is
 *	provably not mid-lookup: a context switch (thread_dispatch), the idle
 *	loop, and the per-CPU clock tick (hardclock).  The clock tick is the
 *	backstop that covers a CPU busy-spinning on a lock: it is not in a read
 *	section there, so its depth is 0 and the tick reports its quiescence,
 *	keeping grace periods from stalling on a non-switching CPU.
 *
 *	Grace period.  urmach_synchronize_rcu() snapshots every online CPU's
 *	rcu_qs_seq after the writer's unlink and waits until each has advanced
 *	once.  One advance means that CPU passed through depth==0 after the
 *	unlink, so any reader that started before the unlink is gone.
 *
 *	Why depth may be per-CPU (not per-thread).  This kernel is built with
 *	MACH_RT==0, so it is non-preemptive: an in-kernel reader runs from
 *	rcu_read_lock to rcu_read_unlock on one CPU without an involuntary
 *	context switch, and the lookup never blocks.  So the same CPU that
 *	increments rcu_read_depth decrements it, and the field is read only by
 *	that CPU (the three checkpoints all run on the local CPU; the clock
 *	tick is a same-CPU interrupt, which observes the local store by
 *	architectural self-consistency).  rcu_read_depth is therefore never
 *	touched cross-CPU -- no atomics, no fences.
 *
 *	TRAP that HAS SPRUNG: if kernel preemption is ever enabled (MACH_RT!=0
 *	or an explicit preempt point), a reader could migrate CPU mid-section
 *	and this per-CPU scheme breaks.  x86-64 added the explicit preempt
 *	point in #459/#463 -- an AST on the way out of any trap, ring 0
 *	included -- so rcu_read_lock() now takes the first of the two remedies
 *	named here and disables preemption for the duration of the section.
 *	See the note on it below; the second remedy, moving the depth into the
 *	thread, would not have been sufficient by itself.
 *
 *	Memory ordering.  i386 is TSO: the only cross-CPU datum is rcu_qs_seq,
 *	and the reader->writer handoff (last deref of the old object, then the
 *	qs_seq store; the writer's qs_seq load, then the free) is ordered by
 *	TSO with just compiler barriers here.  The publish fence lives in
 *	urmach_synchronize_rcu() (see rcu.c).  A non-TSO port would need real
 *	read/write barriers in these inlines.
 */

#ifndef	_KERN_RCU_H_
#define	_KERN_RCU_H_

#include <kern/cpu_data.h>	/* cpu_data[], cpu_number() */

/* Compiler barrier: keep the read-section accesses from being hoisted/sunk
 * across the depth bump.  Sufficient on i386 TSO (see file header). */
#define	urmach_rcu_barrier()	__asm__ __volatile__("" : : : "memory")

/*
 *	Enter a read-side critical section.  Cheap: a CPU-local increment.
 *	Read sections must not block or voluntarily context-switch (QSBR rule);
 *	in this non-preemptive kernel an IPC lookup satisfies that by running
 *	to completion.  Nestable.
 */
static __inline__ void
urmach_rcu_read_lock(void)
{
	/*
	 *	🔥 PREEMPTION OFF FIRST, AND THE TRAP BELOW HAS ALREADY SPRUNG.
	 *
	 *	The note further down says this scheme breaks "if kernel
	 *	preemption is ever enabled (MACH_RT!=0 or an explicit preempt
	 *	point)".  #459/#463 added exactly that on x86-64: the kernel takes
	 *	an AST on the way out of any trap, including in ring 0, so a
	 *	reader can be preempted mid-section and resumed on ANOTHER
	 *	processor -- and then the increment lands on one CPU's counter and
	 *	the decrement on another's.
	 *
	 *	What that costs is not a lost update, it is a permanent one: the
	 *	CPU that took the increment never sees depth 0 again, so it never
	 *	reports a quiescent state again, so EVERY FUTURE GRACE PERIOD ON
	 *	THE MACHINE WAITS FOR IT FOR EVER.  Observed as a wedged boot with
	 *
	 *	  urmach_rcu: WARNING cpu 1 not quiescent (qs_seq=4 snap=4)
	 *
	 *	repeating, and confirmed by a temporary check that recorded the
	 *	processor at lock and compared it at unlock:
	 *
	 *	  panic(cpu 1): pmap read section entered on cpu 3, left on cpu 1
	 *
	 *	Of the two remedies the note names -- disable preemption, or move
	 *	the depth into the thread -- this is the first, because it is the
	 *	one that keeps the counter per-CPU and therefore keeps the
	 *	quiescent state meaning what it says.  Moving the depth into the
	 *	thread would NOT be enough on its own: a preempted reader holds a
	 *	reference while running on no processor at all, so a CPU whose
	 *	current thread has depth zero would report a quiescence that is not
	 *	true of the machine.
	 *
	 *	⚠️ It costs read sections the right to block, which QSBR forbade
	 *	anyway ("read sections must not block or voluntarily
	 *	context-switch"), so the rule is unchanged and now enforced rather
	 *	than asked for.
	 *
	 *	⚠️ On i386 disable_preemption() is still the empty definition from
	 *	<kern/cpu_data.h> and that target does not preempt in kernel mode,
	 *	so this generates nothing there.
	 */
	disable_preemption();
	cpu_data[cpu_number()].rcu_read_depth++;
	urmach_rcu_barrier();
}

/*
 *	Leave a read-side critical section.
 */
static __inline__ void
urmach_rcu_read_unlock(void)
{
	urmach_rcu_barrier();
	cpu_data[cpu_number()].rcu_read_depth--;
	enable_preemption();
}

/*
 *	Report a quiescent state for the current CPU, but only if it is not
 *	inside a read section right now (depth==0).  Called from the three
 *	checkpoints (thread_dispatch, idle loop, hardclock).  The depth gate is
 *	what makes the clock-tick checkpoint safe: a tick that lands mid-lookup
 *	sees depth>0 and reports nothing, so it cannot end a grace period out
 *	from under an active reader.
 */
static __inline__ void
urmach_rcu_quiescent_state(void)
{
	int me = cpu_number();

	if (cpu_data[me].rcu_read_depth == 0) {
		urmach_rcu_barrier();
		cpu_data[me].rcu_qs_seq++;	/* sole cross-CPU datum (volatile) */
	}
}

/*
 *	Mark the current CPU as entering / leaving the idle extended quiescent
 *	state.  An idle CPU holds no read reference, so urmach_synchronize_rcu
 *	counts it as quiescent on sight -- essential because an idle CPU may be
 *	HLTed and not report a quiescent state (tick / context switch) for a
 *	long time, which would otherwise stall every grace period.
 */
static __inline__ void
urmach_rcu_idle_enter(void)
{
	cpu_data[cpu_number()].rcu_cpu_idle = 1;
	urmach_rcu_barrier();
}

static __inline__ void
urmach_rcu_idle_exit(void)
{
	urmach_rcu_barrier();
	cpu_data[cpu_number()].rcu_cpu_idle = 0;
}

/* Wait one grace period: every online CPU must report a quiescent state.
 * Blocking; call only from a sleepable context with no lock held and not at
 * high IPL (it spins with interrupts on).  Rare write paths only. */
extern void	urmach_synchronize_rcu(void);

/*
 *	── Deferred reclamation: queue it, do not wait for it (#566) ────────
 *
 *	urmach_synchronize_rcu() blocks the writer until every processor has
 *	reported.  On a machine where a peer is busy in the kernel that is a
 *	whole clock tick, because a busy processor reports only when it ticks
 *	-- measured: destroying two hundred tasks while another enumerated PCI
 *	cost 31 035 741 cycles each against 2 824 when the peers were idle, and
 *	2 309 with the wait removed altogether.
 *
 *	🔑 NO OTHER SYSTEM BLOCKS THE DESTROYER.  Linux's normal path is
 *	call_rcu() -- queue a callback, return at once, and one grace period
 *	retires hundreds of them; kfree_rcu() is the sugar for "just free it".
 *	FreeBSD has epoch_call(), NetBSD pserialize with cross-calls, and XNU
 *	-- this kernel's own relative -- frees the pmap from a deferred list.
 *	Only synchronize_rcu() blocks, and Linux's version SLEEPS while it
 *	does; ours spins, which is the shape that made a trap cost a tick.
 *
 *	So this is that primitive.  The caller embeds a head in the object,
 *	unlinks the object, calls urmach_call_rcu(), and returns.  The callback
 *	runs later, in thread context, once a grace period that BEGAN AFTER the
 *	call has ended.
 *
 *	⚠️ The object must already be unreachable to new readers when this is
 *	called -- the grace period counted is the one that starts after, not
 *	one already running, which is why the stamp below adds two while a
 *	grace period is in flight and one when none is.
 */
struct urmach_rcu_head {
	struct urmach_rcu_head	*next;
	void			(*func)(struct urmach_rcu_head *);
	unsigned int		gp;	/* released once rcu_gp reaches this */
};

/* Queue `h' to be handed to `f' after a grace period.  Returns at once. */
extern void	urmach_call_rcu(struct urmach_rcu_head *h,
				void (*f)(struct urmach_rcu_head *));

/*
 *	One non-blocking step of the grace-period machine.  From the clock
 *	tick, in interrupt context: it takes a snapshot or checks one, and
 *	returns immediately when there is nothing queued.
 */
extern void	urmach_rcu_advance(void);

/*
 *	Run whatever is ready.  THREAD CONTEXT ONLY -- a callback frees memory
 *	and may take the zone lock.  The idle loop calls it, and so does any
 *	writer about to block in a grace period, so that a machine which never
 *	goes idle still retires its queue.
 */
extern void	urmach_rcu_drain(void);

/* How many callbacks are waiting and how many have been handed back. */
extern unsigned int	urmach_rcu_queued;
extern unsigned int	urmach_rcu_retired;

/* One-time init (counters live in BSS-zeroed cpu_data[], so this is a stub). */
extern void	urmach_rcu_init(void);

#endif	/* _KERN_RCU_H_ */
