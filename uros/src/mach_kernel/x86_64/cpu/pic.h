/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The legacy interrupt controller, silenced (#438).
 *
 * Nothing here wants the 8259: the local APIC and the I/O APIC replace it,
 * and this kernel will never route a device through it.  It exists in this
 * tree for one reason — the firmware leaves it enabled, and where it leaves
 * it pointing is on top of the exception vectors.
 */

#ifndef _X86_64_CPU_PIC_H_
#define _X86_64_CPU_PIC_H_

/*
 * Move it out of the way and mask every line.
 *
 * Must run before interrupts are enabled for the first time.  Out of the
 * box the two controllers deliver their sixteen lines at vectors 0x08-0x0F
 * and 0x70-0x77, and the first of those ranges is the exceptions: the
 * periodic timer the firmware left running would arrive as vector 8, which
 * is the double fault — reported, believed, and halted on, for an interrupt
 * that was working exactly as configured.
 *
 * Masking alone would not be enough to make that safe.  A line that drops
 * before it is acknowledged still produces a spurious interrupt, and a
 * spurious interrupt from a controller that is masked still arrives at
 * whatever vector the controller was told to use.  So the move comes first
 * and the mask second, and the vectors it is moved to are the ones the
 * table in <trap/trap.h> reserves for it.
 */
void pic_disable(void);

/* Where its lines would arrive, if anything ever unmasked one. */
#define PIC_VECTOR_BASE		0x20

#endif	/* _X86_64_CPU_PIC_H_ */
