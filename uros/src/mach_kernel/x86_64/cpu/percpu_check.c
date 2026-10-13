/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A per-CPU index or pointer read where the thread can still move (#626).
 *
 * cpu_number() and percpu() ask percpu_pinned_check() in <cpu/percpu.h>
 * whether the thread that reads them is pinned to its processor, and come
 * here when nothing pins it: interrupts on, the level at SPL0, the preemption
 * level at zero.  This says so once per call site, by name, the way Linux
 * says "using smp_processor_id() in preemptible code" on the first boot that
 * reaches one.
 *
 * Two cases are not reports, and are asked here rather than on the fast path
 * because they need the thread:
 *
 *   no thread yet	before the first thread runs there is no scheduler to
 *			move anybody; the block is zeroed when it is made
 *			(boot_frame_alloc), so the active thread is null until
 *			cpu_launch_first_thread() sets it;
 *
 *   bound here		a thread bound to the processor it runs on is put back
 *			on that processor whenever it is preempted, so the
 *			number it read stays its own.
 */

#include <mach_assert.h>
#include <cpus.h>

#if MACH_ASSERT && NCPUS > 1

#include <stdint.h>

#include <kern/cpu_data.h>		/* current_thread, disable_preemption */
#include <kern/cpu_number.h>		/* cpu_number_hint */
#include <kern/misc_protos.h>		/* printf */
#include <kern/processor.h>		/* cpu_to_processor */
#include <kern/thread.h>		/* bound_processor */

#include <cpu/percpu.h>
#include <cpu/spl.h>
#include <ddb/ksym.h>
#include <sync/atomic.h>

_Static_assert(SPL0 == 0, "percpu_pinned_check() compares the level with 0");

/*
 * The call sites said so far.  A site is said once: a read in a loop would
 * otherwise fill the log, and the first report names the site, which is
 * what has to be fixed.  Slots are claimed with a compare-and-swap, so two
 * processors meeting the same new site say it once between them.
 */
#define	PERCPU_SITES	256

static volatile uint64_t	percpu_sites[PERCPU_SITES];
static volatile uint32_t	percpu_sites_full;

/* Every unpinned read, said or not -- for gdb, and a total beside the names. */
volatile uint32_t		percpu_unpinned_reads;

static const char *const	percpu_asked[] = {
	[PERCPU_ASKED_NUMBER]	= "cpu_number()",
	[PERCPU_ASKED_BLOCK]	= "percpu()",
};

/* 1 if this site is new and now ours to say, 0 if said, -1 if no room. */
static int
percpu_site_claim(uint64_t site)
{
	unsigned	i;
	uint64_t	s;

	for (i = 0; i < PERCPU_SITES; i++) {
		s = percpu_sites[i];
		if (s == 0)
			s = atomic_cmpxchg64(&percpu_sites[i], 0, site);
		if (s == 0)
			return 1;
		if (s == site)
			return 0;
	}
	return -1;
}

void
percpu_unpinned(int what)
{
	uint64_t	site = (uint64_t) (uintptr_t) __builtin_return_address(0);
	thread_t	self = current_thread();
	processor_t	bound;
	const char	*nm;
	uint64_t	off = 0;
	int		fresh;

	if (self == THREAD_NULL)
		return;
	bound = self->bound_processor;
	if (bound != PROCESSOR_NULL && bound == cpu_to_processor(cpu_number_hint()))
		return;

	(void) atomic_add32(&percpu_unpinned_reads, 1);
	fresh = percpu_site_claim(site);
	if (fresh == 0)
		return;

	/* printf's own reads must not come back here */
	disable_preemption();
	if (fresh < 0) {
		if (atomic_swap32(&percpu_sites_full, 1) == 0)
			printf("percpu: more than %u call sites read a per-CPU "
			       "index or pointer where the thread can still "
			       "move -- WRONG; the rest are counted in "
			       "percpu_unpinned_reads and not named (#626)\n",
			       PERCPU_SITES);
	} else if ((nm = ksym_lookup_call(site, &off)) != 0) {
		printf("percpu: %s read where the thread can still move to "
		       "another processor -- WRONG, at %s+0x%lx on thread %p, "
		       "with interrupts on, the level at zero and preemption "
		       "allowed (#626)\n", percpu_asked[what], nm,
		       (unsigned long) off, self);
	} else {
		printf("percpu: %s read where the thread can still move to "
		       "another processor -- WRONG, at %p on thread %p, with "
		       "interrupts on, the level at zero and preemption "
		       "allowed (#626)\n", percpu_asked[what],
		       (void *) (uintptr_t) site, self);
	}
	enable_preemption();
}

#endif	/* MACH_ASSERT && NCPUS > 1 */
