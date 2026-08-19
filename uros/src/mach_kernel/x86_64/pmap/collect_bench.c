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
#include <kern/lock.h>			/* mutex_pause (#455) */
#include <kern/processor.h>
#include <kern/task.h>
#include <kern/thread_swap.h>		/* thread_swappable */
#include <kern/cpu_number.h>
#include <kern/misc_protos.h>
#include <x86_64/cpu/spl.h>
#include <mach/vm_prot.h>
#include <time/tsc.h>			/* rdtsc, for the two arms (#455) */

#include <vm/pmap.h>			/* pmap_collect (#455) */
#include <vm/vm_page.h>			/* vm_page_free_count (#455) */

#include <pmap/pmap.h>
#include <pmap/map.h>
#include <pmap/layout.h>
#include <pmap/pte.h>

/*
 * One PML4 slot, one PDPT entry, one PD entry -- so every worker shares the
 * whole descent above its own leaf table, and differs only in which leaf it
 * builds.  That is the arrangement a per-table lock is for.
 */
#define BENCH_PML4_SLOT		9
/*
 * ⚠️ It was 2000, and the collector's arm is what changed it.  Eight workers
 * times two thousand map/unmap pairs takes about thirty seconds on its own and
 * nearly four minutes with a collector meeting them; a thousand keeps eight
 * thousand pairs -- far more than enough for the frame count to catch a lost
 * table, which it counts exactly rather than as a rate -- and brings the run
 * back inside a sensible watchdog.
 */
#define BENCH_ITERATIONS	400

/*
 * How many collections race the workers before the collector stands down.
 *
 * ⚠️ A bound, and it is a measured one.  A collection that takes anything costs
 * two grace periods and a shootdown to every processor, and with one running
 * per clock tick for the whole run the workers went FIFTEEN TIMES slower --
 * 600 iterations in two hundred seconds against 1600 in thirty.  The bench
 * would then have been measuring the collector's price rather than its safety.
 *
 * A hundred is enough because bench_va() makes the meeting deterministic
 * rather than lucky: every worker leaves one of its two leaf tables empty for
 * sixty-four iterations at a time, so a collection that runs at all finds
 * something to take.
 *
 * Stopping early is sound for what this arm asks.  A mapping lost to a
 * collector is lost permanently, so the witnesses and the resident count at the
 * end still answer the question; what the collector has to do is meet the
 * workers at all, and bench_taken reports whether it did.
 */
#define BENCH_COLLECTIONS	60

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

/*
 * The frame each worker maps, kept so the witness below can be checked against
 * the right one -- every worker grabs its own, so "a mapping is there" is a
 * weaker claim than "the right mapping is there" (#455).
 */
static uint64_t		bench_pa[NCPUS];
/*
 * How far each worker has got.  Written by its own processor and read only
 * when the wait gives up, so there is nothing to lose.
 *
 * ⚠️ Not decoration.  A wait that expires can mean a deadlock or merely a
 * budget that is too small, and those want opposite responses; without this
 * the bench said "only 4 of 8 workers finished" and left the difference to be
 * guessed at.  With it the same run said `iteration 1604 of 2000' and the
 * answer was the budget.
 */
static volatile int	bench_iter[NCPUS];

/*
 * What the cost measurement times, and it is timed BY THE WORKER.
 *
 * ⚠️ The first version took the clock in the waiting thread, around the whole
 * phase.  That thread notices the workers have finished by polling once a
 * clock tick, so ten milliseconds of quantisation sat on top of a loop that
 * takes microseconds: it reported 37000 cycles for a mapping that costs a
 * couple of hundred, and the two arms came out within half a per cent of each
 * other because neither of them was what was being measured.
 */
static volatile uint64_t bench_cycles[NCPUS];

/*
 * Mappings per worker in the cost arm, and eight leaf tables to put them in.
 *
 * Bigger than the safety arm's loop on purpose: what is being priced is a few
 * hundred cycles, so the loop has to be long enough to stand above the cost of
 * starting the threads.  Eight page-directory slots each, 512 entries apiece,
 * and nothing is ever unmapped -- so no entry is ever replaced and no shootdown
 * is ever sent.
 */
