/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Starting the other processors (#438).
 */

#include <stdint.h>

#include <cpu/acpi.h>
#include <cpu/ap_trampoline.h>
#include <cpu/desc.h>
#include <cpu/ipi.h>
#include <cpu/lapic.h>
#include <cpu/percpu.h>
#include <cpu/regs.h>
#include <cpu/smp.h>
#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>
#include <syscall/syscall.h>
#include <thread/fpu.h>
#include <sync/atomic.h>
#include <trap/trap.h>

extern char __trampoline_start[], __trampoline_end[];

/* Where each processor's first stack is.  Indexed by APIC id. */
uint64_t ap_stack_top[SMP_MAX_CPUS];

/* One bit per processor that has reached C.  Read by the boot processor. */
static volatile uint64_t online_mask;
static volatile uint64_t online_count;

/*
 * The other direction: one processor volunteers to send rather than serve.
 *
 * `claimed` is taken by whichever processor arrives first, so nothing here
 * depends on which identifiers the firmware handed out.  `gate` is the boot
 * processor's permission to go, withheld until every processor has an
 * interrupt table — a broadcast issued before that reaches processors that
 * cannot yet survive one.
 */
static volatile uint64_t ap_call_claimed;
static volatile uint64_t ap_call_gate;
static volatile uint64_t ap_call_done;
static volatile uint64_t ap_call_reached_bsp;

/*
 * What the volunteer asks everybody to run.  It asks the hardware whether it
 * is the boot processor rather than comparing identifiers, because that is
 * the actual claim being tested — that the message arrived *there* — and the
 * controller answers it without anyone having to record the answer earlier.
 */
static void ap_call_payload(void *arg)
{
	if (lapic_is_bsp())
		atomic_inc64((volatile uint64_t *)arg);
}

void ap_trampoline_install(void)
{
	uint8_t *page = (uint8_t *)(uintptr_t)phys_to_direct(AP_TRAMPOLINE_BASE);
	const uint8_t *src = (const uint8_t *)__trampoline_start;
	uint64_t size = (uint64_t)(__trampoline_end - __trampoline_start);
	extern void ap_entry64(void);

	if (size > AP_TRAMPOLINE_SIZE)
		panic("smp: the trampoline outgrew the layout it is indexed by");

	for (uint64_t i = 0; i < size; i++)
		page[i] = src[i];

	/*
	 * The two things it could not be assembled knowing.  Written after
	 * the copy, since the copy would overwrite them.
	 */
	*(volatile uint32_t *)(page + AP_PARAM_OFFSET) =
		(uint32_t)(read_cr3() & INTEL_PTE_PFN);
	*(volatile uint64_t *)(page + AP_PARAM_OFFSET + 8) =
		(uint64_t)(uintptr_t)ap_entry64;
}

/*
 * Where an application processor arrives, in the higher half, with no stack
 * and nothing else set up.
 *
 * The first act is finding out which processor this is, and CPUID answers
 * without needing anywhere to put anything — which is the whole reason the
 * stack can be chosen here rather than handed out by whoever did the waking.
 * That is what removes the shared boot stack, and with it the funnel the
 * i386 bring-up has to serialise on.
 */
void ap_entry64(void);

void ap_start_c(uint32_t apic_id)
{
	/*
	 * Off the trampoline's sixteen bytes of descriptor table and onto the
	 * kernel's, with a task register and an interrupt descriptor table of
	 * its own.  First, because until it is done this processor cannot
	 * report a fault — it can only triple-fault and reset, which is the
	 * one failure that leaves nothing on the wire to read afterwards.
	 */
	desc_activate(apic_id);

	/* Per-CPU state; nothing below is shared with another processor. */
	percpu_activate(apic_id);

	/*
	 * And its own interrupt controller, which comes out of a startup
	 * interrupt accepting nothing.  A message sent to a processor that
	 * has not done this is discarded, and the sender is told it was
	 * delivered.
	 */
	lapic_enable();

	/*
	 * And its own SYSCALL machinery, for the same reason: LSTAR, STAR and
	 * the enable bit are per-processor, and a startup interrupt leaves
	 * none of them set.  A processor that missed this would not fail a
	 * syscall slowly — the instruction would not exist on it.
	 */
	syscall_init();

	/*
	 * And its floating-point unit, which arrives from a startup interrupt
	 * with the emulation bit set as reset leaves it — so the first vector
	 * instruction would be an invalid opcode rather than an instruction.
	 */
	fpu_init();

	atomic_test_and_set_bit((volatile uint64_t *)&online_mask, apic_id);
	atomic_inc64((volatile uint64_t *)&online_count);

	/*
	 * Nothing to do yet — the scheduler is #408's, and an idle loop is
	 * what a processor with no threads honestly is.  Halting rather than
	 * spinning so a hyperthreaded sibling gets the core.
	 *
	 * With interrupts on, and that is the whole difference: a halted
	 * processor wakes for an interrupt and for nothing else, so this loop
	 * with them off is not an idle processor but a lost one.  Enabled
	 * here rather than earlier because the tables above are what makes an
	 * arriving interrupt survivable.
	 */
	interrupts_enable();

	/*
	 * The first processor here takes the job of sending one, so that the
	 * direction nothing else exercises gets exercised.  Whoever wins waits
	 * for the boot processor to say every processor has a table to take an
	 * interrupt on; the rest fall straight through to idle.
	 *
	 * The wait is bounded, and giving up is reported rather than hidden:
	 * an unbounded spin here would turn a boot processor that never opens
	 * the gate into a hung machine, which says less than a probe that
	 * comes back zero.
	 */
	if (atomic_cmpxchg64(&ap_call_claimed, 0, apic_id + 1) == 0) {
		uint64_t spins;

		for (spins = 0; spins < 400000000ULL; spins++) {
			if (atomic_load64(&ap_call_gate) != 0)
				break;
			cpu_pause();
		}

		if (atomic_load64(&ap_call_gate) != 0)
			ipi_call_others(ap_call_payload,
					(void *)&ap_call_reached_bsp);

		atomic_store64(&ap_call_done, 1);
	}

	for (;;)
		__asm__ volatile("hlt");
}

unsigned smp_ap_call_probe(void)
{
	uint64_t spins;

	if (atomic_load64(&ap_call_claimed) == 0)
		return 0;		/* nobody arrived to volunteer */

	atomic_store64(&ap_call_gate, 1);

	for (spins = 0; spins < 400000000ULL; spins++) {
		if (atomic_load64(&ap_call_done) != 0)
			break;
		cpu_pause();
	}

	return (unsigned)atomic_load64(&ap_call_reached_bsp);
}

unsigned smp_online_count(void)
{
	return (unsigned)atomic_load64((volatile uint64_t *)&online_count) + 1;
}

int smp_is_online(uint32_t apic_id)
{
	return (atomic_load64((volatile uint64_t *)&online_mask)
		>> apic_id) & 1;
}

unsigned smp_start_others(void)
{
	uint32_t self = lapic_id();
	unsigned asked = 0;
	uint64_t spins;

	ap_trampoline_install();

	for (unsigned i = 0; i < acpi_cpu_count(); i++) {
		const struct acpi_cpu *c = acpi_cpu(i);
		uint64_t frame;

		if (!c->usable || c->apic_id == self)
			continue;

		if (c->apic_id >= SMP_MAX_CPUS)
			panic("smp: an APIC id past the stack table");

		/*
		 * A stack each, before anyone is woken.  This is the whole of
		 * the arrangement: the processor finds its own by id, so there
		 * is nothing to hand over and nothing to take turns with.
		 */
		frame = boot_frames_alloc(4);
		if (frame == 0)
			panic("smp: no memory for a processor's first stack");

		ap_stack_top[c->apic_id] =
			phys_to_direct(frame) + 4 * PAGE_SIZE_4K;

		/*
		 * And its per-CPU page, here rather than on arrival: mapping
		 * it edits the kernel's page tables, and every processor doing
		 * that at the moment it wakes is the one race this design
		 * would otherwise have.
		 */
		percpu_alloc(c->apic_id);

		/*
		 * Its task-state segment and the stacks a fault will land on,
		 * for the same reason twice over: this writes into the shared
		 * descriptor table, and it allocates.  Neither is something to
		 * have several processors doing at the moment they arrive.
		 */
		desc_alloc(c->apic_id);
	}

	/*
	 * Every stack is in place, so every processor can be woken without
	 * waiting for the one before it.
	 */
	for (unsigned i = 0; i < acpi_cpu_count(); i++) {
		const struct acpi_cpu *c = acpi_cpu(i);

		if (!c->usable || c->apic_id == self)
			continue;

		lapic_send_init(c->apic_id);
		lapic_send_startup(c->apic_id, AP_TRAMPOLINE_BASE);
		lapic_send_startup(c->apic_id, AP_TRAMPOLINE_BASE);
		asked++;
	}

	/*
	 * Wait for the count, not for a sequence.  A processor that never
	 * arrives is a fact to report, not a reason to abandon the ones that
	 * did — and with nothing shared in the trampoline, one that is stuck
	 * there is stuck alone.
	 */
	for (spins = 0; spins < 200000000ULL; spins++) {
		if (atomic_load64((volatile uint64_t *)&online_count) == asked)
			break;
		cpu_pause();
	}

	return (unsigned)atomic_load64((volatile uint64_t *)&online_count);
}
