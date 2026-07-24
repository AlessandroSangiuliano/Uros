/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 page-table walk (#407, MD contract 2/6).
 *
 * Following a virtual address down the four levels to the entry that maps
 * it — the primitive underneath pmap_extract() and, later, underneath every
 * operation that has to find an entry before changing it.
 *
 * Both routines read page tables through the direct map, so they require
 * direct_map_init() to have run and the map to cover the tables.  That is
 * the direct map paying for itself: reaching a table at any level is an
 * addition, with no temporary mapping and no self-map trickery.
 */

#ifndef _X86_64_PMAP_WALK_H_
#define _X86_64_PMAP_WALK_H_

#include <stdint.h>

#include <pmap/pte.h>

/*
 * Find the entry that maps va in the tables rooted at the physical address
 * root_pa (what CR3 holds).  Returns a pointer to that entry, or NULL if va
 * is not mapped.
 *
 * The walk stops wherever the mapping actually ends: a PDPT entry with PS
 * set maps 1 GiB and there is no PD below it, a PD entry with PS set maps
 * 2 MiB and there is no PT.  When page_size_out is non-NULL it receives the
 * size the returned entry maps, which is the only way the caller can tell
 * which of the three cases it got.
 */
pt_entry_t *pmap_walk(uint64_t root_pa, uint64_t va, uint64_t *page_size_out);

/*
 * Translate va to the physical address it maps to, offset included.
 * Returns 1 when mapped, 0 when not — a return value rather than a sentinel
 * because physical address zero is a perfectly good answer.
 */
int pmap_resolve(uint64_t root_pa, uint64_t va, uint64_t *pa_out,
		 uint64_t *page_size_out);

#endif	/* _X86_64_PMAP_WALK_H_ */
