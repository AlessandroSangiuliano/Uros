/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What a processor with nothing to do does (#461).
 *
 * Until this file the answer was "spin".  <x86_64/power_save.h> answered
 * POWER_SAVE 0, which was the honest answer while there was no idle path here
 * at all -- and the header said so, and said it would flip in the same change
 * that gave the target one.  This is that change: the application processors
 * are in the scheduler now, and three of them turning a `pause' loop at full
 * rate is not a machine at rest.
 *
 * ── THE ONE THING THAT CAN GO WRONG ──────────────────────────────────
 *
 * A halted processor wakes for an interrupt and for nothing else, so the
 * scheduler has to knock when it gives one work.  Between the knock and the
 * halt there is a window: the idle loop finds nothing to run, the dispatcher
 * publishes a thread and knocks, the interrupt is taken and returns, and only
 * then does the processor halt -- against a doorbell that has already rung.
 * It sleeps until something else happens to it, which on an otherwise idle
 * machine may be never.
 *
 * Two halves close it, and they are the two halves of a Dekker pair:
 *
 *   this side	 raises `halted', fences, and only then re-reads the places
 *		 work can appear.  The re-read is the point: if the dispatcher
 *		 published before the fence, this sees it and does not halt.
 *
 *   that side	 publishes the work, fences, and only then reads `halted'.  If
 *		 it publishes after this side's re-read, then this side has
 *		 already raised the flag, so the knock is sent.
 *
 * The fence is a real one.  x86 keeps stores in order and loads in order, and
 * it does NOT keep a store ahead of a later load -- which is exactly the
 * ordering both halves depend on, and the only case on this architecture
 * where a barrier instruction is actually required rather than decorative.
 *
 * And the halt itself is `sti; hlt' as one step, because x86 does not
 * recognise an interrupt between those two instructions.  Enabling and then
 * halting as two statements would reopen the window inside the code that
 * exists to close it.
 */

#include <stdint.h>

#include <kern/ast.h>			/* need_ast */
#include <kern/cpu_number.h>
#include <kern/processor.h>
#include <kern/thread.h>		/* THREAD_NULL */
#include <mach/machine.h>		/* machine_info.avail_cpus */

#include <cpu/ipi.h>
#include <cpu/regs.h>
#include <time/clock_event.h>	/* #461: say what the ticks recorded */
#include <cpu/smp.h>			/* real_ncpus */
#include <power_save.h>
#include <sync/barrier.h>

/*
 * How many fruitless passes of the idle loop before halting.
 *
 * Not zero, because entering and leaving a halt is not free and a processor
 * that is about to be given work would pay it for nothing.  Not large, because
 * every pass is a processor kept awake for a thread that is not coming.  The
 * number is a spin length, not a duration: what it buys is the case where work
 * arrives within a few microseconds of the last thread blocking, which is the
 * common shape in a microkernel where a server replies almost immediately.
 */
#define IDLE_HLT_GRACE		64

/*
 * Cache-line apart, and per processor by index rather than through %gs.
 *
 * The per-CPU block would be the natural home for everything else here, and is
 * the wrong home for this: `halted' is read by OTHER processors, which reach a
 * block only through their own segment base.  State that crosses processors
 * lives where every processor can address it.
 *
 * Padded because the dispatcher writes one line while the halting processor
 * writes another, and two of them in one line would trade a wake-up for an
 * invalidation on every pass.
 */
struct idle_state {
	volatile uint32_t	halted;
	uint32_t		dry;
	uint64_t		naps;		/* times this one halted */
	uint64_t		knocks;		/* doorbells this one sent */
	uint8_t			pad[64 - 24];
} __attribute__((aligned(64)));

static struct idle_state idle_state[NCPUS];

void
machine_idle(int mycpu)
{
	struct idle_state	*st;
	processor_t		me;

	if (mycpu < 0 || mycpu >= NCPUS)
		return;

	st = &idle_state[mycpu];

	/*
	 * A processor with nothing to do is the right place to say what its
	 * clock has been recording (#461).  Thread context, interrupts on, and
	 * every processor passes through here -- which is exactly what the tick
	 * handler is not and cannot do.
	 */
	clock_event_drain_reports();

	/*
	 * Never while processors are still arriving.  Bring-up runs with
	 * interrupt routing that is only partly built, and a boot that hangs is
	 * far easier to read as a spin than as a halt -- a halted processor and
	 * a wedged one look identical from outside.  The same gate the
	 * scheduler's own hand-off uses.
	 */
	if (machine_info.avail_cpus < real_ncpus)
		return;

	if (++st->dry < IDLE_HLT_GRACE)
		return;

	me = current_processor();

	/*
	 * From here to the halt, closed.  Anything that arrives is held
	 * pending and ends the halt itself rather than being consumed before
	 * it.
	 */
	interrupts_disable();

	st->halted = 1;
	smp_mb();

	/*
	 * Every place work can appear, re-read after the flag is up.  Not just
	 * next_thread: a thread may have been queued on this processor's own
	 * run queue or on the set's without anyone going through the
	 * dispatch-to-an-idle-processor path at all, and an AST may be pending
	 * against this processor for something that is not scheduling.
	 */
	if (me->next_thread == THREAD_NULL &&
	    me->runq.count == 0 &&
	    me->processor_set->runq.count == 0 &&
	    (need_ast[mycpu] & ~AST_SCHEDULING) == 0) {
		st->naps++;
		__asm__ __volatile__("sti; hlt" : : : "memory");
	} else
		interrupts_enable();

	st->halted = 0;
}

void
machine_idle_exit(int mycpu)
{
	if (mycpu < 0 || mycpu >= NCPUS)
		return;

	idle_state[mycpu].dry = 0;
	idle_state[mycpu].halted = 0;
}

void
machine_idle_wake(int cpu)
{
	if (cpu < 0 || cpu >= NCPUS)
		return;

	/*
	 * The other half of the pair.  The caller has already published the
	 * thread and the DISPATCHING state; this fence is what puts those
	 * stores ahead of the load below, and without it a processor that
	 * raised `halted' after this read would sleep on work already given to
	 * it.
	 */
	smp_mb();

	if (idle_state[cpu].halted == 0)
		return;

	idle_state[cpu_number()].knocks++;

	/*
	 * The AST vector, whose handler does nothing at all.  That is the whole
	 * requirement: the processor has to wake up and re-read a word in
	 * memory, and taking an interrupt and returning from it is how it gets
	 * back to the top of its idle loop to do so.
	 */
	ipi_ast_check((uint32_t) cpu);
}

uint64_t
machine_idle_naps(int cpu)
{
	if (cpu < 0 || cpu >= NCPUS)
		return 0;

	return idle_state[cpu].naps;
}

uint64_t
machine_idle_knocks(int cpu)
{
	if (cpu < 0 || cpu >= NCPUS)
		return 0;

	return idle_state[cpu].knocks;
}
