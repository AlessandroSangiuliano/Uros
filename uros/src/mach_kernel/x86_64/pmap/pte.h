/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 page-table entry format (#407, MD contract 2/6).
 *
 * The hardware half of the pmap: what a page-table entry *is* on this
 * machine, with no policy attached.  Everything above — the pmap itself,
 * the direct map, W^X — is built on these definitions.
 *
 * Four levels, and every one of them is a 4 KiB table of 512 eight-byte
 * entries, indexed by nine bits of the virtual address:
 *
 *   63    48 47      39 38      30 29      21 20      12 11         0
 *  +--------+----------+----------+----------+----------+------------+
 *  |  sign  |   PML4   |   PDPT   |    PD    |    PT    |   offset   |
 *  +--------+----------+----------+----------+----------+------------+
 *
 * The top bits are not free: an address is canonical only if bits 63:47 are
 * all copies of bit 47, which is what splits the space into the two halves
 * of docs/uros_design.md ch.11.
 *
 * A table entry can also stop early and map a large page directly: at the
 * PD it covers 2 MiB, at the PDPT 1 GiB.  That is what makes the direct map
 * cheap (ch.11 §11.4) — a handful of TLB entries instead of one per 4 KiB.
 *
 * Note the width: entries are eight bytes at every level.  The i386 backend
 * is full of four-byte assumptions that are correct there and bugs here.
 */

#ifndef _X86_64_PTE_H_
#define _X86_64_PTE_H_

#include <stdint.h>

/*
 * One entry, one table.  A table is exactly one 4 KiB page, so 512 entries
 * of eight bytes each, and nine bits of virtual address select one.
 */
typedef uint64_t	pt_entry_t;

#define PT_ENTRY_NULL	((pt_entry_t *) 0)

#define PTES_PER_TABLE	512
#define PTE_INDEX_MASK	(PTES_PER_TABLE - 1)

/*
 * Shift of the index field for each level, which is also the shift of the
 * page size that level can map as a leaf.
 */
#define PT_SHIFT	12			/* 4 KiB   */
#define PD_SHIFT	21			/* 2 MiB   */
#define PDPT_SHIFT	30			/* 1 GiB   */
#define PML4_SHIFT	39			/* 512 GiB */

#define PAGE_SIZE_4K	(1ULL << PT_SHIFT)
#define PAGE_SIZE_2M	(1ULL << PD_SHIFT)
#define PAGE_SIZE_1G	(1ULL << PDPT_SHIFT)

/* Pull one level's index out of a virtual address. */
#define pml4_index(va)	(((uint64_t)(va) >> PML4_SHIFT) & PTE_INDEX_MASK)
#define pdpt_index(va)	(((uint64_t)(va) >> PDPT_SHIFT) & PTE_INDEX_MASK)
#define pd_index(va)	(((uint64_t)(va) >> PD_SHIFT)   & PTE_INDEX_MASK)
#define pt_index(va)	(((uint64_t)(va) >> PT_SHIFT)   & PTE_INDEX_MASK)

/*
 * Entry bits.  Same names the i386 backend uses for the ones that survive,
 * because they are the same architectural bits — only wider, plus the two
 * this mode adds (a 1 GiB leaf, and execute-disable up at bit 63).
 */
#define INTEL_PTE_VALID		0x0000000000000001ULL	/* P   present         */
#define INTEL_PTE_WRITE		0x0000000000000002ULL	/* R/W writable        */
#define INTEL_PTE_USER		0x0000000000000004ULL	/* U/S user-accessible */
#define INTEL_PTE_WTHRU		0x0000000000000008ULL	/* PWT write-through   */
#define INTEL_PTE_NCACHE	0x0000000000000010ULL	/* PCD cache-disable   */
#define INTEL_PTE_REF		0x0000000000000020ULL	/* A   accessed        */
#define INTEL_PTE_MOD		0x0000000000000040ULL	/* D   dirty (leaf)    */
#define INTEL_PTE_PS		0x0000000000000080ULL	/* leaf here, not a table */
#define INTEL_PTE_GLOBAL	0x0000000000000100ULL	/* G   survives a CR3 load */

/*
 * Execute-disable.  Bit 63 is *reserved* until EFER.NXE is set — setting it
 * before then faults rather than protecting anything, so the pmap must turn
 * NXE on before it starts writing this bit.
 */
#define INTEL_PTE_NX		0x8000000000000000ULL

/*
 * The bits a protection change is allowed to touch: who may write, who may
 * reach it, whether it may execute.  Everything else in a leaf — the frame,
 * PS, accessed/dirty, caching, global — is preserved across a reprotect.
 */
#define INTEL_PTE_PERM		(INTEL_PTE_WRITE | INTEL_PTE_USER | INTEL_PTE_NX)

/*
 * Physical frame masks.  Bits 51:12 hold the frame for a 4 KiB entry; a
 * large leaf must have the bits below its own page size clear, so it gets a
 * narrower mask.
 */
#define INTEL_PTE_PFN		0x000ffffffffff000ULL	/* 4 KiB leaf or table */
#define INTEL_PTE_PFN_2M	0x000fffffffe00000ULL	/* 2 MiB leaf          */
#define INTEL_PTE_PFN_1G	0x000fffffc0000000ULL	/* 1 GiB leaf          */

#define pte_to_pa(pte)		((uint64_t)(pte) & INTEL_PTE_PFN)
#define pa_to_pte(pa)		((pt_entry_t)(pa) & INTEL_PTE_PFN)

#define pte_is_valid(pte)	(((pte) & INTEL_PTE_VALID) != 0)

/*
 * True when a PD or PDPT entry maps a large page instead of pointing at the
 * next table.  Meaningless at the PML4 and the PT, where PS is not defined.
 */
#define pte_is_leaf(pte)	(((pte) & INTEL_PTE_PS) != 0)

#endif	/* _X86_64_PTE_H_ */
