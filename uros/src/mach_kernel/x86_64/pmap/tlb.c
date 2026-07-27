/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Taking a translation away from every processor that has it (#438, #407).
 */

#include <stdint.h>

#include <cpu/ipi.h>
#include <cpu/regs.h>
#include <cpu/smp.h>
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
	served[cpu_apic_id()]++;
}

uint64_t tlb_flushes_served(uint32_t apic_id)
{
	if (apic_id >= SMP_MAX_CPUS)
		return 0;

	return atomic_load64(&served[apic_id]);
}

void tlb_flush_range(uint64_t va, uint64_t size)
{
	/*
	 * On the stack, read by other processors — which is safe for exactly
	 * one reason: the cross-call does not return until every one of them
	 * has finished with it, so this frame outlives every reader of it.
	 */
	struct tlb_request r = { va, size };

	/*
	 * This processor first.  Not for correctness — the order between the
	 * local flush and the remote ones does not matter, since the entry is
	 * already gone from the table by the time either happens — but because
	 * the cross-call spends the wait spinning, and doing the local work
	 * first means it is done by the time the answers arrive.
	 */
	tlb_flush_local_range(va, size);

	/*
	 * Costs nothing while there is nobody else: ipi_call_others() returns
	 * at once when this is the only processor online, which is what lets
	 * every path through pmap use this form from the first page it maps.
	 */
	ipi_call_others(tlb_flush_handler, &r);
}

void tlb_flush_all(void)
{
	tlb_flush_range(0, 0);
}
