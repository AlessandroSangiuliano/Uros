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
 * ioapic.c — I/O APIC bring-up + redirection-table management (#311).
 *
 * Public API + rationale in <i386/ioapic.h>.  Device IRQs are routed
 * vector 0x40+irq to the boot CPU's local APIC, reusing the existing
 * ivect[]/intpri[]/iunit[] dispatch in interrupt.S; per-CPU masking is the
 * LAPIC TPR (spl.S).  The 8259 is masked off once we take over.
 */

#include <cpus.h>

#if	NCPUS > 1

#include <types.h>
#include <mach/boolean.h>
#include <i386/ioapic.h>
#include <i386/apic.h>			/* IOAPIC_* / IOA_R_* / LAPIC_TPR */
#include <i386/lapic.h>			/* lapic_start, LAPIC_REG32 */
#include <i386/pio.h>			/* inb / outb */
#include <i386/pic.h>			/* master_ocw / slaves_ocw */
#include <i386/io_map_entries.h>	/* io_map */
#include <kern/misc_protos.h>		/* printf */
#include <kern/cpu_number.h>		/* cpu_number, #599 counts */
#include <i386/cn_nolock.h>		/* #599: said from interrupt context */

extern unsigned int	mp_ioapic_phys_get(int idx);
extern int		mp_ioapic_count_get(void);
extern unsigned char	mp_bsp_lapic_id_get(void);

/* 8259 OCW1 (mask) ports, set up by picinit() in pic.c. */
extern i386_ioport_t	master_ocw, slaves_ocw;

/*
 * Legacy ISA IRQ count.  We program one redirection-table entry per IRQ,
 * mapping GSI n -> vector 0x40+n (the PICM_VECTBASE the 8259 used, so the
 * interrupt.S dispatch is unchanged).  ISA interrupt-source overrides
 * (e.g. IRQ0 -> GSI2 on QEMU) are not applied: this kernel is event-driven
 * and never arms the PIT, so only the device lines (kbd/com/AHCI/mouse,
 * GSI == IRQ on every machine we target) matter.
 */
#define IOAPIC_ISA_IRQS		16

/* Vector base must match PICM_VECTBASE / the 0x40 dispatch in interrupt.S. */
#define IOAPIC_VECTOR_BASE	0x40

/*
 * 8259 Edge/Level Control Register.  The firmware sets one bit per IRQ:
 * 1 = level-triggered (PCI, active-low), 0 = edge-triggered (ISA, active-
 * high).  Reading it lets us program the I/O APIC RTE trigger/polarity
 * correctly without an AML interpreter.
 */
#define ELCR_PORT_LO		0x4D0	/* IRQ 0-7  */
#define ELCR_PORT_HI		0x4D1	/* IRQ 8-15 */

int		ioapic_enabled;		/* read from spl.S / interrupt.S */

static vm_offset_t	ioapic_base;	/* MMIO virtual base of I/O APIC #0 */
static unsigned int	ioapic_redirs;	/* number of redirection entries */
static unsigned char	ioapic_dest;	/* boot CPU physical APIC ID */

#ifdef ABLATE_599_WIDEN
/*
 * #599 ablation: ABLATE_599_WIDEN reads of port 0x80 (each a VM exit under
 * KVM) in each gap of the pair -- after a select, between a read and the
 * select for the write -- on the other processors only.  Processor 0 keeps
 * its own read-modify-writes, which are the tick's defer and replay, as
 * short as they are.  If the pair's race is what stops the tick, this makes
 * it frequent at four processors and leaves it absent at one.
 */
static void
ioapic_widen(void)
{
	static int	said;
	int		w;

	if (cpu_number() == master_cpu)
		return;
	if (!said) {
		said = 1;
		cn_puts("\nioapic: #599 ablation -- the other processors wait in "
			"each gap of the select/window pair\n");
	}
	for (w = 0; w < ABLATE_599_WIDEN; w++)
		(void) inb(0x80);
}
#else
#define	ioapic_widen()
#endif

/*
 * Indexed register access: write the register number to RSELECT, then read
 * or write the value through RWINDOW.
 */
static unsigned int
ioapic_read(unsigned int reg)
{
	*(volatile unsigned int *)(ioapic_base + IOAPIC_RSELECT) = reg;
	ioapic_widen();
	return *(volatile unsigned int *)(ioapic_base + IOAPIC_RWINDOW);
}

static void
ioapic_write(unsigned int reg, unsigned int value)
{
	*(volatile unsigned int *)(ioapic_base + IOAPIC_RSELECT) = reg;
	ioapic_widen();
	*(volatile unsigned int *)(ioapic_base + IOAPIC_RWINDOW) = value;
}

/*
 * #599: how often the select/window pair is used for a read-modify-write, on
 * which processor, and how often one started while another was still
 * inside.  Nothing in this file serialises the pair, and that is #599's
 * first hypothesis for processor 0's stopped tick: an interleaving can leave
 * one pin holding another pin's entry.  An overlap is the precondition, not
 * the corruption -- which entry ends where depends on how the two sequences
 * interleave -- so it is counted in every boot, where a stop is too rare to
 * wait for.  The first overlap of a boot is also said, without a lock.  Read
 * by the clock watch's line and scripts/i386-clock-snapshot.py.
 */
unsigned int		ioapic_rmw_count[NCPUS];
volatile unsigned int	ioapic_inside;
volatile unsigned int	ioapic_overlaps;

/*
 * 🔴 THE PAIR IS ONE FOR THE WHOLE MACHINE, AND A READ-MODIFY-WRITE SELECTS
 * TWICE (#599).  Two processors interleaving inside it each read or write the
 * other's pin.  The widening arm (UROS_ABLATE_599_WIDEN) showed both ways it
 * ends: pin 2 -- the 8254 -- holding pin 11's entry, so hardclock never ran
 * again while everything else did; and AHCI's pin holding line 5's entry,
 * whose vector had no handler and stopped the tick through intnull's printf.
 * Processor 0 masks and unmasks from interrupt context (the deferral and its
 * replay), and the others from thread context (device_intr_enable after every
 * AHCI interrupt), so the race needs nothing unusual to happen.
 *
 * So every access sequence -- a read-modify-write, the two writes of an entry
 * -- holds ioapic_pair_lock, with interrupts off, the pattern #597 gave the
 * PCI configuration pair (i386/pci/pcibios.c):
 *
 *   - a leaf: nothing is taken or waited on while it is held, so it cannot
 *     be part of a cycle;
 *   - interrupts off for the hold, so an interrupt on the same processor
 *     cannot start a sequence inside one (the deferral path masks from
 *     interrupt context).  Every caller already had them off; this does not
 *     rely on it.
 *
 * ioapic_lock_waits counts how often the lock was found taken: a lock nobody
 * ever waits on is one whose absence could not have been noticed either.
 * The overlap count above is taken inside the lock, where it can only stay 0.
 */
static volatile unsigned char	ioapic_pair_lock;
volatile unsigned int		ioapic_lock_waits;

static unsigned int
ioapic_pair_enter(void)
{
	unsigned int	flags;
	unsigned char	busy;
	int		waited = 0;

	__asm__ volatile("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
#ifndef ABLATE_599_NO_LOCK
	for (;;) {
		busy = 1;
		__asm__ volatile("xchgb %0, %1"
				 : "+q" (busy), "+m" (ioapic_pair_lock)
				 : : "memory");
		if (busy == 0)
			break;
		waited = 1;
		__asm__ volatile("pause");
	}
	if (waited)
		__sync_fetch_and_add(&ioapic_lock_waits, 1);
#endif
	return flags;
}

static void
ioapic_pair_leave(unsigned int flags)
{
#ifndef ABLATE_599_NO_LOCK
	__asm__ volatile("" : : : "memory");
	ioapic_pair_lock = 0;
#endif
	__asm__ volatile("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
}

static __inline__ int
ioapic_enter(void)
{
	ioapic_rmw_count[cpu_number()]++;
	if (__sync_fetch_and_add(&ioapic_inside, 1) == 0)
		return 0;
	return __sync_fetch_and_add(&ioapic_overlaps, 1) == 0;
}

static void
ioapic_leave(int first, unsigned int gsi)
{
	__sync_fetch_and_sub(&ioapic_inside, 1);
	if (!first)
		return;
	cn_puts("\nioapic: a read-modify-write of pin ");
	cn_dec(gsi);
	cn_puts(" on processor ");
	cn_dec((unsigned int)cpu_number());
	cn_puts(" started while another was inside the select/window pair "
		"(#599); the next ones are counted, not said\n");
}

/*
 * Write a redirection-table entry.  `low` carries vector/trigger/polarity/
 * mask; the high dword carries the physical destination APIC ID.  The high
 * word is written first while the entry is (or is about to be) masked, per
 * the usual rule, then the low word commits it.
 */
static void
ioapic_write_rte(unsigned int irq, unsigned int low)
{
	unsigned int reg = IOA_R_REDIRECTION + 2 * irq;
	unsigned int flags;

	flags = ioapic_pair_enter();
	ioapic_write(reg + 1, (unsigned int)ioapic_dest << 24);
	ioapic_write(reg, low);
	ioapic_pair_leave(flags);
}

/*
 * ISA IRQ -> I/O APIC GSI (input pin).
 *
 * On every PC platform the ACPI MADT remaps the 8254 PIT (ISA IRQ 0) onto
 * GSI 2 via an Interrupt Source Override; the other ISA lines are identity.
 * Getting this right matters: hardclock() is wired to IRQ 0 (rtclock.c) and
 * drives the timeout wheel, so if the PIT is left on GSI 0 it never fires
 * and every MACH_RCV_TIMEOUT hangs forever (e.g. block_device_server's HAL
 * replay drain) -- the SMP boot then wedges right after reading the MBR.
 *
 * TODO: parse the MADT Interrupt Source Override entries instead of
 * hardcoding the (universal) IRQ0->GSI2 timer override.
 */
static unsigned int
gsi_for_irq(unsigned int irq)
{
	return (irq == 0) ? 2u : irq;
}

void
ioapic_mask_irq(unsigned int irq)
{
	unsigned int reg, low, gsi, flags;
	int first;

	if (!ioapic_enabled || irq >= IOAPIC_ISA_IRQS)
		return;
	gsi = gsi_for_irq(irq);
	if (gsi >= ioapic_redirs)
		return;
	reg = IOA_R_REDIRECTION + 2 * gsi;
	flags = ioapic_pair_enter();
	first = ioapic_enter();
	low = ioapic_read(reg);
	ioapic_widen();
	ioapic_write(reg, low | IOA_R_R_MASKED);
	ioapic_leave(first, gsi);
	ioapic_pair_leave(flags);
}

void
ioapic_unmask_irq(unsigned int irq)
{
	unsigned int reg, low, gsi, flags;
	int first;

	if (!ioapic_enabled || irq >= IOAPIC_ISA_IRQS)
		return;
	gsi = gsi_for_irq(irq);
	if (gsi >= ioapic_redirs)
		return;
	reg = IOA_R_REDIRECTION + 2 * gsi;
	flags = ioapic_pair_enter();
	first = ioapic_enter();
	low = ioapic_read(reg);
	ioapic_widen();
	ioapic_write(reg, low & ~IOA_R_R_MASKED);
	ioapic_leave(first, gsi);
	ioapic_pair_leave(flags);
}

/*
 * #381: ELCR snapshot taken at ioapic_init(), so the IRQ-forward path can
 * tell level-triggered lines (bit set: mask/unmask flow-control is safe —
 * a still-asserted line re-fires on unmask) from edge-triggered ones (bit
 * clear: an edge arriving while the RTE is masked is LOST forever; the
 * 8259 latched those in its IRR, the I/O APIC does not).
 */
static unsigned int	ioapic_elcr;

boolean_t
ioapic_irq_is_level(unsigned int irq)
{
	if (!ioapic_enabled || irq >= IOAPIC_ISA_IRQS)
		return FALSE;
	return (ioapic_elcr & (1u << irq)) != 0;
}

boolean_t
ioapic_active(void)
{
	return ioapic_enabled;
}

void
ioapic_init(void)
{
	unsigned int	phys;
	unsigned int	version;
	unsigned int	elcr;
	unsigned int	irq;

	if (ioapic_enabled)
		return;			/* idempotent */

	if (mp_ioapic_count_get() < 1) {
		printf("ioapic: no I/O APIC found, staying on 8259\n");
		return;
	}
	if (lapic_start == 0) {
		printf("ioapic: LAPIC not mapped, staying on 8259\n");
		return;
	}

	phys = mp_ioapic_phys_get(0);
	if (phys == 0)
		phys = IOAPIC_START;
	ioapic_base = io_map(phys, IOAPIC_SIZE);
	ioapic_dest = mp_bsp_lapic_id_get();

	/* Max redirection entry is in version reg bits 16-23 (count = max+1). */
	{
		unsigned int	flags = ioapic_pair_enter();

		version = ioapic_read(IOA_R_VERSION);
		ioapic_pair_leave(flags);
	}
	ioapic_redirs = ((version >> IOA_R_VERSION_ME_SHIFT) &
			 IOA_R_VERSION_ME_MASK) + 1;
	if (ioapic_redirs > IOAPIC_ISA_IRQS)
		ioapic_redirs = IOAPIC_ISA_IRQS;

	/* Trigger/polarity straight from the 8259 ELCR (see header). */
	elcr = inb(ELCR_PORT_LO) | (inb(ELCR_PORT_HI) << 8);
	ioapic_elcr = elcr;		/* #381: kept for ioapic_irq_is_level() */

	/*
	 * Program every entry masked.  take_irq()/device_intr_register unmask
	 * the lines that get a real handler; until then a still-asserted line
	 * cannot storm us.
	 *
	 * Step 1: a benign masked placeholder on every pin.
	 * Step 2: for each ISA IRQ, drive its mapped GSI (gsi_for_irq) with
	 * that IRQ's vector (0x40+irq), so e.g. the PIT (IRQ0->GSI2) delivers
	 * vector 0x40 and the existing ivect[0]=hardclock dispatch runs.  The
	 * 8259 cascade (IRQ2) is not a device line under the I/O APIC, so it
	 * gets no entry.
	 */
	for (irq = 0; irq < ioapic_redirs; irq++)
		ioapic_write_rte(irq, (IOAPIC_VECTOR_BASE + irq)
				 | IOA_R_R_DM_FIXED | IOA_R_R_MASKED);

	for (irq = 0; irq < IOAPIC_ISA_IRQS; irq++) {
		unsigned int gsi, low;

		if (irq == 2)
			continue;		/* 8259 cascade */
		gsi = gsi_for_irq(irq);
		if (gsi >= ioapic_redirs)
			continue;

		low  = (IOAPIC_VECTOR_BASE + irq) & IOA_R_R_VECTOR_MASK;
		low |= IOA_R_R_DM_FIXED;	/* physical destination */
		if (elcr & (1u << irq))
			low |= IOA_R_R_TM_LEVEL | IOA_R_R_IP_PLRITY_LOW;
		low |= IOA_R_R_MASKED;
		ioapic_write_rte(gsi, low);
	}

	/*
	 * Take the 8259 out of the picture: mask both halves and silence the
	 * LAPIC's virtual-wire LINT0 so nothing double-delivers alongside the
	 * I/O APIC.
	 */
	outb(master_ocw, 0xFF);
	outb(slaves_ocw, 0xFF);
	LAPIC_REG32(LAPIC_LVT_LINT0) = LAPIC_LVT_MASKED;

	/*
	 * Seed the per-CPU TPR write-skip cache to -1 so the first set_spl on
	 * every CPU programs the LAPIC TPR for real (its current value is 0
	 * from lapic_enable; masking is otherwise inert because every RTE is
	 * still masked until take_irq unmasks a handled line).
	 */
	{
		extern int lapic_tpr_cache[NCPUS];
		int i;
		for (i = 0; i < NCPUS; i++)
			lapic_tpr_cache[i] = -1;
	}

	ioapic_enabled = 1;

	printf("ioapic: I/O APIC #0 @0x%x, %u entries, dest lapic %u, "
	       "elcr=0x%x — 8259 masked, TPR routing armed\n",
	       phys, ioapic_redirs, (unsigned)ioapic_dest, elcr);
}

#endif	/* NCPUS > 1 */
