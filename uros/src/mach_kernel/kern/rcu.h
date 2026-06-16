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
 *	TRAP for the future: if kernel preemption is ever enabled (MACH_RT!=0
 *	or an explicit preempt point), a reader could migrate CPU mid-section
 *	and this per-CPU scheme breaks.  Then rcu_read_lock() must either
 *	disable_preemption() for the duration of the read section, or
 *	rcu_read_depth must move into the thread structure.
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

/* One-time init (counters live in BSS-zeroed cpu_data[], so this is a stub). */
extern void	urmach_rcu_init(void);

#endif	/* _KERN_RCU_H_ */
