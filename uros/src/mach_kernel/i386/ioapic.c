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

/*
 * Indexed register access: write the register number to RSELECT, then read
 * or write the value through RWINDOW.
 */
static unsigned int
ioapic_read(unsigned int reg)
{
	*(volatile unsigned int *)(ioapic_base + IOAPIC_RSELECT) = reg;
	return *(volatile unsigned int *)(ioapic_base + IOAPIC_RWINDOW);
}

static void
ioapic_write(unsigned int reg, unsigned int value)
{
	*(volatile unsigned int *)(ioapic_base + IOAPIC_RSELECT) = reg;
	*(volatile unsigned int *)(ioapic_base + IOAPIC_RWINDOW) = value;
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

	ioapic_write(reg + 1, (unsigned int)ioapic_dest << 24);
	ioapic_write(reg, low);
}

void
ioapic_mask_irq(unsigned int irq)
{
	unsigned int reg, low;

	if (!ioapic_enabled || irq >= ioapic_redirs)
		return;
	reg = IOA_R_REDIRECTION + 2 * irq;
	low = ioapic_read(reg);
	ioapic_write(reg, low | IOA_R_R_MASKED);
}

void
ioapic_unmask_irq(unsigned int irq)
{
	unsigned int reg, low;

	if (!ioapic_enabled || irq >= ioapic_redirs)
		return;
	reg = IOA_R_REDIRECTION + 2 * irq;
	low = ioapic_read(reg);
	ioapic_write(reg, low & ~IOA_R_R_MASKED);
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
	version = ioapic_read(IOA_R_VERSION);
	ioapic_redirs = ((version >> IOA_R_VERSION_ME_SHIFT) &
			 IOA_R_VERSION_ME_MASK) + 1;
	if (ioapic_redirs > IOAPIC_ISA_IRQS)
		ioapic_redirs = IOAPIC_ISA_IRQS;

	/* Trigger/polarity straight from the 8259 ELCR (see header). */
	elcr = inb(ELCR_PORT_LO) | (inb(ELCR_PORT_HI) << 8);

	/*
	 * Program every entry masked.  take_irq()/device_intr_register unmask
	 * the lines that get a real handler; until then a still-asserted line
	 * cannot storm us.
	 */
	for (irq = 0; irq < ioapic_redirs; irq++) {
		unsigned int low;

		low  = (IOAPIC_VECTOR_BASE + irq) & IOA_R_R_VECTOR_MASK;
		low |= IOA_R_R_DM_FIXED;	/* physical destination */
		if (elcr & (1u << irq))
			low |= IOA_R_R_TM_LEVEL | IOA_R_R_IP_PLRITY_LOW;
		low |= IOA_R_R_MASKED;
		ioapic_write_rte(irq, low);
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
