/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 pmap: the machine-dependent physical map (#407, MD contract 2/6).
 */

#include <stdint.h>

#include <cpu/regs.h>
#include <pmap/map.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>
#include <pmap/walk.h>

/*
 * The kernel pmap is a single object, not allocated: it has to exist before
 * any allocator does, and it lives as long as the kernel.  Its tables are
 * the ones boot.S and the direct map already built.
 */
static struct pmap kernel_pmap_store;

void pmap_bootstrap(void)
{
	kernel_pmap_store.root_pa = read_cr3() & INTEL_PTE_PFN;
	kernel_pmap_store.ref_count = 1;
}

pmap_t pmap_kernel(void)
{
	return &kernel_pmap_store;
}

int pmap_enter(pmap_t pmap, uint64_t va, uint64_t pa, vm_prot_t prot,
	       int wired)
{
	/*
	 * Wired is MI bookkeeping — a page the pager may not reclaim — with no
	 * effect on the hardware entry, so it is recorded on the MI side, not
	 * here.  Kept in the signature to match the interface.
	 */
	(void)wired;

	if (prot == VM_PROT_NONE) {
		pmap_unmap_page(pmap->root_pa, va);
		return PMAP_MAP_OK;
	}

	return pmap_map_page(pmap->root_pa, va, pa, pmap_flags_for_prot(prot));
}

uint64_t pmap_extract(pmap_t pmap, uint64_t va)
{
	uint64_t pa = 0;

	return pmap_resolve(pmap->root_pa, va, &pa, 0) ? pa : 0;
}

/*
 * Step a range one mapping at a time.  Advancing by the size the operation
 * reports skips a large page in one stride instead of touching its every
 * 4 KiB; where nothing is mapped, 4 KiB is the step that cannot overshoot a
 * mapping that starts partway through.
 */
void pmap_remove(pmap_t pmap, uint64_t s, uint64_t e)
{
	while (s < e) {
		uint64_t sz = pmap_unmap_page(pmap->root_pa, s);

		s += sz ? sz : PAGE_SIZE_4K;
	}
}

void pmap_protect(pmap_t pmap, uint64_t s, uint64_t e, vm_prot_t prot)
{
	uint64_t flags = pmap_flags_for_prot(prot);

	while (s < e) {
		uint64_t sz;

		if (prot == VM_PROT_NONE)
			sz = pmap_unmap_page(pmap->root_pa, s);
		else
			sz = pmap_protect_page(pmap->root_pa, s, flags);

		s += sz ? sz : PAGE_SIZE_4K;
	}
}
