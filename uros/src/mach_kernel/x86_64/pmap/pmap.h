/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 pmap: the machine-dependent physical map (#407, MD contract 2/6).
 *
 * What machine/pmap.h contributes to the machine-independent vm/pmap.h: the
 * pmap_t type, the kernel's distinguished map, and the translation between
 * Mach's notion of protection and this machine's page-table bits.  The verbs
 * that operate on a pmap — enter, remove, protect, extract — sit on top of
 * this and the primitives in map.h / walk.h.
 */

#ifndef _X86_64_PMAP_PMAP_H_
#define _X86_64_PMAP_PMAP_H_

#include <stdint.h>

#include <pmap/pte.h>

/*
 * One per address space, the kernel's distinguished.  root_pa is exactly
 * what CR3 holds: the physical address of the PML4 that roots this space's
 * four levels.  The kernel half is shared into every space, so every pmap's
 * top-half entries point at the same tables the kernel pmap owns.
 *
 * The MI side maintains a lock and a resident-page count that will join this
 * struct when the machine-independent tree compiles for x86-64; for now it
 * holds only what the primitives beneath it require.
 */
struct pmap {
	uint64_t root_pa;		/* PML4 physical address (the CR3 value) */
	int      ref_count;
};

typedef struct pmap *pmap_t;

#define PMAP_NULL	((pmap_t) 0)

/* The kernel's own map, whose higher half every address space shares. */
pmap_t pmap_kernel(void);

/*
 * Adopt the tables boot.S and the direct map already built as the kernel
 * pmap, taking its root from the live CR3.  After this pmap_kernel() is
 * usable; it does not build anything, it names what is already there.
 */
void pmap_bootstrap(void);

/*
 * Protection: the machine-independent way in, the machine's bits out.  These
 * mirror mach/vm_prot.h and are replaced by that header when the MI tree
 * arrives (guarded so whichever is seen first wins, since the values match).
 *
 * READ is implicit on x86-64 — a present page is readable, there is no
 * read-disable — so it maps to no bit.  WRITE and the *absence* of EXECUTE
 * are the two that move: no-execute is a bit you set, so lack of EXECUTE
 * becomes INTEL_PTE_NX.
 */
#ifndef VM_PROT_NONE
typedef int vm_prot_t;

#define VM_PROT_NONE	0x0
#define VM_PROT_READ	0x1
#define VM_PROT_WRITE	0x2
#define VM_PROT_EXECUTE	0x4
#define VM_PROT_ALL	(VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)
#endif

/*
 * The policy bits of a leaf mapping with protection `prot`.  The present bit
 * belongs to the primitive that writes the leaf, so this yields only policy.
 *
 * VM_PROT_NONE is not handled here: it means remove the mapping, not "map it
 * reachable but with no rights", so the caller turns it into an unmap.
 */
static inline uint64_t pmap_flags_for_prot(vm_prot_t prot)
{
	uint64_t flags = 0;

	if (prot & VM_PROT_WRITE)
		flags |= INTEL_PTE_WRITE;
	if (!(prot & VM_PROT_EXECUTE))
		flags |= INTEL_PTE_NX;
	return flags;
}

/*
 * The pmap verbs, shaped like vm/pmap.h.  Addresses are uint64_t here; the
 * MI vm_offset_t is the same width on x86-64, so the signatures line up when
 * the tree brings the real types.  Ranges are half-open [s, e) and expected
 * page-aligned, as the MI callers pass them.
 */

/* Enter (or, for VM_PROT_NONE, drop) the mapping va -> pa with `prot`. */
int pmap_enter(pmap_t pmap, uint64_t va, uint64_t pa, vm_prot_t prot,
	       int wired);

/* Physical address va maps to, or 0 if unmapped — the MI conflation, where
 * physical zero and "no mapping" share a return, is kept on purpose. */
uint64_t pmap_extract(pmap_t pmap, uint64_t va);

/* Reprotect a range; VM_PROT_NONE removes, as the MI interface specifies. */
void pmap_protect(pmap_t pmap, uint64_t s, uint64_t e, vm_prot_t prot);

/* Remove every mapping in a range. */
void pmap_remove(pmap_t pmap, uint64_t s, uint64_t e);

#endif	/* _X86_64_PMAP_PMAP_H_ */