#define BENCH_COST_MAPS		4096
#define BENCH_COST_SLOTS	8

/*
 * What the collector actually managed to do while the workers ran.
 *
 * ⚠️ Reported, and not as a statistic.  Everything this arm concludes is of
 * the form "nothing was lost", and a run in which the collector never took a
 * table out from under anybody would conclude exactly the same thing while
 * having tested nothing.  The presence has to be visible for the absence to
 * mean anything.
 */
static unsigned		bench_collections;
static unsigned		bench_taken;

static uint64_t bench_va(int worker, int i)
{
	/*
	 * Worker w owns PD slots w and w + NCPUS, so its leaf tables are its
	 * own; the PML4 and PDPT entries above are shared by every worker.
	 * `i' moves within the leaf, which is the part that is never contended.
	 *
	 * 🔥 TWO SLOTS, ALTERNATING EVERY 64 ITERATIONS, and that is what turns
	 * the collector's arm from a lottery into a test.  With one slot a leaf
	 * table is empty only in the instant between a pmap_remove and the next
	 * pmap_enter, so a collection almost never finds one: 758 attempts took
	 * 8 tables.  Alternating leaves the OTHER table empty for sixty-four
	 * iterations at a stretch, so the collector reliably takes a table the
	 * worker is about to come back and use -- which is the case that
	 * matters, and the one a probabilistic arm might never produce.
	 */
	int slot = worker + (((i / 64) & 1) ? NCPUS : 0);

	return ((uint64_t) BENCH_PML4_SLOT << PML4_SHIFT)
	     | ((uint64_t) slot << PD_SHIFT)
	     | ((uint64_t) (i % 64) << PT_SHIFT);
}

/*
 * The one mapping a worker leaves behind, outside the range its loop uses.
 *
 * ⚠️ This is what makes the concurrent-collector arm a test rather than a
 * demonstration.  A collector racing a worker can only be shown harmless by
 * something that would notice harm, and "the machine did not crash" is not
 * that: a table unlinked while a mapping was going into it loses the mapping
 * SILENTLY and pmap_enter still answers success (#455).  So each worker
 * installs one mapping it never removes, and the run ends by asking whether
 * every one of them is still there and still names the right frame.
 */
