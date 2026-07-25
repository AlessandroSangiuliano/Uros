/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 pmap: the machine-dependent physical map (#407, MD contract 2/6).
 */

#include <stdint.h>

#include <cpu/regs.h>
#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/map.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>
#include <pmap/pv.h>
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

/*
 * Somewhere to keep the pmaps themselves until the MI allocator exists.  A
 * slot with no references is free, so a destroyed space's slot comes back —
 * unlike its frames, which do not.
 */
#define PMAP_BOOT_MAX	16

static struct pmap pmap_pool[PMAP_BOOT_MAX];

/*
 * The first PML4 slot of the kernel half.  Entries from here up are the
 * kernel's and are shared into every space; everything below is the space's
 * own.  Deriving it from the layout keeps the split in one place: five-level
 * paging moves the boundary along with KERNEL_HALF_BASE.
 */
#define KERNEL_PML4_FIRST	pml4_index(KERNEL_HALF_BASE)

_Static_assert(KERNEL_PML4_FIRST == 256, "the halves are not evenly split");

pmap_t pmap_create(uint64_t size)
{
	const pt_entry_t *kroot;
	pt_entry_t *root;
	uint64_t root_pa;
	pmap_t pmap = PMAP_NULL;

	/* One size of address space on this machine; the argument is the
	 * interface's, not ours. */
	(void)size;

	for (unsigned i = 0; i < PMAP_BOOT_MAX; i++)
		if (pmap_pool[i].ref_count == 0) {
			pmap = &pmap_pool[i];
			break;
		}

	if (pmap == PMAP_NULL)
		return PMAP_NULL;

	root_pa = boot_frame_alloc();		/* arrives zeroed */
	if (root_pa == 0)
		return PMAP_NULL;

	/*
	 * Share the kernel half by pointing at the same next-level tables, not
	 * by copying them: a mapping the kernel changes later is then seen by
	 * every space at once, because they are all reading the one table.
	 * Only these top-level entries are duplicated.
	 */
	root = (pt_entry_t *)(uintptr_t)phys_to_direct(root_pa);
	kroot = (const pt_entry_t *)(uintptr_t)
		phys_to_direct(kernel_pmap_store.root_pa);

	for (unsigned i = KERNEL_PML4_FIRST; i < PTES_PER_TABLE; i++)
		root[i] = kroot[i];

	pmap->root_pa = root_pa;
	pmap->ref_count = 1;
	return pmap;
}

void pmap_reference(pmap_t pmap)
{
	if (pmap != PMAP_NULL)
		pmap->ref_count++;
}

void pmap_destroy(pmap_t pmap)
{
	if (pmap == PMAP_NULL || pmap == pmap_kernel())
		return;

	if (--pmap->ref_count > 0)
		return;

	/*
	 * The space is gone as far as anyone can tell — the slot is free and
	 * the root unreachable — but its frames are not given back: the boot
	 * allocator has no way to take them.  Reclaiming them is the same
	 * bookkeeping pmap_collect() needs and arrives with the real physical
	 * allocator, not before.
	 */
	pmap->root_pa = 0;
}

void pmap_activate(pmap_t pmap)
{
	if (pmap != PMAP_NULL)
		write_cr3(pmap->root_pa);
}

/*
 * Drop the mapping at va and forget it in the physical index too.  Returns
 * the size removed, or zero if there was nothing there.
 *
 * The index is only told about pages it tracks: a large mapping is the
 * kernel's own — the direct map, the image — and belongs to no VM object, so
 * nothing will ever ask which pmaps hold it.
 */
static uint64_t pmap_forget(pmap_t pmap, uint64_t va)
{
	uint64_t pa = 0;
	uint64_t size;

	pmap_resolve(pmap->root_pa, va, &pa, 0);
	size = pmap_unmap_page(pmap->root_pa, va);

	if (size == PAGE_SIZE_4K)
		pv_remove(pa, pmap, va);

	return size;
}

int pmap_enter(pmap_t pmap, uint64_t va, uint64_t pa, vm_prot_t prot,
	       int wired)
{
	int rc;

	/*
	 * Wired is MI bookkeeping — a page the pager may not reclaim — with no
	 * effect on the hardware entry, so it is recorded on the MI side, not
	 * here.  Kept in the signature to match the interface.
	 */
	(void)wired;

	if (prot == VM_PROT_NONE) {
		pmap_forget(pmap, va);
		return PMAP_MAP_OK;
	}

	/*
	 * Whatever was here before is no longer mapped at this address, so the
	 * index must lose that entry before it gains the new one — otherwise
	 * replacing a mapping would leave the old page believing it still had
	 * one, and a later page_protect would walk to an address that no
	 * longer names it.
	 */
	pmap_forget(pmap, va);

	rc = pmap_map_page(pmap->root_pa, va, pa, pmap_flags_for_prot(prot));
	if (rc == PMAP_MAP_OK)
		pv_enter(pa, pmap, va);

	return rc;
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
		uint64_t sz = pmap_forget(pmap, s);

		s += sz ? sz : PAGE_SIZE_4K;
	}
}

void pmap_protect(pmap_t pmap, uint64_t s, uint64_t e, vm_prot_t prot)
{
	uint64_t flags = pmap_flags_for_prot(prot);

	while (s < e) {
		uint64_t sz;

		if (prot == VM_PROT_NONE)
			sz = pmap_forget(pmap, s);
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
