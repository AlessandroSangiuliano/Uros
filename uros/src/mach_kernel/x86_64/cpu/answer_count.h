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
#include <sync/atomic.h>

/*
 * One slot per APIC id.  Each is written only by the processor it belongs to,
 * so the increment needs no atomic, and read by anybody, so the read takes the
 * whole word at once.
 */
struct answer_count {
	volatile uint64_t by_apic[SMP_MAX_CPUS];
};

/*
 * Count one answer from the processor running this.
 *
 * ⚠️ Bounded on the write as well as on the read.  The two copies checked only
 * the read.  smp.c refuses an application processor whose id is past the
 * table, but nothing asks that of the boot processor, and the boot processor
 * answers too -- the calls the others send it -- so an id past the table would
 * have written past the array.  The check costs a compare.
 */
static inline void answer_count_mark(struct answer_count *c)
{
	uint32_t id = percpu_apic_id();

	if (id < SMP_MAX_CPUS)
		c->by_apic[id]++;
}

/* How many answers the processor with this APIC id has given; 0 past the table. */
static inline uint64_t answer_count_of(const struct answer_count *c,
				       uint32_t apic_id)
{
	if (apic_id >= SMP_MAX_CPUS)
		return 0;

	return atomic_load64(&c->by_apic[apic_id]);
}

#endif	/* _X86_64_CPU_ANSWER_COUNT_H_ */