static uint64_t bench_witness_va(int worker)
{
	/*
	 * ⚠️ A PAGE DIRECTORY SLOT OF ITS OWN, not a spare index in the leaf the
	 * loop uses.  It was written that way first, and it quietly disarmed the
	 * whole arm: a leaf table holding the witness is never empty, so
	 * pmap_collect() could never consider it and the collector and the
	 * workers never met at all.  The witness has to be somewhere the loop's
	 * table can still go empty between two iterations.
	 */
	return ((uint64_t) BENCH_PML4_SLOT << PML4_SHIFT)
	     | ((uint64_t) (worker + 256) << PD_SHIFT);
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
	 * gave it back: at 64 workers that is tens of thousands of pages
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
	bench_pa[me] = pa;

	while (bench_start == 0)
		/* spin: the point is to start together, not to be polite */;

	/*
	 * The witness first, so that it is in place for the whole run and every
	 * moment of the collector's racing with this worker is a moment it
	 * could have lost it.
	 */
	(void) pmap_enter(bench_pmap, bench_witness_va(me), pa, VM_PROT_READ,
			  FALSE);

	for (i = 0; i < BENCH_ITERATIONS; i++) {
		uint64_t va = bench_va(me, i);

		bench_iter[me] = i;
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

/*
 * The measurement the issue asks for, with nothing else running: a space that
 * maps a wide sparse range and unmaps it, and the count of frames it holds
 * before and after -- reported rather than asserted.
 *
 * Sparse on purpose, one page per page directory slot, so every page needs a
 * page table of its own and unmapping leaves that table empty.  That is the
 * behaviour the issue describes -- "a server doing mmap/munmap across
 * scattered addresses builds tables all over the lower half" -- and on a
 * 64-bit space it is the ordinary case rather than a contrived one.
 */
#define BENCH_SPARSE_PAGES	40
#define BENCH_SPARSE_SLOT	11

static uint64_t bench_sparse_va(int i)
{
	return ((uint64_t) BENCH_SPARSE_SLOT << PML4_SHIFT)
	     | ((uint64_t) i << PD_SHIFT);
}

/*
 * The page-directory entry above `va', which is the entry pmap_collect()
 * examines when it decides about the page table below it.
 *
 * Written out here rather than borrowed from pmap_walk(), which answers with
 * the LEAF and so cannot name the thing under test.
 */
static pt_entry_t *bench_pd_entry(pmap_t p, uint64_t va)
{
	pt_entry_t	*table;
	pt_entry_t	 e;

	table = (pt_entry_t *)(uintptr_t)phys_to_direct(p->root_pa);
	e = table[pml4_index(va)];
	if (!pte_is_valid(e) || pte_is_leaf(e))
		return PT_ENTRY_NULL;

	table = (pt_entry_t *)(uintptr_t)phys_to_direct(pte_to_pa(e));
	e = table[pdpt_index(va)];
	if (!pte_is_valid(e) || pte_is_leaf(e))
		return PT_ENTRY_NULL;

	table = (pt_entry_t *)(uintptr_t)phys_to_direct(pte_to_pa(e));
	if (!pte_is_valid(table[pd_index(va)]))
		return PT_ENTRY_NULL;

	return &table[pd_index(va)];
}

/* Collect until a call gives nothing back.  Answers how many tables went. */
static unsigned bench_collect_all(pmap_t p, int *rounds_out)
{
	unsigned	start = pmap_table_frames_live;
	int		rounds;

	/*
	 * Bounded: one call takes at most a batch, so a loop is needed, and a
	 * loop that cannot stop would turn a wrong answer into a hang.
	 */
	for (rounds = 0; rounds < 64; rounds++) {
		unsigned before = pmap_table_frames_live;

		pmap_collect(p);
		if (pmap_table_frames_live == before)
			break;
	}

	if (rounds_out)
		*rounds_out = rounds + 1;

	return start - pmap_table_frames_live;
}

static void bench_collect_quiet(void)
{
	pmap_t		p = pmap_create(0);
	uint64_t	pa;
	unsigned	held, freed, refused, armed = 0;
	unsigned	free_before, free_after;
	int		i, rounds;

	if (p == PMAP_NULL) {
		printf("pmap_bench: no pmap for the quiet measurement -- WRONG\n");
		return;
	}

	pa = pmap_table_frame();
	if (pa == 0) {
		printf("pmap_bench: no frame for the quiet measurement -- WRONG\n");
		pmap_destroy(p);
		return;
	}

	for (i = 0; i < BENCH_SPARSE_PAGES; i++)
		(void) pmap_enter(p, bench_sparse_va(i), pa, VM_PROT_READ,
				  FALSE);

	held = pmap_table_frames_live;
	free_before = vm_page_free_count;

	for (i = 0; i < BENCH_SPARSE_PAGES; i++)
		pmap_remove(p, bench_sparse_va(i),
			    bench_sparse_va(i) + PAGE_SIZE_4K);

	/*
	 * ── The reference-bit hint, with a control ───────────────────────
	 *
	 * ⚠️ The hardware cannot arm it here, and that is a property of the
	 * experiment rather than of the hint.  ACCESSED is set when the
	 * processor WALKS an entry, and this space has never been in CR3:
	 * nothing has ever translated an address through these tables, so
	 * every parent entry is cold whatever the space did.
	 *
	 * So it is armed by hand, which turns "the hint exists" into a
	 * measurement with two sides: with the bit set on every parent, a
	 * collection must give back NOTHING and clear the bits; the next one
	 * must give back everything.  A hint that is only described is a hint
	 * that has never been shown to refuse anything.
	 */
	for (i = 0; i < BENCH_SPARSE_PAGES; i++) {
		pt_entry_t *pde = bench_pd_entry(p, bench_sparse_va(i));

		if (pde != PT_ENTRY_NULL) {
			*pde |= INTEL_PTE_REF;
			armed++;
		}
	}

	refused = bench_collect_all(p, &rounds);
	printf("pmap_bench: %u parents marked referenced by hand; a full "
	       "collection gave back %u tables (%s)\n",
	       armed, refused,
	       refused == 0
	       ? "the second chance refused every one"
	       : "THE REFERENCE BIT DID NOT REFUSE -- WRONG");

	freed = bench_collect_all(p, &rounds);
	free_after = vm_page_free_count;

	printf("pmap_bench: %d sparse pages held %u interior tables; after "
	       "unmapping and a second pass, %u came back in %d rounds, "
	       "leaving %u\n",
	       BENCH_SPARSE_PAGES, held, freed, rounds,
	       pmap_table_frames_live);

	printf("pmap_bench: free pages %u -> %u, so %d went back to the VM "
	       "rather than onto a list nobody pops -- %s\n",
	       free_before, free_after, (int) free_after - (int) free_before,
	       free_after > free_before
	       ? "the pageout daemon gets something for asking"
	       : "NOTHING WAS RECLAIMED (#455)");

	pmap_table_frame_free(pa);
	pmap_destroy(p);
}

/*
 * The worker of the cost measurement: mappings only, into addresses of its own,
 * and never removed.
 *
 * ⚠️ Not the same loop as the safety arm, and the difference is the point.  A
 * map/unmap pair costs a TLB shootdown to every other processor and WAITS for
 * it -- measured at some 57000 cycles a pair, which is twenty microseconds of
 * inter-processor round trip with the exclusion somewhere inside it.  Anything
 * measured that way is a measurement of #439's shootdown.
 *
 * Mapping into an empty slot revokes nothing, so pmap_map_page() sends no
 * message (map.c says why), and what is left in the loop is the descent, the
 * leaf write and the exclusion -- which is the common path the two arms differ
 * on.  Four hundred addresses fit in one leaf table, so the tables are built
 * once and the rest of the run is pure descent.
 */
static void bench_cost_worker(void)
{
	int	 me = cpu_number();
	uint64_t pa;
	int	 i;

	pa = pmap_table_frame();
	if (pa == 0) {
		bench_done[me] = 1;
		thread_terminate_self();
	}
	bench_pa[me] = pa;

	while (bench_start == 0)
		/* spin: the point is to start together, not to be polite */;

	bench_cycles[me] = rdtsc();

	for (i = 0; i < BENCH_COST_MAPS; i++) {
		uint64_t slot = (uint64_t) me * BENCH_COST_SLOTS
			      + (uint64_t) (i / PTES_PER_TABLE);
		uint64_t va = ((uint64_t) BENCH_PML4_SLOT << PML4_SHIFT)
			    | (slot << PD_SHIFT)
			    | ((uint64_t) (i % PTES_PER_TABLE) << PT_SHIFT);

		bench_iter[me] = i;

		/*
		 * ⚠️ pmap_map_page() and not pmap_enter(), because the two arms
		 * differ INSIDE this function and nowhere else.  Going through
		 * pmap_enter() would add pmap_forget()'s two walks and a
		 * physical-index insertion to every iteration -- work both arms
		 * pay equally, which does not cancel out of a ratio, it dilutes
		 * it.  It also grew the pv list of the one frame every worker
		 * maps to four hundred entries and wedged the run.
		 */
		(void) pmap_map_page(bench_pmap->root_pa, va, pa,
				     INTEL_PTE_USER,
				     &bench_pmap->collect_lock);
	}

	bench_cycles[me] = rdtsc() - bench_cycles[me];

	bench_done[me] = 1;
	thread_terminate_self();
}

/*
 * ══ The number the issue asks for ═════════════════════════════════════
 *
 * "Whether that discipline is #338's granularity or a different one is decided
 * with a number in front of it at NCPUS=64, not inherited."
 *
 * The two arms run BACK TO BACK IN ONE BOOT, on one binary, and that is not a
 * convenience: a rate measured in one build and a rate measured in another
 * differ by everything the compiler did differently, so what would be compared
 * is the builds.  Same kernel, same processors, same workload, one branch on a
 * global apart.
 *
 * The load is the one this file was built for: every worker maps into its own
 * leaf table, so they never touch the same page-table entry and a per-table
 * lock would let them all run.  Under ONE lock for the address space they
 * serialise completely.  That is deliberately the worst case for the lock arm
 * -- if a single lock is cheap enough here it is cheap enough anywhere, and if
 * it is not, the answer is #338's granularity rather than none.
 *
 * ⚠️ No collector runs during this.  What is being priced is what the WRITER
 * pays for the exclusion to exist, which it pays on every mapping whether a
 * collection ever happens or not.  The collector's own cost is the other half
 * of the trade and it is measured where it happens, in the arm below.
 */
static uint64_t bench_arm_cost(int arm, int *want_out)
{
	int	 want = 0, i;

	pmap_writer_arm = arm;

	bench_pmap = pmap_create(0);
	if (bench_pmap == PMAP_NULL) {
		*want_out = 0;
		return 0;
	}

	for (i = 0; i < NCPUS; i++) {
		bench_done[i] = 0;
		bench_iter[i] = 0;
		bench_pa[i] = 0;
		bench_cycles[i] = 0;
	}
	bench_start = 0;

	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p == PROCESSOR_NULL || p->state == PROCESSOR_OFF_LINE)
			continue;
		if (bench_thread_bound(bench_cost_worker, p) == THREAD_NULL)
			continue;
		want++;
	}

	bench_start = 1;

	{
		int done = 0, waits;

		for (waits = 0; waits < 1500 && done < want; waits++) {
			int k;

			mutex_pause();
			done = 0;
			for (k = 0; k < NCPUS; k++)
				done += bench_done[k];
		}

		if (done < want) {
			printf("pmap_bench: arm %d: only %d of %d workers "
			       "finished -- the number below is WRONG\n",
			       arm, done, want);
			for (i = 0; i < NCPUS; i++)
				if (bench_pa[i] != 0)
					printf("    cpu %d at %d of %d\n", i,
					       bench_iter[i],
					       BENCH_COST_MAPS);
		}
	}

	/*
	 * The mean of what each worker measured for itself, which is what a
	 * mapping costs A PROCESSOR while `want' of them are mapping at once --
	 * exactly the quantity a shared lock changes and a bit does not.
	 */
	{
		uint64_t total = 0;

		for (i = 0; i < NCPUS; i++)
			total += bench_cycles[i];

		pmap_destroy(bench_pmap);
		bench_pmap = PMAP_NULL;
		pmap_writer_arm = PMAP_ARM_COLLECT_BIT;

		*want_out = want;
		return want ? total / ((uint64_t) want * BENCH_COST_MAPS) : 0;
	}
}

