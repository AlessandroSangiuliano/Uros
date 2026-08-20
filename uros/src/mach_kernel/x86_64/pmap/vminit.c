/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What the machine-independent VM asks this machine before it can run (#453,
 * on #407's ground).
 *
 * Two of these are the bootstrap handshake and the rest are questions about
 * physical pages.  The handshake is the interesting part: at startup the VM
 * has no page allocator of its own -- it is building one -- so it asks the
 * machine for physical pages one at a time, and for the range of kernel
 * virtual addresses it may hand out.  Once vm_page_bootstrap() has taken
 * what it needs, the machine is never asked again.
 */

#include <stdint.h>

#include <mach/machine/vm_param.h>
#include <vm/pmap.h>

#include <kern/rcu.h>			/* the grace periods below (#455) */
#include <sync/atomic.h>		/* atomic_cmpxchg64 (#455) */

#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>
#include <pmap/pv.h>
#include <pmap/tlb.h>
#include <pmap/walk.h>
#include <trap/trap.h>

/*
 * One more physical page for the VM to take over, or FALSE when there are
 * none left.
 *
 * The pages come from the boot frame allocator, which is the only thing that
 * knows what the loader left free.  Handing them over one at a time looks
 * wasteful and is not: the VM calls this until it says no, so the loop is
 * the transfer, and doing it in bulk would need an interface that says how
 * many are coming, which the machine-independent side does not have.
 */
boolean_t
pmap_next_page(vm_offset_t *paddr)
{
	uint64_t	pa = boot_frame_alloc();

	if (pa == 0)
		return FALSE;

	*paddr = (vm_offset_t) pa;
	return TRUE;
}

/*
 * How many are left.  Used to size the page array before the transfer above
 * begins, so it must not be an underestimate -- a short array is a buffer
 * overrun in vm_page_bootstrap(), not a smaller page cache.
 */
unsigned int
pmap_free_pages(void)
{
	return (unsigned int) (boot_frames_total() - boot_frames_used());
}

/*
 * The kernel virtual addresses the VM may allocate from.
 *
 * ⚠️ The heap, and not the whole kernel half.  Everything else in the upper
 * half is already spoken for by the layout -- the direct map, the per-CPU
 * blocks, the entry stacks, the device map, the image -- and handing any of
 * it to the VM would let kmem_alloc() return an address that already means
 * something else.  <pmap/layout.h> is where those boundaries live and this
 * is the one range it leaves free.
 */
void
pmap_virtual_space(vm_offset_t *startp, vm_offset_t *endp)
{
	*startp = (vm_offset_t) KERNEL_HEAP_BASE;
	*endp   = (vm_offset_t) (KERNEL_HEAP_BASE + KERNEL_HEAP_SIZE);
}

/*
 * Advisory: the caller is telling the machine that a range will or will not
 * be paged.  Nothing to do here, and nothing on i386 either.
 *
 * It is a hint from an era when a machine might have wanted to pre-build
 * page tables for a range about to be wired, and this one does not: tables
 * are built on demand by pmap_enter() and there is nothing cheaper to do
 * with advance notice.  Kept because the interface has it, empty because
 * the honest answer is that it costs nothing to ignore.
 */
void
pmap_pageable(pmap_t pmap, vm_offset_t start, vm_offset_t end,
	      boolean_t pageable)
{
	(void) pmap;
	(void) start;
	(void) end;
	(void) pageable;
}

/*
 * ══ Giving back page-table pages that no longer map anything (#455) ═══
 *
 * A space that maps a wide range and then unmaps it keeps every table that
 * mapped it -- up to 2 MiB of tables per GiB of range touched -- and until
 * now this machine gave none of it back.  kern/task.c and kern/thread_swap.c
 * call this under memory pressure, so the cost was paid by whoever needed
 * memory next.
 *
 * 🔑 The hard part is not other threads.  It is that THE HARDWARE WALKS THESE
 * TABLES TOO, and the MMU passes no quiescent state, takes no lock and cannot
 * be asked to wait.  Any scheme that only orders software is wrong here, which
 * is what makes this different from every other reclaim in the kernel.
 *
 * ── The discipline, in the order it has to happen ────────────────────
 *
 *   1. MARK.  cmpxchg the parent entry from `table | VALID' to
 *      `table | VALID | COLLECT'.  The table stays linked and stays valid:
 *      nothing changes for the hardware or for a reader, and the only thing
 *      that has happened is that a claim has been staked.
 *
 *   2. WAIT A GRACE PERIOD -- urmach_synchronize_rcu().  This is the step
 *      that is easy to leave out and impossible to do without.  A writer that
 *      read the parent entry BEFORE the mark did not see it and is entitled to
 *      write into the table; when synchronize returns, every such writer has
 *      finished, because map.c holds a read section across its whole descent
 *      and leaf write.  A writer that starts AFTER the mark sees it and clears
 *      it (next_table), which is step 4's business.
 *
 *   3. SCAN.  Now, and not before: an emptiness test taken before step 2 is a
 *      value that was true once.
 *
 *   4. UNLINK.  cmpxchg the parent from `table | VALID | COLLECT' to zero.  It
 *      succeeds only if the bit is still set -- that is, only if no descent has
 *      passed through since step 1 -- so the scan in step 3 and this exchange
 *      are atomic with respect to insertions.  ⇒ THE EMPTINESS TEST AND THE
 *      UNLINK ARE ONE OPERATION, which is what they had to be and what the
 *      parent entry alone could not give: the inserter writes INSIDE the table
 *      and the collector exchanges the PARENT, so they never met.
 *
 *   5. SHOOT DOWN.  Until this returns, a processor's TLB can still hold a
 *      translation derived THROUGH this table, and an in-flight hardware walk
 *      can still be inside it.  Step 4 removed the way to reach it; only this
 *      removes the copies already taken.  ⚠️ #439 is the size of it: the
 *      shootdown goes to every processor rather than the ones using the pmap,
 *      so on 64 it costs 63 interrupts.  A cost question, not a correctness
 *      one, and batching amortises it.
 *
 *   6. WAIT A SECOND GRACE PERIOD, for the READERS -- pmap_walk()'s callers,
 *      which hold a pointer into the table after the walk returns.
 *
 *   7. FREE.  Not before.
 *
 * ⚠️ Steps 2 and 6 are different waits and neither replaces the other.  The
 * first orders the scan against writers; the second orders the free against
 * readers.  They are batched -- one pair per group of tables rather than per
 * table -- which is why this collects into an array instead of one at a time.
 *
 * ── What a concurrent pmap_enter is guaranteed to see ────────────────
 *
 * It reads the parent entry once, and gets one of three answers:
 *
 *   - the table with COLLECT clear.  Its read section began before the mark
 *     (or there was no mark), so step 2 waits for it and step 3 sees whatever
 *     it wrote.  The table is not freed.
 *
 *   - the table with COLLECT set.  It clears the bit, and step 4 fails.  If
 *     its clear loses the race to step 4, the cmpxchg tells it so and it looks
 *     again -- next_table() is a loop for that reason, and using the value it
 *     had read would have put a mapping into a page on its way back to the VM.
 *
 *   - zero.  It builds a fresh table and publishes it with cmpxchg, which is
 *     step 4's exchange seen from the other side.
 *
 * There is no fourth answer, and in particular none that is a pointer to a
 * freed table -- which is the whole property.
 *
 * ── Why not #338's per-table locks, and why not i386's type stability ─
 *
 * i386 reclaims under PMAP_READ_LOCK plus the per-table locks of #338,
 * arrived at by measurement on eight processors.  The issue asks whether that
 * granularity is still right at NCPUS=64 rather than merely inherited.  It is
 * not the arrangement here, and the reason is where the cost lands: a lock
 * puts it on the common path to spare the rare one.  Here the reader never
 * excludes anybody (a walk is a read section), the writer pays one bit test
 * per interior entry, and the collector pays for the rarity of its own
 * operation -- two grace periods and a shootdown per batch.
 *
 * The other candidate was a GENERATION counter in the parent's spare bits,
 * bumped by every inserter so that the collector's exchange would see an
 * insertion it could not otherwise detect.  It works, and it costs one extra
 * cmpxchg on EVERY pmap_enter, for ever, to make a rare operation cheap.  The
 * bit is strictly better on the same reasoning: it is written only when a
 * collection is actually in progress, so the inserter pays a predictable
 * branch rather than an atomic.  ⚠️ And the generation would have been ABA --
 * fourteen spare bits is a width, not an invariant -- while a single-bit claim
 * has nothing to wrap.
 *
 * i386's own answer to the reader race is neither: #330 makes reclaimed PT
 * pages TYPE-STABLE, never returning them to the VM allocator, so a stale
 * reader is not excluded and not waited for but made HARMLESS -- whatever it
 * reads is still a page-table entry.  That is cheaper than a grace period and
 * it gives nothing back to the system: the pool never shrinks, and the pageout
 * daemon asking for memory would get a pmap-private free list.  The grace
 * period is what lets step 7 be a real vm_page_free().
 * ⚠️ Type stability would NOT have removed step 5.  The hardware walker is not
 * a stale reader that can be satisfied with plausible bytes: a TLB entry
 * derived through a recycled table would translate a live address into another
 * space's page.  The shootdown is required by either answer.
 *
 * ── The reference bit: used, as a second chance ──────────────────────
 *
 * An interior entry gets ACCESSED set by the processor whenever it walks
 * through it, INCLUDING on a walk that goes on to fault -- which is the case
 * that matters here, since the tables being considered are empty.  So the bit
 * answers "is this space still faulting around here", and a table whose parent
 * has it set is one about to be filled again.  The bit is cleared and the
 * table left alone; the next call decides.  i386 uses it the same way and
 * counts the two outcomes.
 *
 * ⚠️ THE STALE-TLB INTERACTION, which the issue asks to be written down: the
 * processor sets ACCESSED when it WALKS, and it does not walk when the
 * translation is already cached.  So a table can be in active use through
 * cached translations while its parent's bit stays clear, and the hint says
 * "cold" about something warm.  That costs a table that will be rebuilt; it
 * cannot cost correctness, because nothing in steps 1-7 consults the bit --
 * the emptiness test and the shootdown do not care why the table was chosen.
 * The hint decides WHETHER to try, never WHETHER IT IS SAFE.
 */

/*
 * How many tables one call may take away.
 *
 * A bound rather than a buffer size: the walk stops when the batch is full and
 * the call returns, so the cost of one pmap_collect() is bounded no matter how
 * large the space is.  The interface allows it -- "success need not be
 * guaranteed ... there may well be pages which are not referenced, but others
 * may be collected" -- and the pageout daemon calls again while the pressure
 * lasts.
 *
 * It is also why a table emptied BY THIS CALL is collected by the next one: a
 * page directory only becomes empty once its page tables are freed, which
 * happens after the scan that would have considered it.
 */
#define PMAP_COLLECT_BATCH	32

struct collect_batch {
	/*
	 * Whose tables these are (#439).  A batch is built while walking one
	 * address space and freed for that one space, so the shootdown at the
	 * end of collect_flush() can be narrowed to the processors that could
	 * be holding its translations -- which it could not, while the only
	 * thing here was a list of frames.
	 */
	pmap_t		pmap;
	struct {
		pt_entry_t	*parent;
		uint64_t	 table_pa;
	} slot[PMAP_COLLECT_BATCH];
	unsigned	count;
	unsigned	freed;
};

/*
 * ⚠️ Zero, not "no valid entry".  A non-zero entry that is not valid is still
 * something somebody wrote, and freeing the table under it would discard it.
 * The test says what it means: nothing here at all.
 */
static boolean_t collect_table_empty(uint64_t table_pa)
{
	const pt_entry_t	*table;
	unsigned		 i;

	table = (const pt_entry_t *)(uintptr_t)phys_to_direct(table_pa);

	for (i = 0; i < PTES_PER_TABLE; i++)
		if (table[i] != 0)
			return FALSE;

	return TRUE;
}

/*
 * Drop the claim, for a table that turned out not to be collectable.
 *
 * It must happen on every path that does not unlink, and not as tidiness: a
 * COLLECT left set is a claim a LATER collector would find already staked, and
 * an exchange to zero that succeeds without a fresh grace period behind it.
 */
static void collect_unmark(pt_entry_t *parent)
{
	pt_entry_t	found = *parent;

	for (;;) {
		pt_entry_t	seen;

		if (!(found & INTEL_PTE_COLLECT))
			return;

		seen = atomic_cmpxchg64((volatile uint64_t *) parent, found,
					found & ~INTEL_PTE_COLLECT);
		if (seen == found)
			return;
		found = seen;
	}
}

/*
 * Step 4.  TRUE means this call took the table out of the tree.
 *
 * ⚠️ The retry is not the writer-writer race, it is the HARDWARE: a processor
 * walking through this entry sets ACCESSED in it without asking, and an
 * exchange that insisted on the exact value it read first would fail for that
 * alone.  What may not change is the claim and the frame, so those are what is
 * re-checked; anything else the word has picked up is carried into the attempt.
 */
static boolean_t collect_unlink(pt_entry_t *parent, uint64_t table_pa)
{
	pt_entry_t	found = *parent;

	for (;;) {
		pt_entry_t	seen;

		if (!(found & INTEL_PTE_COLLECT))
			return FALSE;		/* a descent took it back */
		if (pte_to_pa(found) != table_pa)
			return FALSE;		/* not our table any more */

		seen = atomic_cmpxchg64((volatile uint64_t *) parent, found, 0);
		if (seen == found)
			return TRUE;
		found = seen;
	}
}

/*
 * Steps 2 through 7 for everything marked so far.
 *
 * ⚠️ Runs OUTSIDE any read section, and every one of the three reasons is
 * fatal on its own: urmach_synchronize_rcu() would wait for this processor to
 * report a quiescent state it cannot report while holding one; tlb_flush_all()
 * waits for processors that may themselves be inside synchronize; and
 * pmap_table_frame_free() takes mutexes and may block.
 */
static void collect_flush(struct collect_batch *b)
{
	unsigned	i, kept = 0;

	if (b->count == 0)
		return;

	/*
	 * In the lock arm the tables are already unlinked: they were scanned
	 * and unlinked with every inserter excluded, so steps 2 to 4 have
	 * nothing left to do.  Steps 5 to 7 are the same in both arms, because
	 * they answer the hardware and the readers rather than the writers, and
	 * neither of those is excluded by anything.
	 */
	if (pmap_writer_arm != PMAP_ARM_PMAP_LOCK) {
		urmach_synchronize_rcu();		/* step 2 */

		for (i = 0; i < b->count; i++) {	/* steps 3 and 4 */
			if (!collect_table_empty(b->slot[i].table_pa)) {
				collect_unmark(b->slot[i].parent);
				continue;
			}
			if (!collect_unlink(b->slot[i].parent,
					    b->slot[i].table_pa))
				continue;

			b->slot[kept++] = b->slot[i];
		}

		b->count = kept;
		if (b->count == 0)
			return;
	}

	tlb_flush_all(b->pmap);				/* step 5 */
	urmach_synchronize_rcu();			/* step 6 */

	for (i = 0; i < b->count; i++) {		/* step 7 */
		pmap_table_frame_free(b->slot[i].table_pa);
		atomic_add32((volatile uint32_t *) &pmap_table_frames_live,
			     (uint32_t) -1);
	}

	b->freed += b->count;
	b->count = 0;
}

/* Step 1, plus the two cheap refusals that come before it. */
static void collect_consider(pt_entry_t *parent, struct collect_batch *b)
{
	pt_entry_t	found = *parent;
	uint64_t	table_pa = pte_to_pa(found);

	/*
	 * ── The other arm, and it is here to be measured (#455) ──────
	 *
	 * With one lock for the whole address space held across this scan, no
	 * inserter can be inside any of these tables, so the claim and the
	 * grace period that waits for pre-claim writers are both unnecessary:
	 * the emptiness test and the unlink are atomic because nothing else is
	 * running.  The collector gets cheaper -- one grace period instead of
	 * two, and no second scan -- and every pmap_enter in the system pays
	 * for it.  Which trade is right is what collect_bench.c measures.
	 */
	if (pmap_writer_arm == PMAP_ARM_PMAP_LOCK) {
		if (!collect_table_empty(table_pa))
			return;

		if (found & INTEL_PTE_REF) {
			*parent = found & ~INTEL_PTE_REF;
			return;
		}

		*parent = 0;
		b->slot[b->count].parent   = parent;
		b->slot[b->count].table_pa = table_pa;
		b->count++;
		return;
	}

	/*
	 * Somebody else's claim.  Only one collector may hold a table at a
	 * time, and requiring the bit to be CLEAR before setting it is what
	 * says so -- an unconditional set would let two collectors believe
	 * they had each staked it, and the second exchange to zero would run
	 * without a grace period of its own behind it.
	 */
	if (found & INTEL_PTE_COLLECT)
		return;

	if (!collect_table_empty(table_pa))
		return;

	/* The second chance.  See the reference-bit note above. */
	if (found & INTEL_PTE_REF) {
		atomic_cmpxchg64((volatile uint64_t *) parent, found,
				 found & ~INTEL_PTE_REF);
		return;
	}

	if (atomic_cmpxchg64((volatile uint64_t *) parent, found,
			     found | INTEL_PTE_COLLECT) != found)
		return;

	b->slot[b->count].parent   = parent;
	b->slot[b->count].table_pa = table_pa;
	b->count++;
}

/*
 * Every interior entry below `table', children before parents, stopping when
 * the batch is full.
 *
 * `level' is the level of the table itself: 4 for the PML4, 3 for a PDPT, 2
 * for a page directory.  Its entries name tables one level down, and an entry
 * at level 2 names a page table, which has no interior entries of its own.
 *
 * `last' bounds the indices considered, which matters only at the root: the
 * upper half is the kernel's, shared into every space, and the same bound
 * pmap_destroy() uses keeps this out of it.
 */
static void collect_scan(pt_entry_t *table, unsigned level, unsigned last,
			 struct collect_batch *b)
{
	unsigned	i;

	for (i = 0; i < last; i++) {
		pt_entry_t	found = table[i];

		if (b->count == PMAP_COLLECT_BATCH)
			return;

		/* A large page is a leaf: there is no table under it. */
		if (!pte_is_valid(found) || pte_is_leaf(found))
			continue;

		if (level > 2)
			collect_scan((pt_entry_t *)(uintptr_t)
				     phys_to_direct(pte_to_pa(found)),
				     level - 1, PTES_PER_TABLE, b);

		if (b->count == PMAP_COLLECT_BATCH)
			return;

		collect_consider(&table[i], b);
	}
}

void
pmap_collect(pmap_t pmap)
{
	struct collect_batch	batch;
	boolean_t		held;

	if (pmap == PMAP_NULL)
		return;

	/*
	 * Not the kernel's.  Its top-level entries were COPIED into every
	 * address space at pmap_create(), so unlinking one here would leave
	 * every existing space still pointing at the table -- the sharing that
	 * makes a trap keep running is exactly what makes the kernel half
	 * uncollectable by this mechanism.  i386 declines for its own reasons
	 * and this one is ours.
	 */
	if (pmap == pmap_kernel())
		return;

	/*
	 * Nothing to collect into before the VM exists, and no per-CPU area to
	 * take a read section in -- the same boundary pmap_read_enter() asks
	 * about, and the reason it is safe for the boot walkers to have none.
	 */
	if (!pmap_initialized)
		return;

	batch.pmap = pmap;
	batch.count = 0;
	batch.freed = 0;

	/*
	 * The scan is a reader like any other: the tables it walks through can
	 * be freed by a concurrent collector on the same space, and it is the
	 * section that stops that.  The flush must be outside it, so the two
	 * are sequential and the batch is what passes between them.
	 *
	 * ⚠️ The lower half only, and the bound is the same one pmap_destroy()
	 * uses.  Everything from KERNEL_PML4_FIRST up is the kernel's, shared
	 * into every space.
	 */
	held = pmap_read_enter();
	pmap_writer_lock(&pmap->collect_lock);
	collect_scan((pt_entry_t *)(uintptr_t)phys_to_direct(pmap->root_pa),
		     4, pml4_index(KERNEL_HALF_BASE), &batch);
	pmap_writer_unlock(&pmap->collect_lock);
	pmap_read_leave(held);

	collect_flush(&batch);
}

/*
 * Is this physical page mapped by nobody?
 *
 * Asked before a page is handed out again, so a wrong TRUE gives a task
 * somebody else's memory.  The physical index is the authority: it lists
 * every mapping of every managed page, so an empty list is the answer and
 * an unmanaged page has no list to be empty.
 */
boolean_t
pmap_verify_free(vm_offset_t paddr)
{
	if (!pv_managed((uint64_t) paddr))
		return TRUE;

	return pv_count((uint64_t) paddr) == 0;
}

boolean_t
pmap_page_still_mapped(vm_offset_t paddr)
{
	return !pmap_verify_free(paddr);
}

/*
 * Mark a range modified.
 *
 * The caller is about to write to the pages through some other mapping --
 * the direct map, typically -- and the hardware will not set the dirty bit
 * on the mapping the pager later looks at.  So it is set by hand, and it is
 * set on the entry rather than on a software page record because the pager
 * reads the entry.
 *
 * ⚠️ No TLB shootdown afterwards, and that is correct rather than an
 * omission: the dirty bit is one the processor only ever sets, never clears
 * or tests for permission, so a stale cached entry cannot cause a wrong
 * access.  Clearing it would be a different matter and would need one.
 */
void
pmap_modify_pages(pmap_t pmap, vm_offset_t s, vm_offset_t e)
{
	vm_offset_t	va;

	if (pmap == PMAP_NULL)
		return;

	/*
	 * ⚠️ x86_64_trunc_page and not trunc_page.  The machine-independent
	 * spelling reads `page_mask', a global the VM sets at startup -- so
	 * using it here would make this file depend on the VM being up in
	 * order to tell the VM about pages, and would put a memory load in
	 * front of every iteration of a loop whose page size is a constant on
	 * this machine (#453).
	 */
	for (va = x86_64_trunc_page(s); va < x86_64_round_page(e);
	     va += X86_64_PGBYTES) {
		pt_entry_t	*entry = pmap_walk(pmap->root_pa, va, 0);

		if (entry != PT_ENTRY_NULL && pte_is_valid(*entry))
			*entry |= INTEL_PTE_MOD;
	}
}
