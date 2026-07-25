/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Starting the other processors (#438).
 */

#include <stdint.h>

#include <cpu/acpi.h>
#include <cpu/ap_trampoline.h>
#include <cpu/lapic.h>
#include <cpu/percpu.h>
#include <cpu/regs.h>
#include <cpu/smp.h>
#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>
#include <sync/atomic.h>
#include <trap/trap.h>

extern char __trampoline_start[], __trampoline_end[];

/* Where each processor's first stack is.  Indexed by APIC id. */
uint64_t ap_stack_top[SMP_MAX_CPUS];

/* One bit per processor that has reached C.  Read by the boot processor. */
static volatile uint64_t online_mask;
static volatile uint64_t online_count;

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
	 * Descriptor tables and per-CPU state are this processor's own from
	 * here; nothing below is shared with another.
	 */
	percpu_activate(apic_id);

	atomic_test_and_set_bit((volatile uint64_t *)&online_mask, apic_id);
	atomic_inc64((volatile uint64_t *)&online_count);

	/*
	 * Nothing to do yet — the scheduler is #408's, and an idle loop is
	 * what an processor with no threads honestly is.  Halting rather than
	 * spinning so a hyperthreaded sibling gets the core.
	 */
	for (;;)
		__asm__ volatile("hlt");
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
