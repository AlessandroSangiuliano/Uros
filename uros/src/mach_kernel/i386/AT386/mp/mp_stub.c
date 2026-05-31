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
 * mp_stub.c — minimal symbols required to link the kernel with NCPUS > 1
 * before the real AP-boot machinery (#300 Incrementi 3-4) lands.
 *
 * The historical mp_v1_1.c is intentionally kept out of the build: it carries
 * 1230 lines of Corollary/SystemPro-specific interrupt plumbing that was
 * never exercised in this tree.  It will be replaced by a modern MPS 1.4
 * parser in Increment 3.  Until then, this file provides the externs the
 * rest of the kernel expects.  The runtime behaviour is "single CPU": no AP
 * is woken, cpu_start() refuses, mp_probe_cpus() reports a single processor.
 */

#include <types.h>
#include <mach/machine.h>
#include <mach/kern_return.h>
#include <kern/processor.h>
#include <kern/misc_protos.h>
#include <kern/cpu_data.h>	/* current_cpu_id() (#301) */
#include <i386/lapic.h>		/* lapic_enable / lapic_send_ipi (#302) */
#include <i386/pic.h>		/* NINTR */
#include <i386/ipl.h>		/* SPLHI */
#include <chips/busses.h>	/* intr_t */

#define NSPL	(SPLHI + 1)

/* Local APIC bookkeeping.
 *
 * cpu_number() in <i386/cpu_number.h> reads `lapic_id` to extract its CPU
 * index from the local APIC ID register, so the symbol must exist even
 * before the LAPIC machinery is wired up.  We point it at a benign zeroed
 * word in low memory; reads return CPU 0, which is correct while only the
 * BSP is alive. */
unsigned int	lapic_id_initdata = 0;
int		lapic_id = (int)&lapic_id_initdata;
vm_offset_t	lapic_start;

/* cpu_int_word[NCPUS], real_ncpus and wncpu are already defined by mp.c;
 * cpu_start(), cpu_control() and mp_probe_cpus() likewise live there.
 *
 * get_ncpus() and validate_cpus() live in mp_table.c (Increment 3): they
 * walk the MPS 1.4 FPS/PCMP to enumerate real processors instead of always
 * answering 1. */

/*
 * cpu_interrupt() — send an IPI to `cpu` (typically using cpu_int_word
 * to encode the reason).  With no APs awake this is a no-op.  Replaced
 * by the real LAPIC ICR-driven implementation in #302.
 */
void
cpu_interrupt(int cpu)
{
	(void)cpu;
}

/*
 * start_other_cpus() — called from kern/startup.c on the BSP after the
 * scheduler is up.  Walks every slot the MP table identified as a CPU,
 * skips the BSP itself, and asks cpu_start() (in mp.c) to bring each AP
 * online via the INIT/SIPI/SIPI sequence.
 *
 * #300 Increment 4.  Errors are logged but not fatal: a failed AP just
 * stays offline; the rest of the system keeps running on the CPUs that
 * did make it.
 */
extern int master_cpu;

void
start_other_cpus(void)
{
	/* #301: BSP self-reports its per-CPU identity via %gs. */
	printf("smp: BSP cpu_id=%d via %%gs\n", current_cpu_id());

	/* #302 acceptance (reduced): self-IPI round-trip on the BSP.
	 * Validates that lapic_enable() worked, the ICR write is honoured,
	 * the IDT vector dispatches through ipi_call_func_entry, and the
	 * handler EOI's.  The full BSP→AP path is deferred to #303
	 * (spinlock audit): cpu_start()'s INIT/SIPI/SIPI fires correctly,
	 * the AP reaches slave_machine_init, but the busy-wait on
	 * machine_slot[slot].running observed from the BSP never sees the
	 * AP's update — strong indication of a memory-barrier / lock
	 * ordering issue that #303 will untangle.  All the per-CPU plumbing
	 * (LAPIC enable on AP, cpu_id %gs accessor, IPI vectors with EOI)
	 * stays in place so #303 can flip on cpu_start() with no further
	 * changes here. */
	lapic_send_ipi(master_cpu, IPI_VECTOR_CALL_FUNC);
}

/*
 * slave_machine_init() — entry point each AP jumps to once protected mode
 * and the kernel page tables are live.
 */
void
slave_machine_init(void)
{
	int my_cpu = current_cpu_id();

	/* #302: enable this AP's local APIC.  Without this, IPIs and
	 * eventually LAPIC timer interrupts are silently dropped. */
	lapic_enable();

	/* Tell the BSP this AP is alive.  cpu_start() polls
	 * machine_slot[slot].running and gives up after ~1 s otherwise. */
	machine_slot[my_cpu].running = TRUE;

	/* The AP printf intentionally lives on the BSP side (see
	 * start_other_cpus) instead of here: the console driver isn't
	 * thread-safe across two CPUs yet — that gets fixed by #303
	 * (spinlock audit).  Until then, the BSP reads machine_slot
	 * after cpu_start() returns and prints on the AP's behalf. */
}

/*
 * mp_v1_1_io_lock() / mp_v1_1_io_unlock() — historical big-MP IO barrier
 * used by net_device.c et al.  Stub returns "lock acquired" so existing
 * call sites proceed; with no APs the barrier is a no-op.  The whole
 * mechanism gets reviewed/replaced by #303 (spinlock audit).
 */
boolean_t
mp_v1_1_io_lock(int why, struct processor **saved_processor_p)
{
	(void)why;
	if (saved_processor_p)
		*saved_processor_p = (struct processor *)0;
	return TRUE;
}

void
mp_v1_1_io_unlock(struct processor *p)
{
	(void)p;
}

/* mp_v1_1_init() lives in mp_table.c (Increment 3); the global
 * mp_v1_1_initialized flag is still owned by this file because mp.c and
 * a couple of legacy callers check it directly. */
boolean_t	mp_v1_1_initialized = FALSE;

/*
 * mp_v1_1_take_irq() / mp_v1_1_reset_irq() — install/remove a per-CPU IRQ
 * handler in the SMP interrupt vector tables.  Stubs always succeed; the
 * single-CPU build behaves as if there was no per-CPU IRQ routing — i.e.
 * the legacy PIC remains the single source of IRQs, which is exactly what
 * we want until #302 wires up the IOAPIC.
 */
boolean_t
mp_v1_1_take_irq(int pic, int unit, int spl, void *intr)
{
	(void)pic;
	(void)unit;
	(void)spl;
	(void)intr;
	return TRUE;
}

boolean_t
mp_v1_1_reset_irq(int pic, int *unit, int *spl, void *intr)
{
	(void)pic;
	(void)unit;
	(void)spl;
	(void)intr;
	return TRUE;
}

/*
 * mp_v1_1_intr[] / mp_v1_1_unit[] — per-IRQ handler/argument tables that
 * i386/interrupt.S dispatches through ((,%ecx,4)).  Sized like the
 * historical mp_v1_1.c so the asm indexing stays in-bounds; left zeroed so
 * any vector taken into them is rejected by the dispatcher.
 */
intr_t	mp_v1_1_intr[NSPL * NINTR];
int	mp_v1_1_unit[NSPL * NINTR];

/*
 * kdb_console() — switch DDB I/O to a shared console when running on a
 * second CPU.  Stub: no-op (BSP keeps the same console it had on UP).
 */
void
kdb_console(void)
{
}
