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
 * Internal to pmap_map_page(): a table is missing and the descent is holding
 * a read section it may not allocate in (#455).  Never returned to a caller —
 * the retry loop turns it into a frame and another attempt, or into
 * PMAP_MAP_NO_FRAME when there is no frame to be had.
 */
#define PMAP_MAP_NEED_FRAME	3

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
/*
 * `lock' is the address space's writer lock, or NULL for a caller that has no
 * pmap yet -- the boot paths in x86_64/boot/boot_c.c.  It is taken only in
 * PMAP_ARM_PMAP_LOCK, which exists to be measured against the other arm
 * (#455); in PMAP_ARM_COLLECT_BIT it is not touched at all.
 */
int pmap_map_page(uint64_t root_pa, uint64_t va, uint64_t pa, uint64_t flags,
		  volatile uint8_t *lock);

/*
 * Clear the mapping for va.  Returns the size of the page it removed, or
 * zero if va was not mapped.
 *
 * The intermediate tables are left in place: reclaiming a table once its
 * last entry goes away is collection, a separate operation with its own
 * bookkeeping, not something to fold into every unmap.
 */
uint64_t pmap_unmap_page(uint64_t root_pa, uint64_t va);

/*
 * Change the permission bits (INTEL_PTE_PERM) of the leaf mapping va,
 * keeping its frame and every other bit.  Returns the size of the page it
 * changed, or zero if va was not mapped.  Works on a large leaf as readily
 * as a small one — the protection lives in the entry either way.
 */
uint64_t pmap_protect_page(uint64_t root_pa, uint64_t va, uint64_t flags);

/*
 * Replace the large leaf covering va with a table of next-level entries that
 * map the same physical range with the same permissions — one level finer:
 * a 1 GiB leaf becomes 512 of 2 MiB, a 2 MiB leaf becomes 512 of 4 KiB.
 * Returns the new (smaller) page size, or zero when there is nothing to
 * split: va unmapped, or already at 4 KiB.
 *
 * This is what pmap_map_page() refuses to do implicitly.  It is transparent
 * to anything already using the mapping — the same bytes stay reachable at
 * the same addresses — and is the step that lets a fine-grained operation
 * then touch one sub-page of what used to be one big one.
 */
uint64_t pmap_split_page(uint64_t root_pa, uint64_t va);

#endif	/* _X86_64_PMAP_MAP_H_ */
