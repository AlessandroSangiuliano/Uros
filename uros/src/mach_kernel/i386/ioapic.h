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
 * ioapic.h — modern I/O APIC interrupt routing for SMP (#311).
 *
 * Replaces the global 8259 PIC as the device-IRQ source on SMP.  The 8259
 * is a single global device whose mask is programmed by the BSP only, but
 * spl is per-CPU — so a BSP sitting at a high spl masks device IRQs for the
 * whole machine and starves the disk (the #304 boot hang).  Routing IRQs
 * through the I/O APIC to LAPIC vectors, with per-CPU masking via the LAPIC
 * task-priority register (TPR), removes that structural coupling.
 *
 * Increment 1 (this file): bring up I/O APIC #0, route the legacy ISA IRQs
 * (vector 0x40 + irq, reusing the existing ivect[]/intpri[]/iunit[]
 * dispatch in interrupt.S) to the boot CPU, mask the 8259, and arm the
 * `ioapic_enabled` gate that switches spl.S / interrupt.S onto the LAPIC
 * TPR + LAPIC EOI paths.
 *
 * This is deliberately NOT the historical mp_v1_1.c machinery (the 0x50-
 * based priority-encoded vector scheme + mp_v1_1_intr[] tables): that code
 * is the 90's-era layer slated for removal in the post-SMP kernel rework.
 */

#ifndef _I386_IOAPIC_H_
#define _I386_IOAPIC_H_

#include <cpus.h>

#if	NCPUS > 1

#include <mach/boolean.h>

/*
 * Set TRUE by ioapic_init() once I/O APIC #0 is mapped, the redirection
 * table is programmed and the 8259 is masked.  Read from spl.S and
 * interrupt.S to select the LAPIC TPR / LAPIC EOI paths over the legacy
 * 8259 ones.  Stays FALSE on UP and on SMP machines with no usable I/O
 * APIC, so the kernel transparently falls back to the 8259.
 */
extern int	ioapic_enabled;

/*
 * Bring up I/O APIC #0: map its MMIO window, program one redirection-table
 * entry per legacy ISA IRQ (masked, vector 0x40+irq, delivered to the boot
 * CPU, trigger/polarity taken from the 8259 ELCR), mask the 8259, then set
 * ioapic_enabled.  Idempotent.  Must run with kernel_map alive and the BSP
 * LAPIC already enabled (lapic_enable()).
 */
extern void	ioapic_init(void);

/*
 * Per-IRQ redirection-table mask / unmask.  These are the I/O APIC analogue
 * of the 8259 pic_irq_mask()/pic_irq_unmask() flow-control used by the IRQ
 * forwarding path (device_master.c) and by take_irq()/reset_irq().  No-ops
 * if ioapic_enabled is FALSE so callers need not branch.
 */
extern void	ioapic_mask_irq(unsigned int irq);
extern void	ioapic_unmask_irq(unsigned int irq);

/*
 * #381: trigger-mode query for the IRQ-forward flow control.  TRUE when the
 * I/O APIC owns delivery AND the ELCR marks the line level-triggered (safe
 * to mask/unmask per event: a still-asserted line re-fires on unmask).
 * FALSE for edge lines — the I/O APIC does not latch fronts that arrive
 * while the RTE is masked (the 8259 did, in its IRR), so masking an edge
 * line can lose the front for good; a device that keeps INT asserted
 * afterwards (16550 with a full RX FIFO) never fires again.
 */
extern boolean_t	ioapic_irq_is_level(unsigned int irq);
extern boolean_t	ioapic_active(void);

#endif	/* NCPUS > 1 */

#endif	/* _I386_IOAPIC_H_ */
