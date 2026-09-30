/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 page-table walk (#407, MD contract 2/6).
 */

#include <stdint.h>

#include <mach_assert.h>
#include <kern/misc_protos.h>		/* panic (#604) */
#include <kern/rcu.h>			/* urmach_rcu_read_held (#604) */
#include <pmap/layout.h>
#include <pmap/pmap.h>			/* pmap_initialized (#604) */
#include <pmap/pte.h>
#include <pmap/walk.h>

/*
 * A page table, by physical address, seen through the direct map.  This one
 * line is what the direct map bought: on i386 the same step needs a
 * temporary mapping or the self-map, and has to be undone afterwards.
 */
static inline pt_entry_t *table_at(uint64_t table_pa)
{
	return (pt_entry_t *)(uintptr_t)phys_to_direct(table_pa);
}

pt_entry_t *pmap_walk(uint64_t root_pa, uint64_t va, uint64_t *page_size_out)
{
	pt_entry_t *table;
	pt_entry_t *entry;

	/*
	 * 🔴 THE LOWER HALF IS WALKED INSIDE A READ SECTION (#604).
	 *
	 * pmap_collect() frees interior tables below KERNEL_HALF_BASE, in any
	 * space but the kernel's, and waits only for readers inside a read
	 * section (#455, step 6 in vminit.c).  A walk outside one can be
	 * standing in a table it frees, and read the next level out of a page
	 * that already belongs to somebody else.
	 *
	 * The upper half needs nothing: it is the kernel's, shared into every
	 * space and never collected.  That is what lets a fault report ask
	 * pmap_extract() about a kernel address without touching per-processor
	 * state it may not be able to trust.
	 *
	 * Checked, not asked for: three walkers had forgotten, and the audit
	 * that found two of them missed the third.  Before pmap_initialized
	 * there is no collector and no per-CPU area to count a section in, so
	 * the boot's own walks are exempt -- the boundary pmap_read_enter()
	 * uses, for the same reason.
	 */
#if	MACH_ASSERT
	if (va_is_user(va) && pmap_initialized && !urmach_rcu_read_held())
		panic("pmap_walk: 0x%lx is in the lower half and was walked "
		      "outside a read section (#604)", (unsigned long) va);
#endif

	/*
	 * 🔴 A DESTROYED SPACE MAPS NOTHING — AND WITHOUT THIS IT MAPS PHYSICAL
	 * PAGE ZERO (#558).
	 *
	 * pmap_destroy() clears root_pa once the tables are given back, and
	 * table_at(0) is the direct map's own base: a perfectly readable kernel
	 * address holding whatever the firmware left at physical zero.  A walk
	 * from there does not fault.  It reads those bytes as a PML4 and
	 * follows any of them that has the valid bit set, into frames belonging
	 * to somebody else.
	 *
	 * ⚠️ Not a guard that is always true: root_pa is zero exactly between
	 * pmap_destroy() clearing it and the struct being freed, which is the
	 * window a reader can still be inside.  Everywhere else it is a frame.
	 */
	if (root_pa == 0)
		return PT_ENTRY_NULL;

	/* PML4 — always a table; PS is not defined at this level. */
	table = table_at(root_pa);
	entry = &table[pml4_index(va)];
	if (!pte_is_valid(*entry))
		return PT_ENTRY_NULL;

	/* PDPT — may be a 1 GiB leaf. */
	table = table_at(pte_to_pa(*entry));
	entry = &table[pdpt_index(va)];
	if (!pte_is_valid(*entry))
		return PT_ENTRY_NULL;
	if (pte_is_leaf(*entry)) {
		if (page_size_out)
			*page_size_out = PAGE_SIZE_1G;
		return entry;
	}

	/* PD — may be a 2 MiB leaf. */
	table = table_at(pte_to_pa(*entry));
	entry = &table[pd_index(va)];
	if (!pte_is_valid(*entry))
		return PT_ENTRY_NULL;
	if (pte_is_leaf(*entry)) {
		if (page_size_out)
			*page_size_out = PAGE_SIZE_2M;
		return entry;
	}

	/* PT — always a 4 KiB leaf. */
	table = table_at(pte_to_pa(*entry));
	entry = &table[pt_index(va)];
	if (!pte_is_valid(*entry))
		return PT_ENTRY_NULL;

	if (page_size_out)
		*page_size_out = PAGE_SIZE_4K;
	return entry;
}

int pmap_resolve(uint64_t root_pa, uint64_t va, uint64_t *pa_out,
		 uint64_t *page_size_out)
{
	uint64_t page_size;
	pt_entry_t *entry = pmap_walk(root_pa, va, &page_size);

	if (entry == PT_ENTRY_NULL)
		return 0;

	/*
	 * The frame occupies the entry's address bits above the page size,
	 * and the address supplies everything below it.  Masking with the
	 * page size covers all three cases without a per-level special case:
	 * a 2 MiB leaf simply has no meaningful bits 20:12.
	 */
	if (pa_out)
		*pa_out = ((*entry & INTEL_PTE_PFN) & ~(page_size - 1))
			| (va & (page_size - 1));

	if (page_size_out)
		*page_size_out = page_size;

	return 1;
}
