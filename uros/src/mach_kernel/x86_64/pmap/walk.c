/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 page-table walk (#407, MD contract 2/6).
 */

#include <stdint.h>

#include <pmap/layout.h>
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