/* The middle of five, by insertion -- five numbers do not need a real sort. */
static uint64_t bench_median5(uint64_t *v)
{
	uint64_t a[5];
	int	 i, j;

	for (i = 0; i < 5; i++)
		a[i] = v[i];

	for (i = 1; i < 5; i++) {
		uint64_t k = a[i];

		for (j = i; j > 0 && a[j - 1] > k; j--)
			a[j] = a[j - 1];
		a[j] = k;
	}

	return a[2];
}

static void bench_compare_arms(void)
{
	uint64_t bit[5], lock[5];
	uint64_t warm;
	int	 want = 0, w, r;

	/*
	 * ⚠️ FIVE SAMPLES OF EACH, ALTERNATING, AND A DISCARDED FIRST RUN.
	 *
	 * One sample of each answered that the LOCK was nearly twice as FAST as
	 * the bit, which is not a result: the first arm to run builds its page
	 * tables out of pages the VM has never handed out and the second reuses
	 * the ones the first gave back.  Five alternating samples make the
	 * warm-up one point instead of one arm, and the median is what gets
	 * compared.
	 *
	 * The spread of the five is printed with them, because a difference
	 * smaller than the spread is not a difference -- at two processors it
	 * is exactly that, and saying so is the honest half of the answer.
	 */
	warm = bench_arm_cost(PMAP_ARM_COLLECT_BIT, &want);

	for (r = 0; r < 5; r++) {
		bit[r]  = bench_arm_cost(PMAP_ARM_COLLECT_BIT, &w);
		if (w != want)
			want = 0;
		lock[r] = bench_arm_cost(PMAP_ARM_PMAP_LOCK, &w);
		if (w != want)
			want = 0;
	}

	if (want == 0) {
		printf("pmap_bench: the arms did not run the same load "
		       "-- WRONG\n");
		return;
	}

	printf("pmap_bench: %d mappings on %d processors, cycles each\n",
	       BENCH_COST_MAPS * want, want);
	printf("  collect bit:      %llu %llu %llu %llu %llu (median %llu, "
	       "warm-up %llu)\n",
	       (unsigned long long) bit[0], (unsigned long long) bit[1],
	       (unsigned long long) bit[2], (unsigned long long) bit[3],
	       (unsigned long long) bit[4],
	       (unsigned long long) bench_median5(bit),
	       (unsigned long long) warm);
	printf("  one lock a space: %llu %llu %llu %llu %llu (median %llu)\n",
	       (unsigned long long) lock[0], (unsigned long long) lock[1],
	       (unsigned long long) lock[2], (unsigned long long) lock[3],
	       (unsigned long long) lock[4],
	       (unsigned long long) bench_median5(lock));
	/*
	 * ⚠️ The one thing a reader has to know to use these numbers.
	 *
	 * The lock arm spins, and a spinning guest processor under pure
	 * emulation holds the emulator's own thread for a whole scheduling
	 * quantum while the holder waits its turn -- so an unaccelerated run
	 * prices QEMU rather than the design.  The safety arm below is the
	 * opposite: it wants emulation, because that is where the interleavings
	 * it hunts are widest.  Two questions, two instruments.
	 */
	printf("  ⚠️ only meaningful accelerated or on iron: a spin lock under "
	       "pure emulation measures the emulator\n");
	printf("  the lock costs %llu per mille of the bit\n",
	       (unsigned long long) (bench_median5(bit)
				     ? (bench_median5(lock) * 1000ULL)
					/ bench_median5(bit)
				     : 0));
}

