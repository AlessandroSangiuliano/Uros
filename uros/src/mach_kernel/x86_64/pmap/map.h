/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 page mapping (#407, MD contract 2/6).
 *
 * Installing a 4 KiB mapping, building whatever intermediate tables are
 * missing on the way down.  The primitive under pmap_enter(); the MI
 * interface's own entry point arrives with pmap_t.
 */

#ifndef _X86_64_PMAP_MAP_H_
#define _X86_64_PMAP_MAP_H_

#include <stdint.h>

#define PMAP_MAP_OK		0
#define PMAP_MAP_NO_FRAME	1	/* nothing left to build a table in */
#define PMAP_MAP_BLOCKED	2	/* a large page already covers this */

/*
 * Map va to the physical page pa in the tables rooted at root_pa, with
 * `flags` supplying the permission bits (INTEL_PTE_WRITE, INTEL_PTE_NX,
 * INTEL_PTE_USER ...).  The present bit is added here; the caller states
 * policy, not mechanics.
 *
 * Fails rather than improvises when a large page already covers the range:
 * honouring the request would mean splitting that page into a table of
 * smaller ones, which is a real operation with its own shootdown
 * consequences and does not belong hidden inside this one.
 */
int pmap_map_page(uint64_t root_pa, uint64_t va, uint64_t pa, uint64_t flags);

#endif	/* _X86_64_PMAP_MAP_H_ */
