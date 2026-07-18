/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * power_save.c — HLT the idle loop (#357).
 *
 * Before this file existed POWER_SAVE was 0 and machine_idle() had no
 * implementation: every idle CPU PAUSE-spun at 100% forever.  On a
 * 32-thread part the spinners eat the package power/thermal budget that
 * the working cores need for boost, and on KVM they saturate the host.
 *
 * Two-phase idle:
 *
 *   Phase 1 (poll): the idle loop in sched_prim.c keeps polling
 *   next_thread / runq counts exactly as before (with the #319 1/64
 *   gcount sampling).  machine_idle() just counts dry passes, so a hot
 *   RPC ping-pong partner is picked up with zero added latency and no
 *   IPI ever fires.
 *
 *   Phase 2 (halt): after `idle_hlt_grace` consecutive dry passes the
 *   CPU publishes a per-CPU `halted` flag and executes sti;hlt.  A
 *   halted CPU no longer polls, so thread_setrun's idle-dispatch calls
 *   machine_idle_wake() after writing next_thread: if the target's flag
 *   is set it sends a bare doorbell IPI (cpu_interrupt).  Plain HLT is
 *   C1/C1E — wake is ~1-2 µs, paid only on the first dispatch after a
 *   quiet spell, never per-RPC in steady state.
 *
 * The sleep/wake handshake is Dekker-style; both sides need a StoreLoad
 * fence, which x86 does not give for free:
 *
 *   sleeper:    ST halted=1;      MFENCE;  LD next_thread  → hlt if none
 *   dispatcher: ST next_thread;   MFENCE;  LD halted       → IPI if set
 *
 * With both fences it is impossible that the sleeper misses the work
 * AND the dispatcher misses the flag, so a wakeup cannot be lost.  The
 * final work re-check runs under cli; the sti;hlt pair has no window
 * (the STI interrupt shadow guarantees a pending interrupt is delivered
 * only once HLT is entered, and it wakes it).  Interrupt-driven work
 * needs no flag at all: the interrupt itself terminates HLT.  As a last
 * belt the per-CPU LAPIC tick (#312) bounds any residual missed wake to
 * one clock tick.
 *
 * The grace counter is NOT reset when a tick briefly wakes the CPU and
 * there is still nothing to do — it re-halts on the very next pass.  It
 * is reset by machine_idle_exit() only when the idle loop actually
 * hands the CPU a thread, so intermittent idle stints during a hot
 * workload each get the full poll window again.
 *
 * `-S` (spin) reverts to the legacy always-poll idle for A/B runs.
 */

#include <cpus.h>
#include <power_save.h>

#include <mach/machine.h>		/* machine_info */
#include <kern/ast.h>			/* need_ast[], AST_SCHEDULING */
#include <kern/cpu_number.h>
#include <kern/processor.h>		/* current_processor, processor_t */
#include <kern/thread.h>		/* THREAD_NULL */

#if	NCPUS > 1
extern int	real_ncpus;		/* mp.c: CPUs the MP table promised */
extern void	cpu_interrupt(int cpu);	/* mp_stub.c: doorbell IPI */
#endif	/* NCPUS > 1 */

/*
 * Runtime switch, cleared by the -S boot flag.  Kept out of the BSS
 * (#337: parse_arguments runs BEFORE the BSS clear, so a BSS flag set
 * there is silently wiped).
 */
int		sched_idle_hlt __attribute__((section(".data"))) = 1;

/*
 * Dry poll passes before arming HLT.  One pass through the idle poll
 * loop costs roughly 10²-10³ cycles, so the default window is in the
 * hundreds-of-µs range — orders of magnitude above any RPC wait, while
 * still parking a genuinely idle CPU essentially immediately at human
 * timescales.  Tunable from DDB.
 */
unsigned int	idle_hlt_grace __attribute__((section(".data"))) = 4096;

/*
 * Per-CPU state, one cache line each: `halted` is read by remote
 * dispatchers, everything else is CPU-private (counters are for DDB
 * examine only — no printf here, and none from interrupt context ever).
 * wake_ipis_sent is bumped on the SENDER's own line.
 */
struct idle_hlt_pcpu {
	volatile int	halted;
	unsigned int	dry;
	unsigned long	naps;		/* times this CPU entered HLT */
	unsigned long	wake_ipis_sent;	/* doorbells this CPU sent */
	char		pad[64 - 2 * sizeof(int) - 2 * sizeof(unsigned long)];
} __attribute__((aligned(64)));

static struct idle_hlt_pcpu idle_hlt[NCPUS] __attribute__((aligned(64)));

/* Full barrier (StoreLoad included) that works on plain i686. */
#define	IDLE_HLT_MFENCE() \
	__asm__ __volatile__("lock; addl $0,(%%esp)" : : : "memory", "cc")

/*
 * machine_idle() — called by the idle loop once per dry poll pass, at
 * low spl with interrupts enabled, after the PAUSE.
 */
void
machine_idle(int mycpu)
{
	register struct idle_hlt_pcpu	*st = &idle_hlt[mycpu];
	register processor_t		myprocessor;

	if (!sched_idle_hlt)
		return;
#if	NCPUS > 1
	/*
	 * Never halt while APs are still coming online: bring-up runs
	 * with partial interrupt routing and we want boot hangs to stay
	 * debuggable as pure spins (same gate as the #356 hand-off).
	 */
	if (machine_info.avail_cpus < real_ncpus)
		return;
#endif	/* NCPUS > 1 */
	if (++st->dry < idle_hlt_grace)
		return;

	myprocessor = current_processor();

	/*
	 * Closed window: from here to HLT no interrupt can slip in and
	 * have its wakeup consumed before we sleep — anything that
	 * arrives is held pending and terminates the HLT itself.
	 */
	__asm__ __volatile__("cli" : : : "memory");
	st->halted = 1;
	IDLE_HLT_MFENCE();
	if (myprocessor->next_thread == THREAD_NULL &&
	    myprocessor->runq.count == 0 &&
	    myprocessor->processor_set->runq.count == 0 &&
	    (need_ast[mycpu] & ~AST_SCHEDULING) == 0) {
		st->naps++;
		__asm__ __volatile__("sti; hlt" : : : "memory");
	} else
		__asm__ __volatile__("sti" : : : "memory");
	st->halted = 0;
}

/*
 * machine_idle_exit() — the idle loop got a thread to run: close the
 * idle stint so the next one starts with a fresh poll window.
 */
void
machine_idle_exit(int mycpu)
{
	idle_hlt[mycpu].dry = 0;
	idle_hlt[mycpu].halted = 0;
}

#if	NCPUS > 1
/*
 * machine_idle_wake() — called by thread_setrun (at splsched) right
 * after publishing next_thread/PROCESSOR_DISPATCHING to an idle
 * processor.  The dispatcher side of the Dekker pair above.
 */
void
machine_idle_wake(int cpu)
{
	if (!sched_idle_hlt)
		return;
	IDLE_HLT_MFENCE();
	if (idle_hlt[cpu].halted) {
		idle_hlt[cpu_number()].wake_ipis_sent++;
		cpu_interrupt(cpu);
	}
}
#endif	/* NCPUS > 1 */
