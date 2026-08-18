/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What concurrency does to a pmap that has no locking (#455).
 *
 * ── Why this exists ──────────────────────────────────────────────────
 *
 * #455 asks for pmap_collect(), and the walk it needs is only safe under a
 * locking discipline this pmap does not have -- pv.c says so outright: "this
 * pmap having no locking yet".  The issue also asks that the granularity be
 * "decided with a number in front of it at NCPUS=64, not inherited", i386's
 * answer being #338's per-table locks, arrived at on eight processors.
 *
 * A number requires a load that actually contends.  This is that load, and it
 * is built to make the cheapest possible answer -- one lock per pmap -- look
 * as bad as it can: every worker maps into its OWN leaf table, so they never
 * touch the same page-table entry, and share the PDPT and PD above it.  Under
 * a per-pmap lock they serialise completely; under #338's granularity they
 * would not.  If a single lock is enough here, it is enough anywhere.
 *
 * ⚠️ THE FAILURE DETECTOR IS THE FRAME COUNT, NOT A CRASH.
 *
 * Two processors descending into the same missing table both call
 * pmap_table_frame(), both write their own frame into the parent entry, and
 * one write lands second.  The loser's table is still allocated and now
 * unreachable: nothing faults, nothing panics, and the space quietly holds a
 * page nobody can find.  pmap_table_frames_live counts exactly that, so a run
 * that ends above where it started has lost tables to a race -- which is the
 * same defect pmap_collect() would have to walk into.
 *
 * The threads are bound BEFORE they can run, and that is not a detail:
 * preempt_test.c documents what happens otherwise -- kernel_thread() sets
 * TH_RUN and calls thread_setrun() itself, so a bind that follows arrives
 * after the race it was meant to prevent, and a verdict about N processors
 * gets produced by threads that were on other ones.
 */

#include <stdint.h>

#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/processor.h>
#include <kern/task.h>
#include <kern/thread_swap.h>		/* thread_swappable */
#include <kern/cpu_number.h>
#include <kern/misc_protos.h>
#include <x86_64/cpu/spl.h>
#include <mach/vm_prot.h>

#include <pmap/pmap.h>
#include <pmap/layout.h>
#include <pmap/pte.h>

/*
 * One PML4 slot, one PDPT entry, one PD entry -- so every worker shares the
 * whole descent above its own leaf table, and differs only in which leaf it
 * builds.  That is the arrangement a per-table lock is for.
 */
#define BENCH_PML4_SLOT		9
#define BENCH_ITERATIONS	2000

static pmap_t		bench_pmap;
/*
 * ⚠️ One slot per worker, not a shared counter.
 *
 * It WAS `bench_workers_done++' from every processor, and at -smp 8 the bench
 * reported "only 3 of 8 workers finished" -- five increments lost to exactly
 * the read-modify-write race this bench exists to detect.  A harness that
 * loses updates while measuring lost updates reports its own defect as the
 * subject's.  A slot each is written by one processor and read by one, so
 * there is nothing to lose.
 */
static volatile int	bench_done[NCPUS];
static volatile int	bench_start;
static unsigned		bench_frames_at_start;

static uint64_t bench_va(int worker, int i)
{
	/*
	 * Worker w owns PD slot w, so its leaf table is its own; the PML4 and
	 * PDPT entries above are shared by every worker.  `i' moves within the
	 * leaf, which is the part that is never contended.
	 */
	return ((uint64_t) BENCH_PML4_SLOT << PML4_SHIFT)
	     | ((uint64_t) worker << PD_SHIFT)
	     | ((uint64_t) (i % 64) << PT_SHIFT);
}

static void bench_worker(void)
{
	int	 me = cpu_number();
	uint64_t pa;
	int	 i;

	/*
	 * ⚠️ ONE frame, taken once and mapped over and over.
	 *
	 * The first version of this took a fresh one per iteration and never
	 * gave it back: 64 workers times 2000 iterations is 128000 pages
	 * against the 29776 this machine has free, so every worker blocked in
	 * VM_PAGE_WAIT and none of the 64 ever finished.  The bench reported
	 * "only 0 of 64 workers finished" rather than hanging, which is the
	 * whole reason the wait is bounded -- a test that stops the boot when
	 * it is wrong about itself costs a run to find out.
	 *
	 * What is being measured is the page-table descent, not the page
	 * supply, so the same frame is the right subject anyway.
	 */
	pa = pmap_table_frame();
	if (pa == 0) {
		bench_done[me] = 1;
		thread_terminate_self();
	}

	while (bench_start == 0)
		/* spin: the point is to start together, not to be polite */;

	for (i = 0; i < BENCH_ITERATIONS; i++) {
		uint64_t va = bench_va(me, i);

		(void) pmap_enter(bench_pmap, va, pa, VM_PROT_READ, FALSE);
		pmap_remove(bench_pmap, va, va + PAGE_SIZE_4K);
	}

	bench_done[me] = 1;
	thread_terminate_self();
}

/*
 * kernel_thread() with the bind moved inside, mirroring preempt_test.c's
 * preempt_thread_bound() -- including the order of act_deallocate() and
 * thread_resume().  This is that function with one call inserted, not a
 * second way of making a kernel thread.
 */
static thread_t bench_thread_bound(void (*fn)(void), processor_t target)
{
	thread_t	th;
	thread_act_t	act;
	spl_t		s;

	if (thread_create_at(kernel_task, &th, fn) != KERN_SUCCESS)
		return THREAD_NULL;

	thread_swappable(th->top_act, FALSE);

	s = splsched();
	thread_lock(th);

	act = th->top_act;
	th->max_priority = BASEPRI_SYSTEM;
	th->priority     = BASEPRI_SYSTEM;
	th->sched_pri    = BASEPRI_SYSTEM;

	thread_bind_locked(th, target);

	th->state |= TH_RUN;
	thread_setrun(th, TRUE, TAIL_Q);
	thread_unlock(th);
	splx(s);

	act_deallocate(act);
	thread_resume(act);

	return th;
}

void
pmap_collect_bench(void)
{
	int	 want = 0;
	int	 i;
	unsigned expect;

	bench_pmap = pmap_create(0);
	if (bench_pmap == PMAP_NULL) {
		printf("pmap_bench: no pmap -- WRONG\n");
		return;
	}

	bench_frames_at_start = pmap_table_frames_live;
	for (i = 0; i < NCPUS; i++)
		bench_done[i] = 0;
	bench_start = 0;

	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p == PROCESSOR_NULL || p->state == PROCESSOR_OFF_LINE)
			continue;
		if (bench_thread_bound(bench_worker, p) == THREAD_NULL)
			continue;
		want++;
	}

	printf("pmap_bench: %d workers, %d iterations each, one leaf table "
	       "apiece under a shared PDPT and PD\n", want, BENCH_ITERATIONS);

	bench_start = 1;

	/*
	 * ⚠️ A bounded wait, and it reports a timeout rather than hanging.  A
	 * bench that stops the boot when a worker never starts costs a whole
	 * run to find out; one that says how many finished names the failure.
	 */
	{
		int spins, done = 0;

		for (spins = 0; spins < 200000000 && done < want; spins++) {
			int k;

			done = 0;
			for (k = 0; k < NCPUS; k++)
				done += bench_done[k];
		}

		if (done < want) {
			printf("pmap_bench: only %d of %d workers finished "
			       "-- WRONG\n", done, want);
			return;
		}
	}

	/*
	 * ⚠️ want + 2, and the two are the point of the arrangement.
	 *
	 * Every worker builds its own LEAF, and they all share one PDPT and one
	 * PD above it -- which also have to be created, once, by whichever
	 * worker gets there first.  The first version of this expected `want'
	 * and reported a race on a run that had none: an expectation that
	 * forgets part of what it asked for reads a correct result as a defect,
	 * which is the same way round as a check that cannot fail.
	 */
	expect = bench_frames_at_start + (unsigned) want + 2;

	printf("pmap_bench: %d workers done, interior tables %u -> %u, "
	       "expected %u (%s)\n",
	       want, bench_frames_at_start, pmap_table_frames_live, expect,
	       pmap_table_frames_live == expect
	       ? "one leaf each plus the shared PD and PDPT, nothing lost"
	       : "TABLES LOST TO A RACE -- the pmap needs a lock (#455)");

	/*
	 * And then destroy the space, which is the question a lock does not
	 * answer: pmap_destroy() reclaims by WALKING THE TREE, and a table lost
	 * to the race is linked to nothing -- it is not in the tree to be
	 * found.  So whatever is left standing here is leaked for the lifetime
	 * of the machine, and no collector that walks can ever see it.
	 */
	pmap_destroy(bench_pmap);
	printf("pmap_bench: after pmap_destroy, interior tables %u (started at "
	       "%u) -- %s\n",
	       pmap_table_frames_live, bench_frames_at_start,
	       pmap_table_frames_live == bench_frames_at_start
	       ? "every table came back"
	       : "the lost ones are UNREACHABLE: a walk cannot find them");
}
