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
 * lapic.h — local APIC primitives for SMP (#302).
 *
 * The base address of the LAPIC MMIO window is io_map()'d by mp_table.c
 * once kernel_map is alive (see mp_v1_1_init phase 2) and exported as
 * `lapic_start`.  Until that mapping is in place no LAPIC access is safe:
 * all entry points below assert lapic_start != 0 before touching MMIO.
 */

#ifndef _I386_LAPIC_H_
#define _I386_LAPIC_H_

#include <mach/vm_param.h>	/* vm_offset_t */
#include <i386/apic.h>		/* LAPIC_* register / bit defines */

#if	NCPUS > 1

extern vm_offset_t lapic_start;	/* set by mp_table.c via io_map() */

/* Single accessor used by every LAPIC consumer: 32-bit MMIO at lapic_start+off. */
#define LAPIC_REG32(off)	(*(volatile unsigned int *)(lapic_start + (off)))

/*
 * IPI vectors reserved for the kernel.  Numbers picked above the legacy
 * PIC range and well below the LAPIC error / spurious slots so they
 * coexist with the rest of the interrupt map.
 *
 * #316: cpu_interrupt() (mp_stub.c) sends a single coalescing IPI on
 * IPI_VECTOR_MP whenever a producer sets a bit in cpu_int_word[cpu]
 * (MP_TLB_FLUSH / MP_AST / MP_CLOCK / MP_KDB).  ipi_mp_handler() drains
 * every pending bit and dispatches it; the asm stub (ipi.S) then re-enters
 * the shared all_intrs AST/preempt epilogue so a cross-CPU reschedule
 * actually happens on return.  RESCHED / CALL_FUNC remain distinct vectors
 * for the bring-up self-test and possible directed sends.
 */
#define IPI_VECTOR_RESCHED	0xF0
#define IPI_VECTOR_MP		0xF1
#define IPI_VECTOR_CALL_FUNC	0xF2

#define LAPIC_SPURIOUS_VECTOR	0xFF

extern void	lapic_enable(void);
extern void	lapic_eoi(void);
extern void	lapic_send_ipi(int slot, unsigned int vector);
extern void	lapic_send_ipi_all_excluding_self(unsigned int vector);

/* #312: per-CPU LAPIC timer.  lapic_timer_calibrate() measures the local
 * timer against the 8254 RTC and stores the per-HZ-tick INITIAL_COUNT (at
 * divide-by-16) in lapic_timer_count. */
extern void		lapic_timer_calibrate(void);
extern unsigned int	lapic_timer_count;

#endif	/* NCPUS > 1 */

#endif	/* _I386_LAPIC_H_ */
