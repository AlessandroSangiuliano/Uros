/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Taking a translation away from every processor that has it (#438, #407).
 */

#include <stdint.h>

#include <kern/cpu_data.h>	/* #638: disable_preemption */

#include <cpu/answer_count.h>	/* #605: who answered, shared with ipi.c */
#include <cpu/ipi.h>
#include <cpu/percpu.h>
#include <cpu/regs.h>
#include <cpu/smp.h>
#include <pmap/layout.h>	/* KERNEL_IMAGE_BASE */
#include <pmap/pmap.h>	/* cpus_using, pmap_kernel (#439) */
#include <pmap/pte.h>
#include <pmap/tlb.h>
#include <sync/atomic.h>

/* What one processor is being asked to discard. */
struct tlb_request {
	uint64_t va;
	uint64_t size;		/* zero means everything */
	uint64_t ctx_id;	/* #412: the user space it is for, or 0 */
	uint64_t gen;		/* #412: the generation it brings that space to */
};

static struct answer_count served;

void tlb_flush_local_all(void)
{
	int was = interrupts_enabled();
	uint64_t cr4;

	/*
	 * ⚠️ ON ONE PROCESSOR, FROM THE READ TO THE LAST WRITE.  CR4 and CR3
	 * are read here and written back, and a thread moved in between would
	 * write one processor's values into another's: CR4 is not the same on
	 * every processor (#669: the boot processor keeps the MCE and DE bits
	 * the firmware left), and CR3 is the space a processor runs in.  Most
	 * callers hold the processor already; the shootdown of a space loaded
	 * nowhere (tlb_flush_range(), using == 0) did not.  Found by #412's
	 * review.
	 */
	interrupts_disable();
	cr4 = read_cr4();

	/*
	 * Reloading CR3 is the usual way to discard everything, and it has one
	 * exception that is easy to be wrong about for a long time: an entry
	 * marked global survives it.  That is the whole purpose of marking one
	 * global, and it means a kernel that turns global pages on quietly
	 * acquires a flush that no longer flushes the kernel's own mappings —
	 * which are exactly the ones this kernel changes.
	 *
	 * #437 turns CR4.PGE on, on every processor (pmap_control_regs_here()).
	 * With global pages on, clearing and restoring the enable bit discards
	 * them too, and is the architecture's own answer for this; one read of
	 * a control register keeps the flush right with them and without.
	 * ablations/437-flush-keeps-globals.patch takes this branch out.
	 */
	/*
	 * #412: with INVPCID, one instruction that discards every PCID's
	 * entries and the global ones, instead of two CR4 writes.  Without it
	 * the PGE toggle below already reaches every PCID: a CR4 write that
	 * changes PGE flushes the whole TLB, whatever CR3 names.  The CR3
	 * reload at the end reaches the current PCID only, and is reached only
	 * with global pages off, where pmap_enable_pcid() leaves PCID off too.
	 */
	if (pmap_pcid_mode == PMAP_PCID_INVPCID)
		invpcid(INVPCID_ALL_GLOBAL, 0, 0);
	else if (cr4 & CR4_PGE) {
		write_cr4(cr4 & ~CR4_PGE);
		write_cr4(cr4);
	} else
		write_cr3(read_cr3());

	if (was)
		interrupts_enable();
}

void tlb_flush_local_range(uint64_t va, uint64_t size)
{
	uint64_t pages;

	if (size == 0) {
		tlb_flush_local_all();
		return;
	}

	/* Round outward: a range that starts mid-page still covers that page. */
	pages = (((va & (PAGE_SIZE_4K - 1)) + size) + PAGE_SIZE_4K - 1)
		/ PAGE_SIZE_4K;

	if (pages > TLB_FLUSH_PAGE_LIMIT) {
		tlb_flush_local_all();
		return;
	}

	va &= ~(uint64_t)(PAGE_SIZE_4K - 1);
	while (pages--) {
		invlpg(va);
		va += PAGE_SIZE_4K;
	}
}

/* Whether a range is flushed page by page, the way tlb_flush_local_range() decides. */
static int tlb_range_by_pages(uint64_t va, uint64_t size)
{
	uint64_t pages = (((va & (PAGE_SIZE_4K - 1)) + size) + PAGE_SIZE_4K - 1)
			 / PAGE_SIZE_4K;

	return size != 0 && pages <= TLB_FLUSH_PAGE_LIMIT;
}

/*
 * A kernel-half range, or any range without PCID, on this processor.
 *
 * 🔴 NOT THE IMAGE'S WINDOW BY ADDRESS.  Its leaves are not global (it shares
 * a directory with the boot identity map, #640), so with PCID every space that
 * ran there holds a copy under its own PCID, and invlpg reaches the current
 * one's alone (SDM Vol. 3A 253668-093US 5.10.4.2): a range reaching into it
 * empties every PCID.  Nothing changes the window after boot today; this is so
 * that the first thing that does cannot leave its old translation under
 * somebody else's PCID.  Both ways a kernel range is flushed come here: the
 * request on every processor, and the one processor before the others start.
 */
static void tlb_flush_kernel_here(uint64_t va, uint64_t size)
{
	if (pmap_pcid_mode != PMAP_PCID_OFF
	    && (va >= KERNEL_IMAGE_BASE || KERNEL_IMAGE_BASE - va < size))
		tlb_flush_local_all();
	else
		tlb_flush_local_range(va, size);
}

/*
 * One request, on this processor: the local half of a shootdown, and the
 * whole of what the handler does.
 *
 * A kernel-half request (ctx_id 0), or any request without PCID, is the flush
 * it always was: kernel entries are global, and invlpg reaches a global entry
 * in every PCID -- on the processors PCID is left on for: pmap_pcid_erratum()
 * turns it off where Intel's INVLPG reaches the current PCID's alone, and
 * pmap_enable_pcid() refuses it without global pages.  Except the image's
 * window, whose leaves are not global (#640): tlb_flush_kernel_here() empties
 * every PCID for it.  A user space's request concerns this processor only if that space is
 * the one CR3 names -- asked of CR3, the one thing that cannot be ahead of or
 * behind the TLB, even at boot, where CR3 is written directly.  Then it
 * brings that PCID's generation forward:
 *
 *   - by the range alone, if it is the very next generation;
 *   - by discarding the whole PCID, if a generation in between has not
 *     arrived yet.  Two shootdowns of one space can reach a processor in
 *     either order, and advancing past one whose pages were never flushed
 *     here is how a stale entry would come to be trusted (#412).
 *
 * A space this processor only caches is left alone: its slot is behind the
 * space's generation, and the next switch into it discards its entries.
 * Interrupts off, so that a request landing in the middle cannot see the slot
 * and CR3 disagree either.
 */
static void tlb_flush_request_here(const struct tlb_request *r)
{
	struct percpu	*pc;
	uint64_t	 pcid, *gen;
	int		 was;

	if (r->ctx_id == 0 || pmap_pcid_mode == PMAP_PCID_OFF) {
		tlb_flush_kernel_here(r->va, r->size);
		return;
	}

	was = interrupts_enabled();
	interrupts_disable();

	pc = percpu();
	pcid = read_cr3() & CR3_PCID_MASK;
	if (pcid != 0 && pcid <= PCID_SLOTS
	    && pc->pcid_slot[pcid - 1].ctx_id == r->ctx_id) {
		gen = &pc->pcid_slot[pcid - 1].gen;
		if (*gen + 1 == r->gen && tlb_range_by_pages(r->va, r->size)) {
			tlb_flush_local_range(r->va, r->size);
			*gen = r->gen;
		} else if (*gen < r->gen) {
			write_cr3(read_cr3());	/* this PCID, globals kept */
			*gen = r->gen;
		}
	}

	if (was)
		interrupts_enable();
}

static void tlb_flush_handler(void *arg)
{
	tlb_flush_request_here(arg);
	answer_count_mark(&served);
}

uint64_t tlb_flushes_served(unsigned cpu)
{
	return answer_count_of(&served, cpu);
}

/*
 * Flush this processor, and have the others flush too: every one in `using',
 * or every one at all when `broadcast' is set.
 *
 * 🔴 ONE PROCESSOR FOR BOTH HALVES (#638).  The local flush and the cross-call
 * each mean "this processor": the first flushes it, the second leaves it out.
 * They must mean the same one, and at level zero they did not have to -- a
 * thread moved between the two had flushed the processor it left and left out
 * the one it arrived at, which nobody flushed.  That one can hold the
 * translation: a switch between two threads with the same map does not reload
 * CR3.  So both halves run with preemption off.
 * ablations/638-flush-unpinned.patch runs them with it on again.
 *
 * The local flush still goes first.  Not for correctness — the order between
 * the local flush and the remote ones does not matter, since the entry is
 * already gone from the table by the time either happens — but because the
 * cross-call spends the wait spinning, and doing the local work first means it
 * is done by the time the answers arrive.
 */
static void tlb_flush_here_and_there(struct tlb_request *r, int broadcast,
				     uint64_t using)
{
	disable_preemption();

	tlb_flush_request_here(r);

	if (broadcast)
		ipi_call_others(tlb_flush_handler, r);
	else
		ipi_call_mask(using, tlb_flush_handler, r);

	enable_preemption();
}

void tlb_flush_range(struct pmap *pmap, uint64_t va, uint64_t size)
{
	/*
	 * On the stack, read by other processors — which is safe for exactly
	 * one reason: the cross-call does not return until every one of them
	 * has finished with it, so this frame outlives every reader of it.
	 */
	struct tlb_request r = { va, size, 0, 0 };

	uint64_t using;

	/*
	 * ── Who else has to be told (#439) ────────────────────────────────
	 *
	 * PMAP_NULL means the caller has no address space object to name, or
	 * means the kernel's own: every processor keeps the kernel half loaded
	 * at all times, so for those mappings the broadcast is the right
	 * answer rather than a pessimistic one.
	 *
	 * ⚠️ pmap_kernel() is asked as well as PMAP_NULL, and it must be: the
	 * kernel pmap is a real object with a real cpus_using, and that set is
	 * NOT the set of processors holding kernel translations.  It records
	 * who has that root in CR3, which is nobody once user threads are
	 * running — every one of them is in some user space whose upper half
	 * is a copy of the kernel's.  Trusting the set here would silently
	 * stop shooting down kernel mappings on every processor in the
	 * machine, and the first symptom would be somewhere else entirely.
	 */

	/*
	 * 🔴 THE KERNEL HALF BY ADDRESS, WHATEVER PMAP ASKED.  Its tables are
	 * the same in every space and its leaves are global (#437), so a kernel
	 * address reached through a user's pmap -- pmap_may_map() lets one
	 * through, and the walk lands in the shared tables -- is a translation
	 * every processor may hold, under every PCID.  Sent to that pmap's
	 * cpus_using it would reach the processors with that space loaded and
	 * leave the rest.  No caller does this today; this is so that none can.
	 */
	if (size != 0 && va_is_kernel(va))
		pmap = PMAP_NULL;

	if (pmap == PMAP_NULL || pmap == pmap_kernel()) {
		/*
		 * With only this processor online there is nobody to tell and
		 * nowhere to be moved to, so the flush here is the whole job.
		 * It is also the only case before percpu_activate(), where %gs
		 * is based at zero and raising the preemption count would
		 * write at address zero's page (#638).
		 */
		if (smp_online_count() <= 1) {
			tlb_flush_kernel_here(va, size);
			return;
		}
		tlb_flush_here_and_there(&r, 1, 0);
		return;
	}

	/*
	 * Read once.  A processor joining the set after this load is a
	 * processor that is about to load CR3 with this root — and CR3 is
	 * written after the entry we just changed was already gone from the
	 * table, so what it walks is the new state.  Without PCID it has
	 * nothing stale to discard, which is why missing it here is not a
	 * hole.  With PCID it may: its slot can still hold the space's old
	 * entries, and what makes it discard them is the generation it reads
	 * after joining, which is the order the next paragraph is about.
	 *
	 * A processor LEAVING after this load still gets its interrupt and
	 * flushes something it no longer needs, which costs a message.
	 */
	/*
	 * 🔴 THE GENERATION FIRST, THEN THE SET (#412).  A processor switching
	 * into this space joins cpus_using and then reads tlb_gen; this side
	 * increments tlb_gen and then reads cpus_using.  Both are locked
	 * operations, full barriers here, so at least one side sees the other:
	 * either the switch finds the new generation and discards the space's
	 * entries, or this shootdown finds the processor and interrupts it.
	 * Reading the set first would let a processor slip between the two,
	 * keep its stale entries, and never be told -- FreeBSD shipped that
	 * order once (EN-22:04).  Without PCID the generation is never read,
	 * and is not touched.
	 */
	if (pmap_pcid_mode != PMAP_PCID_OFF) {
		r.ctx_id = pmap->ctx_id;
		r.gen = atomic_add64(&pmap->tlb_gen, 1) + 1;
	}

	using = atomic_load64(&pmap->cpus_using);

	/*
	 * Nobody has this space loaded, so there is nobody to interrupt: with
	 * PCID the generation moved above is what reaches the processors that
	 * only cache the space, and the flush here is for this processor, which
	 * may have loaded the root directly (the boot self-tests do).  This is
	 * the ORDINARY case, not an edge one: a freshly forked
	 * address space is loaded on the processor doing the forking and on no
	 * other, and that processor's own bit is not in the set it would send
	 * to anyway.
	 *
	 * ⚠️ It is also what keeps the boot self-tests working, and that is
	 * worth naming rather than leaving as a happy accident.  They build
	 * pmaps and unmap from them before percpu_activate() has run, and both
	 * holding this processor (#638) and ipi_call_mask() reach the per-CPU
	 * block, which does not exist yet.  Returning here means neither is
	 * asked.  A pmap can only have a bit set by pmap_activate(), which
	 * cannot run before that block exists, so "the set is empty" and
	 * "there is no block" cannot come apart.
	 */
	if (using == 0) {
		tlb_flush_local_range(va, size);
		return;
	}

	tlb_flush_here_and_there(&r, 0, using);
}

void tlb_flush_all(struct pmap *pmap)
{
	tlb_flush_range(pmap, 0, 0);
}
