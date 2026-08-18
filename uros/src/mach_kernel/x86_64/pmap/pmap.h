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

	/*
	 * Pages mapped in this space.  The machine-independent tree reads it
	 * through pmap_resident_count() and vm_debug.c sizes an array from
	 * it, so it is a count and not an estimate (#453).
	 */
	int      resident_count;
};

typedef struct pmap *pmap_t;

#define PMAP_NULL	((pmap_t) 0)

/*
 * The kernel's own map, whose higher half every address space shares.
 *
 * A macro over the object rather than a function returning it, because the
 * machine-independent tree asks for it on paths that run per page fault and
 * per kernel allocation.  As a macro it is the address of a static object --
 * an immediate under -mcmodel=kernel, folded at compile time -- where a
 * function would be a call across a translation unit that no optimiser can
 * see through without link-time optimisation we do not build with.
 *
 * <vm/pmap.h> declares `extern pmap_t (pmap_kernel)(void)`, parenthesised so
 * this definition wins at every call site; the declaration is never referred
 * to and so never has to exist.  i386 does exactly the same thing, and the
 * machine-independent code is written against the name, not against which of
 * the two it turns out to be (#453).
 */
extern struct pmap kernel_pmap_store;

#define	pmap_kernel()	(&kernel_pmap_store)

/*
 * A new, empty address space.  Its lower half is bare; its higher half is
 * the kernel's, shared by pointing at the very same next-level tables — that
 * sharing is what lets a trap or a system call keep executing kernel code
 * without a change of address space.
 *
 * A non-zero size requests a map over part of a space, which this backend
 * does not build: it returns PMAP_NULL, which is the interface's way of
 * saying the caller should treat the map as software-only.  PMAP_NULL also
 * comes back when there is nothing left to build a space out of.
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
 * #456: keep one pool-allocated space alive past pmap_init(), so that the
 * boundary between the two allocators has a subject to be tested on.  Called
 * from the machine-dependent self-tests; a caller after the boot would get a
 * zone element and prove nothing.
 */
void pmap_boundary_probe_arm(void);

/* #455: interior page-table frames alive right now.  See pmap.c. */
extern unsigned pmap_table_frames_live;

