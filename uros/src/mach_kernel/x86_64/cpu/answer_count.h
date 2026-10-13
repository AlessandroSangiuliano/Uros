/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * How many times each processor answered something, counted by the one that
 * answered (#605).
 *
 * The cross-call (cpu/ipi.c) and the TLB shootdown (pmap/tlb.c) each keep one:
 * the boot-time proof that the messages arrive, and afterwards a way to name a
 * processor that has stopped answering rather than merely count that one has.
 * They were two copies of the same array and the same accessor; this is the
 * one they share, so a change to either is a change to both.
 */

#ifndef _X86_64_CPU_ANSWER_COUNT_H_
#define _X86_64_CPU_ANSWER_COUNT_H_

#include <stdint.h>

#include <cpu/percpu.h>
#include <cpu/smp.h>
#include <kern/cpu_number.h>	/* #663: counted by processor number */
#include <sync/atomic.h>

/*
 * One slot per APIC id.  Each is written only by the processor it belongs to,
 * so the increment needs no atomic, and read by anybody, so the read takes the
 * whole word at once.
 *
 * ⚠️ Eight slots share a cache line, so a broadcast has up to eight answering
 * processors writing the same line at once.  UROS_PROBE_605_PADDED gives every
 * slot a line of its own, to measure what the sharing costs before deciding
 * anything about it (#605).
 */
#ifndef	PROBE_605_PADDED
#define	PROBE_605_PADDED	0
#endif

struct answer_slot {
	volatile uint64_t	n;
#if	PROBE_605_PADDED
	char			pad[56];
#endif
}
#if	PROBE_605_PADDED
__attribute__((aligned(64)))
#endif
;

struct answer_count {
	struct answer_slot by_cpu[SMP_MAX_CPUS];
};

/*
 * Count one answer from the processor running this, by its number (#663).
 *
 * The write needs no bound: a number is below SMP_MAX_CPUS by construction,
 * which is what smp_number_cpus() keeps.  It used to be the APIC id, which
 * nothing bounded for the boot processor, and the check here stood in for
 * that.  The read keeps its bound, since a caller may ask about any number.
 */
static inline void answer_count_mark(struct answer_count *c)
{
	c->by_cpu[cpu_number()].n++;
}

/* How many answers the processor numbered `cpu' has given; 0 past the table. */
static inline uint64_t answer_count_of(const struct answer_count *c,
				       unsigned cpu)
{
	if (cpu >= SMP_MAX_CPUS)
		return 0;

	return atomic_load64(&c->by_cpu[cpu].n);
}

#endif	/* _X86_64_CPU_ANSWER_COUNT_H_ */
