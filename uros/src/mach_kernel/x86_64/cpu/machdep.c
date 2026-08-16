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
#include <mach/machine.h>		/* #461: machine_slot[], CPU_TYPE_X86_64 */
#include <mach/processor_info.h>	/* processor_info_t */

#include <cpu/acpi.h>			/* #461: which processors exist */
#include <cpu/desc.h>			/* #477: the selectors a return imposes */
#include <cpu/ipi.h>
#include <cpu/lapic.h>			/* #461: lapic_id */
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
 * Which slots of machine_slot[] are processors, and what kind (#461).
 *
 * ⚠️ NOTHING ON THIS TARGET SET THIS, and the consequence was not a wrong
 * answer from host_info() -- it was that the kernel had no idle threads at
 * all.  start_kernel_threads() creates one per slot with `is_cpu' set, so an
 * array of zeroes means zero idle threads, on every processor including the
 * boot processor.  It went unnoticed for three issues because the boot
 * processor is handed a real first thread and this kernel has never yet run
 * out of work to do: it stops at bootstrap_create (#422) long before any
 * processor would have to go idle.  The first processor to ask for its idle
 * thread was an application processor arriving in slave_main(), and it found
 * THREAD_NULL.
 *
 * Indexed by APIC id, because that is what cpu_number() answers on this
 * machine -- <x86_64/cpu_number.h> reads it out of the per-CPU block -- so
 * the slot a processor uses and the slot it is registered in are the same
 * number by construction rather than by a mapping somebody has to maintain.
 *
 * ⚠️ Sparse, therefore, and deliberately: firmware does not promise
 * consecutive identifiers, so a machine with four processors may light up
 * slots 0, 2, 4 and 6.  Everything that walks machine_slot[] tests is_cpu, so
 * the holes cost array space and nothing else -- and the alternative, a dense
 * logical numbering, means a translation on the path of every cpu_number()
 * call in the kernel.
 */
void
machine_slots_init(void)
{
	unsigned	self = (unsigned) lapic_id();
	unsigned	i, n;

	/*
	 * The boot processor first, and unconditionally.  It is running this
	 * code, which settles the question more firmly than any table: a
	 * machine whose ACPI tables list no processors at all still has this
	 * one, and would otherwise reach start_kernel_threads() with nothing
	 * marked and no idle thread anywhere.
	 */
	if (self >= NCPUS)
		panic("machine_slots_init: the boot processor's APIC id (%u) "
		      "is past the %d slots this kernel was built with", self,
		      NCPUS);

	machine_slot[self].is_cpu = TRUE;
	machine_slot[self].cpu_type = CPU_TYPE_X86_64;
	machine_slot[self].cpu_subtype = CPU_SUBTYPE_X86_64_ALL;
	n = 1;

	/*
	 * Then the ones that answered.  Online, not listed: a processor ACPI
	 * named and that never reported in has no per-CPU block, no descriptor
	 * table and no stack, and giving it an idle thread would mean the
	 * scheduler dispatching work to a processor that does not exist.
	 */
	for (i = 0; i < acpi_cpu_count(); i++) {
		const struct acpi_cpu *c = acpi_cpu(i);

		if (!c->usable || c->apic_id == self)
			continue;
		if (!smp_is_online(c->apic_id))
			continue;

		machine_slot[c->apic_id].is_cpu = TRUE;
		machine_slot[c->apic_id].cpu_type = CPU_TYPE_X86_64;
		machine_slot[c->apic_id].cpu_subtype = CPU_SUBTYPE_X86_64_ALL;
		n++;
	}

	if (n != (unsigned) real_ncpus)
		panic("machine_slots_init: %u slots marked but real_ncpus is "
		      "%d — the scheduler would wait for a processor that has "
		      "no slot, or give an idle thread to one that is not "
		      "there (#461)", n, real_ncpus);
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

	/*
	 * 🔥 And the two selectors, checked here, on the way out (#477).
	 *
	 * The caller is about to iretq off this frame, and iretq will not tell
	 * us anything useful if it dislikes what it finds: it raises a general
	 * protection at RING 0, naming a descriptor index and nothing about
	 * where the value came from.  Under TCG it does not even do that -- the
	 * emulator accepts an SS whose RPL does not match the CS's, so the
	 * thread enters ring 3 on a stack selector no real processor would have
	 * loaded, and the port's "it works" rests on that.
	 *
	 * These two fields are imposed by thread_state_to_frame() and by
	 * thread_frame_init() and are never a thread's to choose, so anything
	 * other than these values means something wrote over the frame.  That
	 * is a claim worth testing every time rather than a thing to remember:
	 * pcb->user lives IN the kernel stack, at its top, sharing those bytes
	 * with the frame a trap builds -- deliberately -- which makes "who else
	 * writes there" a permanent question rather than a bug that gets fixed
	 * once.
	 */
	if (act->mact.pcb->user->cs != USER_CS_RPL3
	    || act->mact.pcb->user->ss != USER_DS_RPL3)
		panic("act_user_frame: thread %p is about to return to ring 3 "
		      "with cs 0x%lx ss 0x%lx, not 0x%x/0x%x -- something wrote "
		      "over its frame (#477)",
		      thread,
		      (unsigned long) act->mact.pcb->user->cs,
		      (unsigned long) act->mact.pcb->user->ss,
		      USER_CS_RPL3, USER_DS_RPL3);

	return act->mact.pcb->user;
}
