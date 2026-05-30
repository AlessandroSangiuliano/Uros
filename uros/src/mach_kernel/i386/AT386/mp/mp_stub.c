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
 * cpu_start(), cpu_control() and mp_probe_cpus() likewise live there. */

/*
 * get_ncpus() — called by mp.c::mp_probe_cpus() to ask the platform how
 * many CPUs it found.  Stub answers 1.
 */
int
get_ncpus(void)
{
	return 1;
}

/*
 * validate_cpus() — marks the first `n` machine_slot[] entries as CPUs.
 * mp.c calls this from the MP_V1_1 branch of mp_probe_cpus(); we keep it
 * truthful for n==1 and ignore higher counts (no AP support yet).
 */
void
validate_cpus(int ncpus)
{
	(void)ncpus;
	machine_slot[0].is_cpu = TRUE;
}

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
 * start_other_cpus() — called from kern/startup.c on the BSP to bring all
 * APs into the scheduler.  Stub does nothing; #300 Incrementi 3-4 wire
 * this up to the real INIT/SIPI sequence.
 */
void
start_other_cpus(void)
{
}

/*
 * slave_machine_init() — entry point each AP jumps to once protected mode
 * and the kernel page tables are live.  Never reached as long as
 * start_other_cpus() above is a stub.
 */
void
slave_machine_init(void)
{
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

/*
 * mp_v1_1_init() — bring the historical MPS 1.x machinery up.  Stub does
 * nothing; replaced by the modern MP table parser in Increment 3.
 */
boolean_t	mp_v1_1_initialized = FALSE;

void
mp_v1_1_init(void)
{
}

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
