/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The processor's performance-monitoring counters, for the profiler's phases
 * (#804).
 */

#ifndef _X86_64_CPU_PMC_H_
#define _X86_64_CPU_PMC_H_

#include <stdint.h>

/*
 * The counters read together: instructions retired and unhalted core cycles
 * in the first two fixed counters, then up to four events in the general
 * ones.
 */
#define	PMC_MAX		6

/*
 * Once on every processor, beside hwp_init_cpu(): the boot processor reads
 * what CPUID says the processor has and decides, and pmc=N on the command
 * line picks an event set, which each processor then programs into its own
 * counters (each logical processor has its own, SDM 22.2.1).  Without pmc=,
 * no MSR is touched.
 */
void pmc_init_cpu(int boot_processor);

/*
 * What the processor has, what was asked and what each processor programmed,
 * in one line, once every processor is past pmc_init_cpu().
 */
void pmc_report(void);

/* How many counters count -- 0 without pmc= or when it was refused -- and each one's name. */
int		pmc_count(void);
const char	*pmc_name(int i);

/*
 * And what each one measures, by which pmc_test.c knows what a load of known
 * shape must make it count; 0 for a counter that is not counting.
 */
#define	PMC_ROLE_INST			1
#define	PMC_ROLE_CYCLES			2
#define	PMC_ROLE_L1D_MISS		3	/* a line replaced in the L1D */
#define	PMC_ROLE_L2_MISS		4
#define	PMC_ROLE_LLC_MISS		5
#define	PMC_ROLE_HITM			6	/* a load from another core's modified line */
#define	PMC_ROLE_DTLB_LOAD_WALK		7
#define	PMC_ROLE_DTLB_STORE_WALK	8
#define	PMC_ROLE_ITLB_WALK		9
#define	PMC_ROLE_LLC_REF		10
#define	PMC_ROLE_BRANCH			11
#define	PMC_ROLE_BRANCH_MISS		12
int		pmc_role(int i);

/*
 * The same count, for the profiler's marks, which read it on every phase and
 * must not pay a call to learn that nothing is counting.  Set once, by the
 * boot processor.
 */
extern int	pmc_counting;

/*
 * RDPMC reads counter ECX: a general one by its index, a fixed one with bit
 * 30 set (Vol. 2B, RDPMC).  At CPL 0 it needs no CR4.PCE.
 */
static inline uint64_t
rdpmc(uint32_t counter)
{
	uint32_t	lo, hi;

	__asm__ volatile("rdpmc" : "=a"(lo), "=d"(hi) : "c"(counter));
	return ((uint64_t) hi << 32) | lo;
}

/* Every counter that counts, into v[0 .. pmc_count() - 1]. */
void	pmc_read(uint64_t *v);

#endif	/* _X86_64_CPU_PMC_H_ */
