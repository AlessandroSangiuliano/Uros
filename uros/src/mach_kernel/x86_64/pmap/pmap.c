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

/*
 * Section bounds from the linker (boot.ld), page-aligned so each belongs to
 * exactly one protection.
 */
extern char __ktext_start[], __ktext_end[];
extern char __krodata_start[], __krodata_end[];
extern char __kdata_start[], __kernel_end[];

void pmap_protect_kernel(void)
{
	pmap_t k = pmap_kernel();
	uint64_t t0 = (uint64_t)(uintptr_t)__ktext_start;
	uint64_t end = (uint64_t)(uintptr_t)__kernel_end;
	uint64_t va;

	/*
	 * The image rode in on one large writable-and-executable page (a few,
	 * if it ever outgrows 2 MiB).  Break every large page across it to
	 * 4 KiB first, so section boundaries that fall mid-large-page can each
	 * get their own permission.  Splitting an already-small page is a
	 * no-op, so this is safe whatever the page size turns out to be.
	 */
	for (va = t0 & ~(PAGE_SIZE_2M - 1); va < end; va += PAGE_SIZE_2M)
		pmap_split_page(k->root_pa, va);

	pmap_protect(k, t0, (uint64_t)(uintptr_t)__ktext_end,
		     VM_PROT_READ | VM_PROT_EXECUTE);
	pmap_protect(k, (uint64_t)(uintptr_t)__krodata_start,
		     (uint64_t)(uintptr_t)__krodata_end, VM_PROT_READ);
	pmap_protect(k, (uint64_t)(uintptr_t)__kdata_start, end,
		     VM_PROT_READ | VM_PROT_WRITE);

	/*
	 * Until now the kernel could write a page marked read-only: CR0.WP
	 * makes the read-only bit bind at ring 0 too, which is what turns the
	 * .text and .rodata protections from advisory into real.
	 */
	write_cr0(read_cr0() | CR0_WP);
}
