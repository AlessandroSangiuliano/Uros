/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <machine/cpu_number.h> for x86-64 (#450).
 *
 * Which processor is running this code.  Eighty-one of the machine-
 * independent sources stop on this one header -- more than any other single
 * name -- because kern/cpu_number.h defers to the machine as soon as the
 * kernel is built for more than one CPU, and everything that takes a lock or
 * touches per-CPU state goes through it.
 *
 * The id is read straight out of this processor's own per-CPU block through
 * %gs, so the answer does not depend on a lock, an index, or on the caller
 * being pinned: the segment base is what makes it this processor's.  One
 * instruction, no memory the other processors can touch.
 *
 * i386 reads the same thing from %gs at a hard-coded offset (12).  Here the
 * offset comes from PERCPU_CPU_ID in <cpu/percpu.h>, which is also what the
 * assembly entry paths use, so the struct and the reads cannot drift apart.
 *
 * ⚠️ Valid only after percpu_activate() has run on this processor.  Before
 * that %gs's base is zero, and early in boot address zero is mapped, so a
 * premature call returns a plausible small number instead of faulting.
 */

#ifndef _X86_64_CPU_NUMBER_H_
#define _X86_64_CPU_NUMBER_H_

#include <cpu/percpu.h>

#ifndef __ASSEMBLER__

static __inline__ int cpu_number(void)
{
	int cpu;

	__asm__ volatile("movl %%gs:%c1, %0"
			 : "=r"(cpu)
			 : "i"(PERCPU_CPU_ID));
	return cpu;
}

#endif	/* __ASSEMBLER__ */

#endif /* _X86_64_CPU_NUMBER_H_ */
