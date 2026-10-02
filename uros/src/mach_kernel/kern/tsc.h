/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The timestamp counter, read from machine-independent code (#537).
 *
 * One reader for every instrument above the machine layer.  The trap profile
 * (#411) and the fault profile (#482) each had a copy of it, identical, and
 * #319's lock counter a third without the fence; #537's count of what one
 * per-page DMA ask costs would have been a fourth.  A copy changed alone --
 * the fence taken out of one, a target added to another -- would make two
 * columns disagree about time itself.
 */

#ifndef	_KERN_TSC_H_
#define	_KERN_TSC_H_

#include <stdint.h>

/*
 * 🔴 NOT cpuid+rdtsc.  #439 measured a CPUID at 1,920 cycles against 1 for the
 * ordinary read: under a hypervisor it is an exit.  A Mach trap is a few
 * hundred cycles all told and a copy-on-write fault a few thousand, so the
 * textbook serialisation would cost several times the one subject and a third
 * of the other, charged to whichever phase was open.  lfence orders the
 * earlier loads for a handful of cycles.
 *
 * ⚠️ lfence is SSE2, which only x86-64 is guaranteed to have, so i386 reads the
 * counter unordered.  What that costs is a few cycles of slop at a boundary,
 * one way on each side of it: a column on i386 is a little softer than the
 * same column on x86-64, and a DIFFERENCE of a few cycles between the two
 * targets is not a finding.  A difference of hundreds is (#554).
 */
static __inline__ uint64_t
urmach_tsc(void)
{
	uint32_t	lo, hi;

#if	defined(__x86_64__)
	__asm__ __volatile__("lfence; rdtsc" : "=a" (lo), "=d" (hi) :: "memory");
#elif	defined(__i386__)
	__asm__ __volatile__("rdtsc" : "=a" (lo), "=d" (hi) :: "memory");
#else
#error	"urmach_tsc: no timestamp counter on this machine"
#endif
	return ((uint64_t) hi << 32) | lo;
}

#endif	/* _KERN_TSC_H_ */
