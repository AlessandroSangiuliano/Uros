/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 physical-to-virtual index (#407, MD contract 2/6).
 *
 * The page tables answer "what does this virtual address map to".  Several
 * of the machine-independent pmap operations ask the opposite question —
 * given a physical page, reach every mapping of it — and the tables cannot
 * answer that at all without searching every one of them.
 *
 * So each managed physical page keeps a list of the mappings that name it.
 * That is what pmap_page_protect() walks to take write permission away from
 * every copy at once, which is how copy-on-write is armed, and what the
 * reference and modify bits are gathered from, since the hardware records
 * them per page-table entry rather than per page.
 *
 * The structure and its names follow the i386 backend (intel/pmap.c): the
 * head of each list is an entry itself, so a page with a single mapping —
 * which is nearly all of them — needs no allocation at all.
 */

#ifndef _X86_64_PMAP_PV_H_
#define _X86_64_PMAP_PV_H_

#include <stdint.h>

#include <pmap/pmap.h>

typedef struct pv_entry {
	struct pv_entry *next;		/* next mapping of this page */
	pmap_t		 pmap;		/* PMAP_NULL when the head is unused */
	uint64_t	 va;
} *pv_entry_t;

#define PV_ENTRY_NULL	((pv_entry_t) 0)

/*
 * Build the index for physical memory up to top_of_ram: one head per page,
 * in one array indexed by page number.  Pages that fall in a hole get a head
 * nobody ever touches — cheaper than a structure that could describe the
 * holes, and the holes are not where memory is.
 *
 * Requires the frame allocator, which is where the array comes from.
 */
void pv_bootstrap(uint64_t top_of_ram);

/* Whether this physical address is one the index covers. */
int pv_managed(uint64_t pa);

/* The head of the list for a managed page. */
pv_entry_t pv_head(uint64_t pa);

/*
 * Record that `pmap` maps `pa` at `va`, and forget it again.  Both are
 * no-ops for a page the index does not cover, so callers do not have to
 * separate device memory from real memory before asking.
 */
void pv_enter(uint64_t pa, pmap_t pmap, uint64_t va);
void pv_remove(uint64_t pa, pmap_t pmap, uint64_t va);

/* How many mappings a page has — the index's own view, for checking it. */
unsigned pv_count(uint64_t pa);

#endif	/* _X86_64_PMAP_PV_H_ */
