/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The processor surface the machine-independent scheduler calls (#453).
 *
 * Five names: how many processors there really are, how to start one, how to
 * control one, and the two halves of asking a processor to look at its
 * pending work.  The mechanism is x86_64/cpu/smp.c and x86_64/cpu/ipi.c;
 * this is the layer between them and kern/machine.c.
 */

#include <stdint.h>

#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/thread.h>
#include <kern/thread_act.h>
#include <mach/kern_return.h>
#include <mach/processor_info.h>	/* processor_info_t */

#include <cpu/ipi.h>
#include <cpu/percpu.h>
#include <cpu/smp.h>
#include <trap/trap.h>

/*
 * How many processors this machine really has.
 *
 * ⚠️ Read by kern/sched_prim.c to decide when the machine is fully up:
 * `machine_info.avail_cpus >= real_ncpus'.  So it is the number that were
 * found, not the number configured -- NCPUS is the size of the per-CPU
 * arrays and is 64 whatever the machine turns out to be.  A real_ncpus of 64
 * on a four-processor machine would mean the scheduler waited forever for
 * sixty processors that do not exist.
 *
 * Set once by the bring-up, from what ACPI reported and the trampoline
 * confirmed.
 */
int	real_ncpus = 1;

void
machine_real_ncpus_init(void)
{
	real_ncpus = (int) smp_online_count();
}

/*
 * Start the processor in this slot.
 *
 * The bring-up here is not per-processor: x86_64/cpu/smp.c starts them all
 * in one pipelined pass at boot, because the trampoline and the funnel
 * counter that paces it are shared and starting one at a time would mean
 * setting that machinery up and tearing it down per processor.
 *
 * So by the time anything calls this, the processor is either already
 * running or was never found -- and the answer is which of the two.
 *
 * ⚠️ KERN_FAILURE and not KERN_SUCCESS for one that is not there.  The
 * caller is asking for a processor to be made available and would otherwise
 * dispatch work to it.
 */
kern_return_t
cpu_start(int slot_num)
{
	if (slot_num < 0 || (unsigned) slot_num >= smp_online_count())
		return KERN_FAILURE;

	return KERN_SUCCESS;
}

/*
 * Machine-specific processor control -- the operations a processor_t exposes
 * beyond start and exit.
 *
 * ⚠️ This machine has none, so every request is refused rather than accepted
 * and ignored.  KERN_FAILURE is what the interface says for "this processor
 * does not support that", and a caller who asked for a control it did not
 * get is entitled to know.
 */
kern_return_t
cpu_control(int slot_num, processor_info_t info, unsigned int count)
{
	(void) slot_num;
	(void) info;
	(void) count;

	return KERN_FAILURE;
}

/*
 * Arrange for a processor to look at its pending asynchronous work.
 *
 * The pair exists because a processor cannot be interrupted into checking
 * something it is already checking: init_ast_check() prepares whatever the
 * machine needs so that the check can be provoked, and cause_ast_check()
 * provokes it.
 *
 * Here the first has nothing to prepare -- the mechanism is an interprocessor
 * interrupt and the interrupt controller is already up -- and the second is
 * that interrupt.  The processor takes it, returns from it, and the return
 * path is where the machine-independent AST check runs.
 *
 * ⚠️ Not sent to this processor.  A processor that wants to check its own
 * ASTs does so on the way out of whatever it is doing, and an IPI to itself
 * would be a slower way of arriving at the same place.
 */
void
init_ast_check(processor_t processor)
{
	(void) processor;
}

void
cause_ast_check(processor_t processor)
{
	if (processor == PROCESSOR_NULL)
		return;

	if (processor->slot_num == (int) cpu_number())
		return;

	ipi_ast_check((uint32_t) processor->slot_num);
}

/*
 * The trap frame of the thread this processor is running.
 *
 * Called from assembly, by the three return paths in trap/entry.S, because
 * reaching a thread's frame is three pointer hops through structures whose
 * layout the machine-independent tree owns -- and a hop written in assembly
 * is a constant that goes stale the day one of those structures gains a
 * field.
 *
 * ⚠️ Panics rather than answering zero.  Its callers are about to load the
 * result into %rsp and execute iretq off it; a null would iretq off address
 * zero, in ring 0, with the fault arriving somewhere unrecognisable.
 */
struct trap_frame *
act_user_frame(void)
{
	thread_t	thread = (thread_t) percpu()->active_thread;
	thread_act_t	act;

	if (thread == THREAD_NULL)
		panic("act_user_frame: no thread on this processor");

	act = thread->top_act;
	if (act == THR_ACT_NULL || act->mact.pcb == PCB_NULL)
		panic("act_user_frame: thread %p has no activation", thread);

	if (act->mact.pcb->user == (struct trap_frame *) 0)
		panic("act_user_frame: thread %p has never been to user mode",
		      thread);

	return act->mact.pcb->user;
}
