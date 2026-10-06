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
#include <pmap/pmap.h>	/* cpus_using, pmap_kernel (#439) */
#include <pmap/pte.h>
#include <pmap/tlb.h>
#include <sync/atomic.h>

/* What one processor is being asked to discard. */
struct tlb_request {
	uint64_t va;
	uint64_t size;		/* zero means everything */
};

static struct answer_count served;

void tlb_flush_local_all(void)
{
	uint64_t cr4 = read_cr4();

	/*
	 * Reloading CR3 is the usual way to discard everything, and it has one
	 * exception that is easy to be wrong about for a long time: an entry
	 * marked global survives it.  That is the whole purpose of marking one
	 * global, and it means a kernel that turns global pages on quietly
	 * acquires a flush that no longer flushes the kernel's own mappings —
	 * which are exactly the ones this kernel changes.
	 *
	 * Nothing sets CR4.PGE yet; #437 is where that happens.  Rather than
	 * leave a trap for it, ask: with global pages on, clearing and
	 * restoring the enable bit discards them too, and is the architecture's
	 * own answer for this.  One read of a control register to be right in
	 * both worlds.
	 */
	if (cr4 & CR4_PGE) {
		write_cr4(cr4 & ~CR4_PGE);
		write_cr4(cr4);
		return;
	}

	write_cr3(read_cr3());
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

static void tlb_flush_handler(void *arg)
{
	const struct tlb_request *r = arg;

	tlb_flush_local_range(r->va, r->size);
	answer_count_mark(&served);
}

uint64_t tlb_flushes_served(uint32_t apic_id)
{
	return answer_count_of(&served, apic_id);
}

#ifndef	ABLATE_638_FLUSH_UNPINNED
#define	ABLATE_638_FLUSH_UNPINNED	0
#endif

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
 * CR3.  So both halves run with preemption off.  UROS_ABLATE_638_FLUSH_UNPINNED
 * runs them with it on again.
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
	if (!ABLATE_638_FLUSH_UNPINNED) {
#if	WIDEN_638_WINDOW
		shootdown_widen();	/* before deciding: a move is harmless */
#endif
		disable_preemption();
	}

	tlb_flush_local_range(r->va, r->size);

#if	WIDEN_638_WINDOW
	if (ABLATE_638_FLUSH_UNPINNED)
		shootdown_widen();	/* between the local flush and the call */
#endif

	if (broadcast)
		ipi_call_others(tlb_flush_handler, r);
	else
		ipi_call_mask(using, tlb_flush_handler, r);

	if (!ABLATE_638_FLUSH_UNPINNED)
		enable_preemption();
}

void tlb_flush_range(struct pmap *pmap, uint64_t va, uint64_t size)
{
	/*
	 * On the stack, read by other processors — which is safe for exactly
	 * one reason: the cross-call does not return until every one of them
	 * has finished with it, so this frame outlives every reader of it.
	 */
	struct tlb_request r = { va, size };

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
	 * ── Putting the broadcast back, to find out what it cost (#482) ────
	 *
	 * #439 narrowed this on an ARGUMENT: the page copy did not move a cycle
	 * between one processor and eight while the whole fault rose thirty
	 * times over, and the shootdown was the one part of the path whose cost
	 * is a function of processor count.  The narrowing worked -- 35,490 down
	 * to 5,790 -- and a result is not a premise.
	 *
	 * The phase breakdown cannot confirm the premise on its own, because
	 * what it now measures is a fault with no broadcast in it: the growth
	 * the argument was about is gone, and "nothing grows" is consistent with
	 * the argument having been right and with its having been right for the
	 * wrong reason.  🔥 A correction is verified by TAKING IT AWAY.
	 *
	 * So this switch restores the broadcast and nothing else -- same kernel,
	 * same run, same instrument -- and the question becomes an observation:
	 * does FP_PROTECT plus FP_ENTER swell while every other phase stays
	 * where it was?  Off in every build that is not answering that question.
	 */
#ifndef	ABLATE_439
#define	ABLATE_439	0
#endif

	if (ABLATE_439 || pmap == PMAP_NULL || pmap == pmap_kernel()) {
		/*
		 * With only this processor online there is nobody to tell and
		 * nowhere to be moved to, so the flush here is the whole job.
		 * It is also the only case before percpu_activate(), where %gs
		 * is based at zero and raising the preemption count would
		 * write at address zero's page (#638).
		 */
		if (smp_online_count() <= 1) {
			tlb_flush_local_range(va, size);
			return;
		}
		tlb_flush_here_and_there(&r, 1, 0);
		return;
	}

	/*
	 * Read once.  A processor joining the set after this load is a
	 * processor that is about to load CR3 with this root — and CR3 is
	 * written after the entry we just changed was already gone from the
	 * table, so what it walks is the new state.  It has nothing stale to
	 * discard, which is why missing it here is not a hole.
	 *
	 * A processor LEAVING after this load still gets its interrupt and
	 * flushes something it no longer needs, which costs a message.
	 */
	using = atomic_load64(&pmap->cpus_using);

	/*
	 * Nobody has this space loaded, so the flush here is the whole job.
	 * This is the ORDINARY case, not an edge one: a freshly forked
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
