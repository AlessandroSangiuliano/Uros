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
 * A new, empty address space.  Its lower half is bare; its higher half is
 * the kernel's, shared by pointing at the very same next-level tables — that
 * sharing is what lets a trap or a system call keep executing kernel code
 * without a change of address space.
 *
 * The size argument is the MI interface's, and unused: this machine's spaces
 * are all the same size.  Returns PMAP_NULL if nothing is left to build one.
 *
 * The sharing is established by copying the kernel's top-level entries at
 * create time, which carries a standing obligation: a kernel-half entry
 * added to the PML4 *after* a space is created will not appear in it.  Every
 * kernel region therefore has to own its top-level entry before the first
 * user space exists — true today, since the direct map and the image take
 * theirs during bootstrap.
 */
pmap_t pmap_create(uint64_t size);

void pmap_reference(pmap_t pmap);

/* Drop a reference; the space is torn down when the last one goes. */
void pmap_destroy(pmap_t pmap);

/*
 * Make `pmap` the address space this CPU translates through — a CR3 load,
 * which also flushes every non-global TLB entry.  The kernel half is
 * identical across spaces, so the kernel keeps running across the switch.
 */
void pmap_activate(pmap_t pmap);

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

/*
 * Restrict access to one physical page wherever it is mapped, in every
 * address space at once.  VM_PROT_NONE removes the mappings outright.
 *
 * This is how copy-on-write is armed: taking write permission from every
 * mapping of a shared page means the next writer, whoever it is, faults —
 * and only then is a copy made.  It reaches mappings through the physical
 * index, which is the whole reason that index exists.
 */
void pmap_page_protect(uint64_t pa, vm_prot_t prot);

/*
 * The reference and modify bits, which the hardware sets in a page-table
 * entry when a page is read or written.  The pager asks about the page, not
 * about one mapping of it, so these gather across every mapping: referenced
 * or modified anywhere is referenced or modified.
 */
int  pmap_is_referenced(uint64_t pa);
void pmap_clear_reference(uint64_t pa);
int  pmap_is_modified(uint64_t pa);
void pmap_clear_modify(uint64_t pa);
void pmap_set_modify(uint64_t pa);

/*
 * Apply W^X to the kernel image.  boot.S maps the whole image with one large
 * writable-and-executable page; this breaks it to 4 KiB and reprotects each
 * section to its own permission — .text execute-only-of-reads, .rodata
 * read-only, .data/.bss writable and never executable — then sets CR0.WP so
 * the kernel itself honours the read-only pages.  ch.11 §11.5.
 */
void pmap_protect_kernel(void);

/*
 * The other half of the protection posture of ch.11 §11.5: SMEP and SMAP,
 * which stop the kernel executing or touching user pages even where it has
 * been tricked into holding a user pointer.  W^X protects the kernel's own
 * image from being rewritten; these protect it from running or trusting
 * memory the other side controls.
 *
 * Both are optional in the hardware, so each is enabled only where the CPU
 * offers it.  Returns the CR4 bits it actually turned on.
 */
uint64_t pmap_enable_smep_smap(void);

#endif	/* _X86_64_PMAP_PMAP_H_ */
