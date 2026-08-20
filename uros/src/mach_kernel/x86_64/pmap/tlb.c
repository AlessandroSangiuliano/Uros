/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Taking a translation away from every processor that has it (#438, #407).
 */

#include <stdint.h>

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

static volatile uint64_t served[SMP_MAX_CPUS];

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
	served[percpu_apic_id()]++;
}

uint64_t tlb_flushes_served(uint32_t apic_id)
{
	if (apic_id >= SMP_MAX_CPUS)
		return 0;

	return atomic_load64(&served[apic_id]);
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
	 * This processor first.  Not for correctness — the order between the
	 * local flush and the remote ones does not matter, since the entry is
	 * already gone from the table by the time either happens — but because
	 * the cross-call spends the wait spinning, and doing the local work
	 * first means it is done by the time the answers arrive.
	 */
	tlb_flush_local_range(va, size);

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
	if (pmap == PMAP_NULL || pmap == pmap_kernel()) {
		/*
		 * Costs nothing while there is nobody else: ipi_call_others()
		 * returns at once when this is the only processor online.
		 */
		ipi_call_others(tlb_flush_handler, &r);
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
	 * Nobody has this space loaded, so the local flush above was the whole
	 * job.  This is the ORDINARY case, not an edge one: a freshly forked
	 * address space is loaded on the processor doing the forking and on no
	 * other, and that processor's own bit is not in the set it would send
	 * to anyway.
	 *
	 * ⚠️ It is also what keeps the boot self-tests working, and that is
	 * worth naming rather than leaving as a happy accident.  They build
	 * pmaps and unmap from them before percpu_activate() has run, and
	 * ipi_call_mask() needs this processor's APIC id to strike its own bit
	 * out -- which it reads from the per-CPU block that does not exist yet.
	 * Returning here means it is never asked.  A pmap can only have a bit
	 * set by pmap_activate(), which cannot run before that block exists, so
	 * "the set is empty" and "there is no block" cannot come apart.
	 */
	if (using == 0)
		return;

	ipi_call_mask(using, tlb_flush_handler, &r);
}

void tlb_flush_all(struct pmap *pmap)
{
	tlb_flush_range(pmap, 0, 0);
}
