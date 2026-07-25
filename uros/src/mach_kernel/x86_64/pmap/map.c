/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 page mapping (#407, MD contract 2/6).
 */

#include <stdint.h>

#include <cpu/regs.h>
#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/map.h>
#include <pmap/pte.h>
#include <pmap/walk.h>
#include <trap/trap.h>

static inline pt_entry_t *table_at(uint64_t table_pa)
{
	return (pt_entry_t *)(uintptr_t)phys_to_direct(table_pa);
}

/*
 * The table below `entry`, created if it is not there yet.  Returns NULL and
 * sets *err when the descent cannot continue.
 */
static pt_entry_t *next_table(pt_entry_t *entry, int *err)
{
	uint64_t frame;

	if (pte_is_valid(*entry)) {
		/*
		 * A large page here means the range is already mapped at a
		 * coarser grain, and there is no table below to descend into.
		 */
		if (pte_is_leaf(*entry)) {
			*err = PMAP_MAP_BLOCKED;
			return PT_ENTRY_NULL;
		}
		return table_at(pte_to_pa(*entry));
	}

	frame = boot_frame_alloc();
	if (frame == 0) {
		*err = PMAP_MAP_NO_FRAME;
		return PT_ENTRY_NULL;
	}

	/*
	 * An interior entry carries no NX and no permission policy of its
	 * own.  NX here would veto execution for everything below it
	 * regardless of what the leaves say, and write permission at an
	 * interior level does not grant anything the leaf withholds — so the
	 * leaf stays the only place that decides.
	 */
	*entry = frame | INTEL_PTE_VALID | INTEL_PTE_WRITE;
	return table_at(frame);
}

int pmap_map_page(uint64_t root_pa, uint64_t va, uint64_t pa, uint64_t flags)
{
	pt_entry_t *table = table_at(root_pa);
	pt_entry_t *entry;
	int err = PMAP_MAP_OK;

	entry = &table[pml4_index(va)];
	table = next_table(entry, &err);
	if (table == PT_ENTRY_NULL)
		return err;

	entry = &table[pdpt_index(va)];
	table = next_table(entry, &err);
	if (table == PT_ENTRY_NULL)
		return err;

	entry = &table[pd_index(va)];
	table = next_table(entry, &err);
	if (table == PT_ENTRY_NULL)
		return err;

	entry = &table[pt_index(va)];
	*entry = pa_to_pte(pa) | INTEL_PTE_VALID | flags;

	/*
	 * Local only.  Under SMP this store also has to be ordered against
	 * the other CPUs' views, which is the shootdown — and the barrier that
	 * makes it correct was proved necessary and sufficient under x86-TSO
	 * with herd7 in #350.  x86-64 is still x86-TSO, so that proof carries
	 * across unchanged.  Worth saying out loud: it is the one place the
	 * second architecture costs us nothing, and the next one will not be
	 * so kind.
	 */
	invlpg(va);
	return PMAP_MAP_OK;
}

uint64_t pmap_unmap_page(uint64_t root_pa, uint64_t va)
{
	uint64_t size = 0;
	pt_entry_t *entry = pmap_walk(root_pa, va, &size);

	if (entry == PT_ENTRY_NULL)
		return 0;

	*entry = 0;
	invlpg(va);
	return size;
}

uint64_t pmap_protect_page(uint64_t root_pa, uint64_t va, uint64_t flags)
{
	uint64_t size = 0;
	pt_entry_t *entry = pmap_walk(root_pa, va, &size);

	if (entry == PT_ENTRY_NULL)
		return 0;

	*entry = (*entry & ~INTEL_PTE_PERM) | (flags & INTEL_PTE_PERM);
	invlpg(va);
	return size;
}

uint64_t pmap_split_page(uint64_t root_pa, uint64_t va)
{
	uint64_t size = 0;
	pt_entry_t *entry = pmap_walk(root_pa, va, &size);
	uint64_t base, perm, sub_size, table_pa;
	pt_entry_t *sub;
	int from_1g;

	if (entry == PT_ENTRY_NULL || size == PAGE_SIZE_4K)
		return 0;

	from_1g = (size == PAGE_SIZE_1G);
	sub_size = from_1g ? PAGE_SIZE_2M : PAGE_SIZE_4K;

	/* The frame is aligned to the leaf's own size, so mask by that. */
	base = *entry & (from_1g ? INTEL_PTE_PFN_1G : INTEL_PTE_PFN_2M);
	perm = *entry & INTEL_PTE_PERM;

	/*
	 * Zero is already this function's way of saying "there was nothing to
	 * split", so it cannot also mean "there was, and I could not".  The
	 * caller would read the second as the first and carry on believing the
	 * range is now fine-grained when it is still one large page — which is
	 * how a section ends up sharing its permissions with its neighbour.
	 */
	table_pa = boot_frame_alloc();
	if (table_pa == 0)
		panic("pmap: no frame to split a large page into");

	sub = table_at(table_pa);
	for (unsigned i = 0; i < PTES_PER_TABLE; i++) {
		pt_entry_t leaf = (base + (uint64_t)i * sub_size)
				| INTEL_PTE_VALID | perm;

		/*
		 * The 2 MiB sub-pages of a split gigabyte are still leaves —
		 * at the PD, where PS marks a leaf.  The 4 KiB sub-pages of a
		 * split 2 MiB page are PT entries, where PS is not defined.
		 */
		if (from_1g)
			leaf |= INTEL_PTE_PS;

		sub[i] = leaf;
	}

	/* The interior entry replacing the leaf carries no policy of its own. */
	*entry = table_pa | INTEL_PTE_VALID | INTEL_PTE_WRITE;

	/*
	 * One TLB entry covered the whole large page, and invlpg would only
	 * clear it for one sub-page's worth of addresses — the rest could
	 * keep hitting the stale large translation.  A CR3 reload drops every
	 * non-global entry at once; the direct map is not global, so this
	 * reaches it.  Split is rare and this is bootstrap, so the blunt tool
	 * is the right one.
	 */
	write_cr3(read_cr3());
	return sub_size;
}