/* #455: -C, what concurrency does to a pmap that has no locking. */
void pmap_collect_bench(void);

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
 * Protection: the machine-independent way in, the machine's bits out.
 *
 * READ is implicit on x86-64 — a present page is readable, there is no
 * read-disable — so it maps to no bit.  WRITE and the *absence* of EXECUTE
 * are the two that move: no-execute is a bit you set, so lack of EXECUTE
 * becomes INTEL_PTE_NX.
 *
 * ⚠️ These used to be defined here behind `#ifndef VM_PROT_NONE', with a
 * note saying they would be "replaced by that header when the MI tree
 * arrives".  It has (#453), and the guard did not do what it promised:
 * <mach/vm_prot.h> defines them unconditionally, so when this header was
 * seen first the two definitions collided and the tree compiled with five
 * redefinition warnings and no rule about which set won.
 *
 * Now there is one definition and it is the interface's.
 */
#include <mach/vm_prot.h>

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

/*
 * Bracket a kernel access to a page ring 3 can reach.
 *
 * SMAP makes such an access fault, which is the whole point: the kernel
 * touching user memory is nearly always a bug — a stray pointer, an
 * argument taken on trust — and the few places where it is deliberate
 * should have to say so.  These are how they say so.
 *
 * Deliberate means copyin and copyout and nothing else, once those exist.
 * The window between them is the only time this kernel can be made to read
 * a user address by accident, so it should be as short as the access.
 *
 * They do nothing when SMAP is not enabled, and that check is not defensive
 * tidiness: the instructions themselves are invalid opcodes unless CR4.SMAP
 * is set, so a kernel on a part without it would fault on the guard rather
 * than on the thing being guarded.  A branch for now; the day it is on a
 * hot path it becomes patched code, which is what the flag exists to make
 * possible later without changing any caller.
 */
void pmap_user_access_begin(void);
void pmap_user_access_end(void);

/* Whether SMAP is actually on, which decides whether the pair does anything. */
int pmap_smap_enabled(void);

/*
 * Whether `pmap` may hold `va` at all.
 *
 * The kernel's map has no user half: §11.1 gives the lower half to the
 * address space, so a lower-half mapping in the kernel's own map is a
 * mistake — and one that would otherwise arrive as a page ring 3 can read,
 * long after whatever asked for it.
 *
 * A predicate rather than a check buried in pmap_enter(), because
 * pmap_enter()'s answer to a violation is to stop, and a rule whose only
 * expression is a panic can never be shown to work without ending the boot.
 * This is the same rule the panic uses, askable.
 */
int pmap_may_map(pmap_t pmap, uint64_t va);

/*
 * Enter (or, for VM_PROT_NONE, drop) the mapping va -> pa with `prot`.
 *
 * `wired` says the pager may not reclaim the page.  The hardware has no bit
 * for it, so it is recorded in one of the entry's software bits — in the
 * entry rather than beside it, where it cannot drift out of step with the
 * mapping it describes.
 */
int pmap_enter(pmap_t pmap, uint64_t va, uint64_t pa, vm_prot_t prot,
	       int wired);

/*
 * A zeroed physical frame for a page table, from whichever allocator owns
 * physical memory right now.  See the comment on the definition (#458).
 */
uint64_t pmap_table_frame(void);

extern int pmap_initialized;

/* Change whether an existing mapping is wired.  Returns 0 if va is unmapped. */
int pmap_change_wiring(pmap_t pmap, uint64_t va, int wired);

/* Whether the mapping at va is wired.  Zero if it is not, or is not there. */
int pmap_is_wired(pmap_t pmap, uint64_t va);

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

/*
 * Make `size` bytes of device registers at physical `pa` reachable, and
 * answer with the address to reach them at.
 *
 * Uncached, because for registers caching is not a performance choice but a
 * wrong answer: a read served from a cache line is a read of the past, and
 * a write sitting in one is an order the device never received.  Also
 * non-executable and kernel-only, neither of which any device needs.
 *
 * The address is taken from the device region of ch.11 and never given
 * back — a mapped register block belongs to a driver for as long as the
 * driver exists, and unmapping is a problem to solve when one can be
 * unloaded.  A `pa` that is not page-aligned keeps its offset in the answer.
 */
uint64_t pmap_map_device(uint64_t pa, uint64_t size);

/*
 * Zeroing and copying physical pages (x86_64/pmap/phys.c).  Addresses are
 * physical and reached through the direct map, so unlike the i386 twin
 * these borrow no temporary mapping.  The lpage and rpage forms are the
 * asymmetric ones: in each, one side is a physical page and the other an
 * address the caller already holds.
 *
 * Offsets and lengths must stay within the page they name — the caller's
 * obligation, as in the machine-independent interface.
 */
void pmap_zero_page(uint64_t p);
void pmap_zero_part_page(uint64_t p, uint64_t offset, uint64_t len);
void pmap_copy_page(uint64_t src, uint64_t dst);
void pmap_copy_part_page(uint64_t src, uint64_t src_offset,
			 uint64_t dst, uint64_t dst_offset, uint64_t len);
void pmap_copy_part_lpage(uint64_t src_va, uint64_t dst,
			  uint64_t dst_offset, uint64_t len);
void pmap_copy_part_rpage(uint64_t src, uint64_t src_offset,
			  uint64_t dst_va, uint64_t len);

/*
 * ── The contract the machine-independent VM calls through (#453) ──────
 *
 * <vm/pmap.h> supplies the _USER and _KERNEL forms itself, in terms of the
 * two below, whenever the machine does not define them.  We let it: those
 * wrappers are policy about when to switch, and policy is not ours.  What is
 * ours is what switching means, and here it means one write of CR3.
 */
#define	PMAP_ACTIVATE(pmap, act, cpu)	pmap_activate(pmap)

/*
 * Deactivation records nothing, and that is a statement rather than a gap.
 *
 * On i386 the pair maintains a per-pmap bitmap of the processors using it,
 * for the sole purpose of narrowing TLB shootdown to those processors.  This
 * target does not have that narrowing yet -- its shootdown goes to every
 * processor (#439) -- so a bitmap kept here would have no reader, and a
 * bitmap with no reader is worse than none: it costs a store on every switch
 * and rots quietly until someone trusts it.
 *
 * When #439 lands it lands as a set this macro maintains, and this comment
 * is the thing that has to change with it.
 */
#define	PMAP_DEACTIVATE(pmap, act, cpu)

/*
 * Move a thread onto another address space.  The order matters and is not
 * arbitrary: the map has to be recorded before CR3 changes, so that a trap
 * taken on the very next instruction finds the activation describing the
 * space the processor is actually in.
 */
#define	PMAP_SWITCH_USER(act, new_map, cpu)				\
MACRO_BEGIN								\
	(act)->map = (new_map);						\
	pmap_activate(vm_map_pmap(new_map));				\
MACRO_END

/*
 * Is this a kernel virtual address?  The upper bound is the top of the
 * address space, so the test is really the lower one -- but it is written as
 * a range because that is what the name promises, and because a canonical
 * hole sits below VM_MIN_KERNEL_ADDRESS rather than above it.
 */
#define	pmap_kernel_va(VA)						\
	(((vm_offset_t)(VA) >= VM_MIN_KERNEL_ADDRESS) &&		\
	 ((vm_offset_t)(VA) <= VM_MAX_KERNEL_ADDRESS))

/*
 * The physical address behind a kernel virtual one.
 *
 * ⚠️ Not direct_to_phys().  That is a subtraction and would be wrong for
 * every address outside the direct map -- which includes the kernel image
 * itself, mapped in the top 2 GiB, and everything kmem_alloc() hands out of
 * kernel_map.  The callers here pass exactly those: test_device.c asks about
 * memory it allocated.  So it is a walk, like i386's, and the fast
 * subtraction stays where it is correct, behind its own name.
 */
/*
 * ── Four the machine-independent tree calls but never defines ─────────
 *
 * Each is a macro on i386 and so has no symbol to link against; the MI code
 * calls them by name and the machine decides whether that name is work or
 * nothing.  Written out here rather than inherited, because two of the four
 * are "nothing" and it is worth saying which two and why.
 */

/*
 * Pages this map has resident.  A real count, kept by the pmap, because
 * vm_debug.c sizes an array from it -- an answer that is too small is a
 * buffer overrun in the caller, so this is not a place for an estimate.
 *
 * ⚠️ It lives in the pmap and is therefore a shared line on a machine with
 * 64 processors.  #349 is the follow-up that makes it per-CPU; until then it
 * is one counter and its cost is a decision that has been noticed rather
 * than one that has not.
 */
#define	pmap_resident_count(pmap)	((pmap)->resident_count)

/* The address a page frame number names. */
#define	pmap_phys_address(frame)	((vm_offset_t) x86_64_ptob(frame))
#define	pmap_phys_to_frame(phys)	((int) x86_64_btop(phys))

/*
 * Copying mappings from one map to another at fork: nothing, deliberately.
 *
 * It is an optimisation, not a requirement -- the interface allows a machine
 * to pre-populate the child's tables so its first touches do not fault, and
 * allows it to decline, in which case the faults do the work.  Declining is
 * right here for now because the cost of the copy is real and the saving is
 * a guess; #436 is where batched population gets measured, and if it wins
 * this is where it lands.
 */
#define	pmap_copy(dst, src, dst_addr, len, src_addr)			\
	((void)(dst), (void)(src), (void)(dst_addr), (void)(len),	\
	 (void)(src_addr))

/*
 * Machine-specific memory attributes: none on this machine.
 *
 * KERN_INVALID_ARGUMENT and not KERN_SUCCESS: the caller asked for an
 * attribute to be applied and it was not, so answering success would be the
 * plausible return this project refuses.  i386 answers the same way.
 */
#define	pmap_attribute(pmap, addr, size, attr, value)			\
	((void)(pmap), (void)(addr), (void)(size), (void)(attr),	\
	 (void)(value), KERN_INVALID_ARGUMENT)

#define	kvtophys(VA)	((vm_offset_t) pmap_extract(pmap_kernel(), (uint64_t)(VA)))

#endif	/* _X86_64_PMAP_PMAP_H_ */
