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

#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>
#include <pmap/pv.h>
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
 * Give back page-table pages that no longer map anything.
 *
 * Nothing yet, and this one is a real gap rather than a hint that does not
 * apply: a space that maps and unmaps a large range keeps the empty tables
 * that mapped it, which is up to 2 MiB of tables per GiB of range touched.
 *
 * ⚠️ i386 DOES implement this -- intel/pmap.c walks the directory, finds
 * tables whose pages are unreferenced, and frees them under PMAP_READ_LOCK
 * and #338's per-table locks.  So the work is known to be possible and the
 * locking is known to have an answer; what this pmap has not got yet is that
 * answer, because the walk must be safe against another processor entering a
 * mapping into the table being freed.
 *
 * It is empty rather than absent because kern/task.c and kern/thread_swap.c
 * call it under memory pressure and a missing symbol would not link.  The
 * pageout daemon asking for memory back and getting none is a slower kernel;
 * a freed table another processor is walking into is a corrupted one.
 *
 * ▶️ #455 carries it.  The discipline it needs is written out below, because
 * the issue asks for a discipline and not for a lock -- and because two of the
 * three races it has to survive are already closed without one.
 *
 * ── THE DISCIPLINE FOR FREEING A PAGE TABLE ──────────────────────────
 *
 * 🔑 The hard part is not other threads.  It is that THE HARDWARE WALKS THESE
 * TABLES TOO, and the MMU passes no quiescent state, takes no lock and cannot
 * be asked to wait.  Any scheme that only orders software is wrong here, which
 * is what makes this different from every other reclaim in the kernel.
 *
 * Four steps, and the order is the whole of it:
 *
 *   1. UNLINK, with cmpxchg, from `table | VALID' to 0.  A plain store would
 *      lose whatever another processor put there; a failed exchange means the
 *      entry changed under us and the table is in use again, so abandon it.
 *      This is the same arbitration next_table() uses to publish (map.c), read
 *      in the other direction.
 *
 *   2. SHOOT DOWN, on every processor that may have this pmap loaded.  Until
 *      that returns, a processor's TLB can still hold a translation that was
 *      derived THROUGH this table, and an in-flight hardware walk can still be
 *      inside it.  Step 1 removed the way to reach it; only this removes the
 *      copies already taken.  ⚠️ #439 is the size of this: today the shootdown
 *      goes to every processor rather than the ones using the pmap, so on 64
 *      it costs 63 interrupts to reclaim one page.  That is a cost question,
 *      not a correctness one, and it does not gate this.
 *
 *   3. WAIT FOR SOFTWARE WALKERS -- urmach_synchronize_rcu().  A walk that
 *      read the parent entry before step 1 holds a pointer into the table and
 *      is entitled to finish.  QSBR says that when synchronize returns, every
 *      processor has passed a quiescent state, so no such walk is outstanding.
 *      ⚠️ The read section CANNOT go inside pmap_walk(), and getting that
 *      wrong is the easy mistake here.  pmap_walk() returns a POINTER INTO the
 *      table it found, and all nine of its callers dereference that pointer
 *      after it has returned -- so a section opened and closed inside the walk
 *      would end while the reference is still live, and protect the lookup
 *      instead of the use.  It would also look right.
 *
 *      The section belongs to the caller, spanning the walk AND every access
 *      through what it returned: pmap_protect_page and pmap_unmap_page in
 *      map.c, pmap_extract and pmap_page_protect's pv loop in pmap.c, and the
 *      rest of the nine.  That is the one piece of this that is code rather
 *      than ordering, and it is nine small edits rather than one.
 *
 *      🔥 AND IT CANNOT BE DONE BEFORE THE PER-CPU DATA EXISTS.  Tried, and
 *      the boot died in the pmap self-tests with garbage on the console:
 *      urmach_rcu_read_lock() is `cpu_data[cpu_number()].rcu_read_depth++',
 *      and x86_64/boot/boot_c.c exercises pmap_split_page() long before
 *      cpu_data is set up -- so the increment landed in whatever that address
 *      happened to be.  A read section is not free of prerequisites just
 *      because it compiles to one increment.
 *
 *      ⇒ the sections have to be either gated on the system being up, or
 *      confined to the callers that only run after it is.  The boot-time
 *      walkers do not need them: nothing collects during the self-tests.
 *
 *      ⚠️ It depends on the kernel being non-preemptive.  rcu.h says so in as
 *      many words -- rcu_read_depth is per-CPU and "if this kernel becomes
 *      preemptive this per-CPU scheme breaks" -- and MACH_RT is 0 here, which
 *      is what makes it true rather than hoped.  A depth counted per processor
 *      by a thread that can move between processors counts nothing.
 *
 *   4. FREE the frame.  Not before.
 *
 * ── WHAT A CONCURRENT pmap_enter IS GUARANTEED TO SEE ────────────────
 *
 * It reads the parent entry once, and gets one of two answers:
 *
 *   - the table, because its read happened before step 1.  The table is still
 *     allocated -- step 4 has not run and cannot until step 3 lets it -- so
 *     the entry it writes lands in a live table.  The collector's cmpxchg in
 *     step 1 then fails, because installing a mapping does not change the
 *     parent entry but the RCU read section keeps the grace period open, and
 *     a collector that got past step 1 before the mapping was written would
 *     free a table with a live entry in it.  ⇒ THE EMPTINESS TEST AND THE
 *     UNLINK MUST BE ONE OPERATION, or the test is a value that was true once.
 *
 *   - zero, because its read happened after.  next_table() then creates a
 *     fresh table and publishes it with cmpxchg, which is step 1's exchange
 *     seen from the other side: exactly one of the two wins the entry.
 *
 * There is no third answer, and in particular there is no answer that is a
 * pointer to a freed table -- which is the whole property.
 *
 * ── 🔥 AND THERE IS A CHEAPER ANSWER THAN ALL OF IT ──────────────────
 *
 * I wrote "i386 solves this with a lock" three times before reading what i386
 * actually does about THIS race, and it is not a lock.  #330:
 *
 *   "pmap_extract() now walks page tables lock-free.  A concurrent
 *    pmap_collect() can unlink the PDE of an empty PT page while a reader
 *    still holds a pointer into it.  To make that safe WITHOUT a grace period
 *    ... reclaimed PT pages are never returned to the VM allocator:
 *    pmap_collect() pushes them here and pmap_expand() pops them back ... so a
 *    stale reader only ever dereferences PT-SHAPED MEMORY (an empty PT page
 *    reads as invalid PTEs; a recycled one reads as some other pmap's PTEs --
 *    never freed or repurposed memory)."
 *
 * TYPE STABILITY.  The stale reader is not excluded and not waited for -- it is
 * made HARMLESS.  Whatever it reads is a page-table entry, valid or not, and a
 * walk that lands on some other space's mapping returns a wrong answer to a
 * question about an address that is no longer mapped, which the caller was
 * already prepared for.  No section, no shootdown ordering for the software
 * side, no grace period.
 *
 * ⚠️ Its stated reason -- that a grace period is what "a non-preemptive kernel
 * cannot provide cheaply (see #336)" -- may no longer hold: kern/rcu.c gives
 * this kernel a QSBR synchronize that #330 did not have.  So the choice is
 * open rather than settled, and it is the one the issue means by "decided with
 * a number in front of it":
 *
 *   - type stability costs a pool that never shrinks and a walk that can read
 *     another space's entries;
 *   - the grace period costs read sections on every walker and a
 *     synchronize per collection, and gives back memory for real.
 *
 * ⚠️ Type stability does NOT remove step 2.  The hardware walker is not a
 * stale reader that can be satisfied with plausible bytes: a TLB entry derived
 * through a recycled table would translate a live address to another space's
 * page.  The shootdown is required by either answer.
 *
 * ── 🔥 WHAT RCU DOES NOT SOLVE, AND IT IS THE HARD HALF ──────────────
 *
 * RCU answers READER against FREE.  pmap_collect() against pmap_enter() is
 * WRITER against WRITER, and no grace period touches it.
 *
 * The inserter writes INSIDE the table; the collector's cmpxchg is on the
 * PARENT entry.  They do not meet.  So this sequence is admissible:
 *
 *   collector: scans the table, finds it empty
 *   inserter:  reads the parent entry, gets the table
 *   collector: cmpxchg parent -> 0.  Succeeds: the parent never changed.
 *   inserter:  writes its mapping into the now-unlinked table
 *
 * The mapping is lost, silently, and the caller was told it succeeded.  A
 * second scan after synchronize_rcu() detects it -- by then the inserter has
 * finished -- but there is nothing useful to do with the answer: republishing
 * the table can lose to a next_table() that has already put a fresh one in
 * that slot, and freeing it drops the mapping.
 *
 * ⇒ THE EMPTINESS TEST AND THE UNLINK MUST EXCLUDE INSERTERS, not merely be
 * atomic with each other.  That is a writer-writer exclusion on the slot, and
 * it is the one place in this design where something has to be held.  It is
 * also small: it does not cover the walk, it does not cover the leaf writes,
 * and readers never take it -- which is what keeps the common path free.
 *
 * ⚠️ This is where i386's per-table locks (#338) come back, and on their own
 * terms rather than by inheritance.  The question the bench has to answer is
 * no longer "lock or no lock" -- it is how wide the writer-writer exclusion
 * has to be: one lock per pmap, one per table, or a per-table generation
 * counter the inserter bumps and the collector's cmpxchg includes.
 *
 * ── THE WRITER-WRITER PROTOCOL: A GENERATION IN THE PARENT ───────────
 *
 * The reason cmpxchg cannot see an insertion is that the inserter writes
 * inside the table and the collector exchanges the parent.  So make the
 * inserter touch the parent too, and the exchange sees everything.
 *
 * An interior entry has room: bits 11:9 are ignored by the hardware, and on
 * x86-64 so are 62:52.  An interior entry also carries no policy of its own --
 * map.c says so where it writes one -- so INTEL_PTE_WIRED, which spends bit 9
 * in a LEAF, is free here.  That is up to 14 bits with nothing else in them.
 *
 *   inserter (map.c, after installing a leaf):
 *       bump the generation in the parent entry, with cmpxchg
 *   collector:
 *       parent = *entry            (table | VALID | gen = N)
 *       scan the table; if not empty, done
 *       cmpxchg(entry, parent, 0)  -- fails if ANY insertion happened,
 *                                     because the generation moved
 *
 * The emptiness test and the unlink are now atomic with respect to insertions,
 * which is what "one operation" had to mean and what the parent alone could
 * not give.
 *
 * ⚠️ IT IS ABA, AND THE WIDTH IS THE WHOLE ARGUMENT.  A generation that wraps
 * back to N between the read and the exchange lets the collector free a table
 * that was refilled.  Three bits wrap after eight insertions, which is
 * reachable in a page fault storm; the eleven above them make it 2048, which
 * is not reachable in the window between two adjacent instructions but is not
 * a proof either.  ⇒ USE ALL FOURTEEN, and say in the code that the safety is
 * a width and not an invariant -- a counter that cannot wrap would be a
 * different design (a pointer-sized side table), and it is not obviously worth
 * it.
 *
 * ⚠️ AND THE GENERATION COSTS EVERY MAPPING, not every collection.  One extra
 * cmpxchg on the parent per pmap_enter, forever, to make a rare operation
 * cheap.  That is the trade a per-table LOCK inverts: the lock costs the
 * collector and costs the inserter only when they meet.  Which is right is
 * exactly what collect_bench.c was built to answer, and it is now a question
 * with two implementable arms rather than a preference.
 *
 * ── WHY NOT #338's PER-TABLE LOCKS ───────────────────────────────────
 *
 * i386 reclaims under PMAP_READ_LOCK plus the per-table locks of #338, arrived
 * at by measurement on eight processors.  This kernel is NCPUS=64, and the
 * question the issue asks is whether that granularity is still the right one
 * rather than merely the inherited one.  What the four steps above show is
 * that the READER never needs to exclude anybody: walking is a read section,
 * publishing is a cmpxchg, and the collector pays for the rarity of its own
 * operation.  A lock would put the cost on the common path to spare the rare
 * one.  x86_64/pmap/collect_bench.c is the load that decides it, and it is
 * already written.
 */
void
pmap_collect(pmap_t pmap)
{
	(void) pmap;
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
