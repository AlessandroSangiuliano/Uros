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
	 *	BSS and therefore already zero at boot.  Nothing to do; kept as
	 *	an explicit init hook for symmetry and future state.
	 */
}

void
urmach_synchronize_rcu(void)
{
	unsigned int	snap[NCPUS];
	int		c;
	int		me = cpu_number();

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
				       "(qs_seq=%u snap=%u), still waiting\n",
				       c, cpu_data[c].rcu_qs_seq, snap[c]);
				spins = 0;
			}
		}
	}
}
