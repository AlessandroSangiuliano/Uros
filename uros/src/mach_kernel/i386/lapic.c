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
 * lapic.c — local APIC bring-up + IPI primitives (#302 increment 1).
 *
 * Public API documented in <i386/lapic.h>.  The MMIO window itself is
 * mapped by mp_table.c into `lapic_start` once the kernel VA system is
 * alive (mp_v1_1_init phase 2).  Each entry point here checks that
 * mapping before touching the device.
 *
 * Vector handlers + IDT wiring come in increment 2; AP-side enable
 * (slave_machine_init) + 8259 masking come in increment 3.
 */

#include <cpus.h>

#if	NCPUS > 1

#include <types.h>
#include <i386/lapic.h>
#include <kern/cpu_data.h>	/* current_cpu_id() */
#include <kern/misc_protos.h>	/* printf */

extern unsigned char	mp_bsp_lapic_id_get(void);
extern unsigned char	mp_cpu_lapic_id_get(int slot);

/* Spin until the previous IPI has been accepted by its target(s).
 * Polling LAPIC_ICR_DS_PENDING in the ICR low word is the only way the
 * SDM blesses for tracking IPI delivery from the source side. */
static void
lapic_ipi_wait(void)
{
	while (LAPIC_REG32(LAPIC_ICR) & LAPIC_ICR_DS_PENDING)
		;
}

void
lapic_enable(void)
{
	unsigned int svr;

	if (lapic_start == 0) {
		printf("lapic_enable: LAPIC not mapped yet, skipping\n");
		return;
	}

	/*
	 * Accept every priority level — TPR=0 means no class-based masking,
	 * which is what the kernel wants while it is the only consumer.
	 */
	LAPIC_REG32(LAPIC_TPR) = 0;

	/*
	 * Spurious interrupt vector: set the enable bit and pin the vector
	 * to LAPIC_SPURIOUS_VECTOR.  Without the enable bit the LAPIC stays
	 * software-disabled and IPIs are simply dropped.
	 */
	svr  = LAPIC_REG32(LAPIC_SVR);
	svr &= ~LAPIC_SVR_MASK;
	svr |= LAPIC_SVR_ENABLE | (LAPIC_SPURIOUS_VECTOR & LAPIC_SVR_MASK);
	LAPIC_REG32(LAPIC_SVR) = svr;
}

void
lapic_eoi(void)
{
	if (lapic_start == 0)
		return;
	LAPIC_REG32(LAPIC_EOI) = 0;
}

void
lapic_send_ipi(int slot, unsigned int vector)
{
	unsigned char lapic_dest;

	if (lapic_start == 0)
		return;

	lapic_dest = mp_cpu_lapic_id_get(slot);
	if (lapic_dest == 0xFF)
		return;		/* slot unknown — silent, callers check ncpus */

	lapic_ipi_wait();
	LAPIC_REG32(LAPIC_ICRD) =
	    ((unsigned int)lapic_dest & 0xFFu) << LAPIC_ICRD_DEST_SHIFT;
	LAPIC_REG32(LAPIC_ICR)  =
	    LAPIC_ICR_DM_FIXED
	    | LAPIC_ICR_LEVEL_ASSERT
	    | LAPIC_ICR_DSS_DEST
	    | (vector & LAPIC_ICR_VECTOR_MASK);
	lapic_ipi_wait();
}

void
lapic_send_ipi_all_excluding_self(unsigned int vector)
{
	if (lapic_start == 0)
		return;

	lapic_ipi_wait();
	LAPIC_REG32(LAPIC_ICR) =
	    LAPIC_ICR_DM_FIXED
	    | LAPIC_ICR_LEVEL_ASSERT
	    | LAPIC_ICR_DSS_OTHERS
	    | (vector & LAPIC_ICR_VECTOR_MASK);
	lapic_ipi_wait();
}

/*
 * IPI handlers — stubs for #302 increment 2.  Real reschedule / TLB
 * shootdown / cross-CPU call semantics land in their own sub-issues
 * (TLB shootdown is #304).  Until then, prove the path end-to-end by
 * just printing on receipt and acking via lapic_eoi().
 *
 * These run with interrupts disabled (K_INTR_GATE in the IDT) and on
 * the stack the LAPIC interrupted — no kmsg pool / no preemption / no
 * sleeping, exactly like any other hardware interrupt handler.
 */
void
ipi_resched_handler(void)
{
	printf("IPI: resched on cpu %d\n", current_cpu_id());
	lapic_eoi();
}

/*
 * #304: dispatch to the legacy OSF pmap_update_interrupt() machinery.
 * It expects to be entered at splip (the LAPIC IDT_ENTRY uses
 * K_INTR_GATE which already cleared IF for us), walks
 * cpu_update_list[cpu_number()], runs each requested INVALIDATE_TLB,
 * and clears cpu_update_needed[].  Acknowledging via EOI is our job —
 * pmap_update_interrupt was written before the LAPIC era and has no
 * idea EOI even exists.
 */
extern void pmap_update_interrupt(void);

void
ipi_tlb_shoot_handler(void)
{
	pmap_update_interrupt();
	lapic_eoi();
}

void
ipi_call_func_handler(void)
{
	printf("IPI: call-func on cpu %d\n", current_cpu_id());
	lapic_eoi();
}

#endif	/* NCPUS > 1 */