void
pmap_collect_bench(void)
{
	int	 want = 0;
	int	 i;
	unsigned expect;

	/*
	 * The quiet measurement first, while nothing else is building tables:
	 * it reads a global count, so anything running beside it would be
	 * measuring the neighbour.
	 */
	bench_collect_quiet();

	/*
	 * Then the price of each arm on the common path, before the arm that
	 * decides whether the chosen one is SAFE.  Order matters only in that
	 * both want a quiet machine.
	 */
	bench_compare_arms();

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

	bench_collections = 0;
	bench_taken = 0;

	bench_start = 1;

	/*
	 * ⚠️ A bounded wait, and it reports a timeout rather than hanging.  A
	 * bench that stops the boot when a worker never starts costs a whole
	 * run to find out; one that says how many finished names the failure.
	 */
	{
		/*
		 * ⚠️ THE WAIT YIELDS, IT DOES NOT SPIN, AND THAT IS THE WHOLE
		 * DIFFERENCE BETWEEN A RACE AND A FAMINE.
		 *
		 * This was a busy loop with a spin budget, and the budget kept
		 * becoming the verdict: three runs reported "only 1 of 8
		 * workers finished -- WRONG" with the workers at iteration
		 * 1604, then 721, then 576 of 2000 and still climbing.  Raising
		 * the budget tenfold made it WORSE, which is the clue -- the
		 * spinner is a ninth runnable thread on eight processors, and
		 * every processor it holds is one a BOUND worker cannot run on
		 * at all.  The instrument was eating the subject.
		 *
		 * mutex_pause() gives the processor up for a tick instead.  The
		 * collector then runs about once per tick for the whole run,
		 * which is frequent enough to meet the workers and cheap enough
		 * to leave them the machine.
		 */
		int done = 0, waits;

		for (waits = 0; waits < 20000 && done < want; waits++) {
			int	 k;
			unsigned was = pmap_table_frames_live;

			/*
			 * 🔥 THE COLLECTOR RUNS HERE, against them: eight
			 * workers building and tearing down leaf tables while
			 * pmap_collect() tries to take the empty ones away.
			 *
			 * ⚠️ A collection that finds anything to take costs TWO
			 * GRACE PERIODS, and a grace period ends only when
			 * every processor has passed through depth zero -- so
			 * its latency is set by how often the readers are
			 * OUTSIDE a read section.  These workers are inside one
			 * much of the time, so one collection can cost several
			 * clock ticks.  That cost is real, it is paid by the
			 * rare operation exactly as intended, and it is why
			 * this cannot be run in a tight loop.
			 */
			if (bench_collections < BENCH_COLLECTIONS) {
				pmap_collect(bench_pmap);
				bench_collections++;
				if (pmap_table_frames_live < was)
					bench_taken += was
						     - pmap_table_frames_live;
			}

			mutex_pause();

			done = 0;
			for (k = 0; k < NCPUS; k++)
				done += bench_done[k];
		}

		if (done < want) {
			/*
			 * ⚠️ How far each one got, not just how many finished.
			 * That is what tells a wedged run from a slow one, and
			 * without it three separate runs were reported as
			 * failures of the pmap when they were failures of the
			 * budget.
			 */
			printf("pmap_bench: only %d of %d workers finished "
			       "after %d waits, %u collections, %u tables "
			       "taken -- WRONG\n",
			       done, want, waits, bench_collections,
			       bench_taken);
			for (i = 0; i < NCPUS; i++)
				if (bench_pa[i] != 0)
					printf("  cpu %d reached iteration "
					       "%d of %d\n", i,
					       bench_iter[i],
					       BENCH_ITERATIONS);
			return;
		}
	}

	/*
	 * Every witness, and the frame it names.
	 *
	 * This is the check that a concurrent collector loses nothing.  A table
	 * unlinked while a mapping was being written into it takes the mapping
	 * with it and pmap_enter still answers success, so nothing else in this
	 * run would notice.
	 */
	{
		int lost = 0, wrong = 0;

		for (i = 0; i < NCPUS; i++) {
			uint64_t got;

			if (bench_done[i] == 0 || bench_pa[i] == 0)
				continue;

			got = pmap_extract(bench_pmap, bench_witness_va(i));
			if (got == 0)
				lost++;
			else if (got != bench_pa[i])
				wrong++;
		}

		/*
		 * ⚠️ One processor is not a failure to race, it is a
		 * configuration in which there is no race to have: the
		 * collector runs on the same processor as the worker, so the
		 * two can never be inside the tables at the same instant.
		 * Saying "IT NEVER RACED" there would put an alarm on the
		 * uniprocessor for behaving exactly as it must.
		 */
		printf("pmap_bench: the collector ran %u times during the run "
		       "and took %u tables out from under the workers -- %s\n",
		       bench_collections, bench_taken,
		       bench_taken > 0
		       ? "the race happened"
		       : want > 1
		       ? "IT NEVER RACED: what follows tests nothing"
		       : "one processor, so there was no overlap to have -- "
			 "what follows is the uniprocessor's own answer");

		printf("pmap_bench: %d witness mappings across a running "
		       "collector: %d lost, %d naming the wrong frame (%s)\n",
		       want, lost, wrong,
		       (lost == 0 && wrong == 0)
		       ? "every mapping survived the race"
		       : "A MAPPING WAS LOST TO pmap_collect -- WRONG");

		/*
		 * And the count, which catches what the witnesses cannot.
		 *
		 * A mapping written into a table the collector had just
		 * unlinked is not lost to the caller -- pmap_enter answers
		 * success -- and the following pmap_remove then finds nothing
		 * to remove, so the space is left believing it holds one more
		 * page than it does.  Each worker ends with exactly its
		 * witness, so the count is answerable to the last unit.
		 */
		printf("pmap_bench: resident pages %d, expected %d (%s)\n",
		       pmap_resident_count(bench_pmap), want,
		       pmap_resident_count(bench_pmap) == want
		       ? "every map/unmap pair balanced across the collector"
		       : "A MAPPING WENT INTO AN UNLINKED TABLE -- WRONG");
	}

	/*
	 * Now collect what the run left behind, with nothing else running.
	 *
	 * Each worker's loop table is empty at this point -- its last act was a
	 * pmap_remove -- so a collection to exhaustion must take every one of
	 * them and leave exactly the witnesses' tables plus the page directory
	 * and PDPT they share.  Which is the same expectation the run already
	 * had, arrived at through the collector rather than around it.
	 */
	{
		int	 rounds;
		unsigned reclaimed = bench_collect_all(bench_pmap, &rounds);

		printf("pmap_bench: a final collection took %u empty leaf "
		       "tables in %d rounds\n", reclaimed, rounds);
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
	       ? "one witness leaf each plus the shared PD and PDPT, "
		 "nothing lost"
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
