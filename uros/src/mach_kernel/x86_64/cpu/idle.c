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
#include <cpu/quiet_census.h>		/* #476: who never ran */
#include <mach/machine.h>		/* machine_info.avail_cpus */

#include <cpu/ipi.h>
#include <cpu/regs.h>
#include <time/clock_event.h>	/* #461: say what the ticks recorded */
#include <cpu/smp.h>			/* real_ncpus */
#include <power_save.h>
#include <sync/barrier.h>
#include <cpu/percpu.h>		/* #526: raised_by, raised_on */
#include <cpu/spl.h>			/* #526: splget */
#include <ddb/ksym.h>			/* #526: the raise, by name */
#include <kern/misc_protos.h>		/* printf */

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
	uint32_t		said_level;	/* #526: said once */
	uint8_t			pad[64 - 28];
} __attribute__((aligned(64)));

static struct idle_state idle_state[NCPUS];

/*
 * 🔴 An idle processor whose level is above zero (#526).
 *
 * The idle loop restores the level it finds -- `s = splsched(); ... splx(s)'
 * -- and does not force zero, and the check that would have said so is under
 * `#if 0' in kern/sched_prim.c.  A level that reaches it at SPLHI stays
 * there: the processor's own tick is deferred for ever, and when that
 * processor is the master, timeout_tick() never runs again and every timed
 * wait in the machine sleeps for good.  That is how #526's boots stopped,
 * read on a live kernel: processor 0 idle at 14, the tick pending,
 * timeout_ticks frozen.
 *
 * So the first time a processor comes here above zero, it says so, with the
 * raise from zero that was never lowered (splx() keeps it in raised_by).
 * Said rather than mended: lowering the level here would let the boot go on
 * and leave the path that leaked it unknown.
 */
static void
idle_say_level(int mycpu, struct idle_state *st)
{
	struct percpu	*p = percpu();
	spl_t		level = splget();
	const char	*nm;
	uint64_t	off = 0;

	if (level == SPL0 || st->said_level)
		return;
	st->said_level = 1;

	nm = ksym_lookup_call(p->raised_by, &off);
	if (nm != 0)
		printf("idle: processor %d idles at level %u, which defers its "
		       "own tick -- WRONG; the level was raised from zero by "
		       "%s+0x%lx on thread %p and never lowered (#526)\n",
		       mycpu, (unsigned) level, nm, (unsigned long) off,
		       p->raised_on);
	else
		printf("idle: processor %d idles at level %u, which defers its "
		       "own tick -- WRONG; the level was raised from zero at "
		       "%p on thread %p and never lowered (#526)\n", mycpu,
		       (unsigned) level, (void *)(uintptr_t) p->raised_by,
		       p->raised_on);
}

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
	 * And, for the same reason, what every thread in the system is doing
	 * once nothing is doing anything (#476).  Prints once, after a long
	 * stretch of quiet; see kern/quiet_census.c for why this is not a gdb
	 * script.
	 */
	quiet_census_pass(mycpu);

	/* #526: and whether this processor came here with its level raised. */
	idle_say_level(mycpu, st);

	/*
	 * #599: refusals an engine recorded are no longer read here.  A
	 * processor spinning in a driver never comes here, so a refusal
	 * nobody asked about waited for the spin to end, and on a
	 * uniprocessor it could wait for ever; the fault log has a thread of
	 * its own (x86_64/cpu/iommu_fault.c).
	 */

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

	/*
	 * #599: the quiet census no longer counts this as work -- it asks
	 * whether a user task's thread ran (cpu/quiet_census.c).
	 */
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
