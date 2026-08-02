/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 pmap: the machine-dependent physical map (#407, MD contract 2/6).
 */

#include <stdint.h>

#include <cpu/regs.h>
#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/map.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>
#include <pmap/pv.h>
#include <pmap/tlb.h>
#include <pmap/walk.h>
#include <trap/trap.h>

/*
 * The kernel pmap is a single object, not allocated: it has to exist before
 * any allocator does, and it lives as long as the kernel.  Its tables are
 * the ones boot.S and the direct map already built.
 */
struct pmap kernel_pmap_store;

void pmap_bootstrap(void)
{
	kernel_pmap_store.root_pa = read_cr3() & INTEL_PTE_PFN;
	kernel_pmap_store.ref_count = 1;
}

/*
 * Somewhere to keep the pmaps themselves until the MI allocator exists.  A
 * slot with no references is free, so a destroyed space's slot comes back —
 * unlike its frames, which do not.
 */
#define PMAP_BOOT_MAX	16

static struct pmap pmap_pool[PMAP_BOOT_MAX];

/*
 * The first PML4 slot of the kernel half.  Entries from here up are the
 * kernel's and are shared into every space; everything below is the space's
 * own.  Deriving it from the layout keeps the split in one place: five-level
 * paging moves the boundary along with KERNEL_HALF_BASE.
 */
#define KERNEL_PML4_FIRST	pml4_index(KERNEL_HALF_BASE)

_Static_assert(KERNEL_PML4_FIRST == 256, "the halves are not evenly split");

pmap_t pmap_create(uint64_t size)
{
	const pt_entry_t *kroot;
	pt_entry_t *root;
	uint64_t root_pa;
	pmap_t pmap = PMAP_NULL;

	/*
	 * A non-zero size asks for a map covering part of a space rather than
	 * a space, which this backend does not build — the same answer the
	 * i386 one gives, and for the same reason: refusing lets the caller
	 * treat the map as software-only, while handing back a full address
	 * space would have it believe it got what it asked for and pay a root
	 * frame for something it will never translate through.
	 */
	if (size != 0)
		return PMAP_NULL;

	for (unsigned i = 0; i < PMAP_BOOT_MAX; i++)
		if (pmap_pool[i].ref_count == 0) {
			pmap = &pmap_pool[i];
			break;
		}

	if (pmap == PMAP_NULL)
		return PMAP_NULL;

	root_pa = boot_frame_alloc();		/* arrives zeroed */
	if (root_pa == 0)
		return PMAP_NULL;

	/*
	 * Share the kernel half by pointing at the same next-level tables, not
	 * by copying them: a mapping the kernel changes later is then seen by
	 * every space at once, because they are all reading the one table.
	 * Only these top-level entries are duplicated.
	 */
	root = (pt_entry_t *)(uintptr_t)phys_to_direct(root_pa);
	kroot = (const pt_entry_t *)(uintptr_t)
		phys_to_direct(kernel_pmap_store.root_pa);

	for (unsigned i = KERNEL_PML4_FIRST; i < PTES_PER_TABLE; i++)
		root[i] = kroot[i];

	pmap->root_pa = root_pa;
	pmap->ref_count = 1;
	return pmap;
}

void pmap_reference(pmap_t pmap)
{
	if (pmap != PMAP_NULL)
		pmap->ref_count++;
}

/* The index shift of each paging level, deepest first. */
static const unsigned level_shift[] = {
	PT_SHIFT, PD_SHIFT, PDPT_SHIFT, PML4_SHIFT
};

/*
 * Give back every table below `table_pa`, and the table itself.
 *
 * What is freed is the space's own scaffolding, never what it was pointing
 * at: a leaf names a page belonging to some VM object, which outlives the
 * mapping and is not this layer's to reclaim.  Leaves are visited only to
 * strike them from the physical index — an entry naming a pmap that no
 * longer exists would be followed by the next pmap_page_protect on that
 * page, into a root that has since been handed to someone else.
 *
 * `level` counts up from 1 at the page table, so it indexes level_shift
 * directly and the recursion is at most four deep.
 */
static void pmap_free_tables(uint64_t table_pa, unsigned level,
			     uint64_t va_base, pmap_t pmap)
{
	const pt_entry_t *table =
		(const pt_entry_t *)(uintptr_t)phys_to_direct(table_pa);

	for (unsigned i = 0; i < PTES_PER_TABLE; i++) {
		pt_entry_t e = table[i];
		uint64_t va;

		if (!pte_is_valid(e))
			continue;

		va = va_base + ((uint64_t)i << level_shift[level - 1]);

		if (level == 1) {
			pv_remove(pte_to_pa(e), pmap, va);
			continue;
		}

		/* A large page is a leaf too: no table under it to free. */
		if (pte_is_leaf(e))
			continue;

		pmap_free_tables(pte_to_pa(e), level - 1, va, pmap);
	}

	boot_frame_free(table_pa);
}

void pmap_destroy(pmap_t pmap)
{
	const pt_entry_t *root;

	if (pmap == PMAP_NULL || pmap == pmap_kernel())
		return;

	if (--pmap->ref_count > 0)
		return;

	root = (const pt_entry_t *)(uintptr_t)phys_to_direct(pmap->root_pa);

	/*
	 * The lower half only.  Everything from KERNEL_PML4_FIRST up points at
	 * tables the kernel owns and every other space is still using — freeing
	 * those would hand the kernel's own page tables to the next caller who
	 * asked for a frame, and the damage would surface anywhere but here.
	 */
	for (unsigned i = 0; i < KERNEL_PML4_FIRST; i++)
		if (pte_is_valid(root[i]))
			pmap_free_tables(pte_to_pa(root[i]), 3,
					 (uint64_t)i << PML4_SHIFT, pmap);

	boot_frame_free(pmap->root_pa);
	pmap->root_pa = 0;
}

void pmap_activate(pmap_t pmap)
{
	if (pmap != PMAP_NULL)
		write_cr3(pmap->root_pa);
}

/*
 * Drop the mapping at va and forget it in the physical index too.  Returns
 * the size removed, or zero if there was nothing there.
 *
 * The index is only told about pages it tracks: a large mapping is the
 * kernel's own — the direct map, the image — and belongs to no VM object, so
 * nothing will ever ask which pmaps hold it.
 */
static uint64_t pmap_forget(pmap_t pmap, uint64_t va)
{
	uint64_t pa = 0;
	uint64_t size;

	pmap_resolve(pmap->root_pa, va, &pa, 0);
	size = pmap_unmap_page(pmap->root_pa, va);

	if (size == PAGE_SIZE_4K)
		pv_remove(pa, pmap, va);

	if (size != 0) {
		/*
		 * panic() and not assert(), because Assert() lives in
		 * kern/debug.c and the machine-independent tree is not in this
		 * kernel yet -- and because an underflow here means the count
		 * has drifted from the tables, which is not a condition to
		 * carry on from.
		 */
		if (pmap->resident_count == 0)
			panic("pmap_forget: resident_count underflow at "
			      "va 0x%lx", (unsigned long) va);
		pmap->resident_count--;
	}

	return size;
}

int pmap_may_map(pmap_t pmap, uint64_t va)
{
	return !(va_is_user(va) && pmap == pmap_kernel());
}

int pmap_enter(pmap_t pmap, uint64_t va, uint64_t pa, vm_prot_t prot,
	       int wired)
{
	uint64_t flags;
	int rc;

	if (prot == VM_PROT_NONE) {
		pmap_forget(pmap, va);
		return PMAP_MAP_OK;
	}

	/*
	 * Whatever was here before is no longer mapped at this address, so the
	 * index must lose that entry before it gains the new one — otherwise
	 * replacing a mapping would leave the old page believing it still had
	 * one, and a later page_protect would walk to an address that no
	 * longer names it.
	 */
	pmap_forget(pmap, va);

	flags = pmap_flags_for_prot(prot);
	if (wired)
		flags |= INTEL_PTE_WIRED;

	/*
	 * Whether ring 3 may reach it follows from the address, not from an
	 * argument.  §11.1 gives the lower half to the address space and the
	 * upper half to the kernel, so the question is already answered by
	 * where the caller asked for the mapping — and a caller that had to
	 * say so as well could say something different, which is a way for a
	 * kernel page to become reachable that no reviewer would spot.
	 *
	 * The 32-bit pmap asks a different question — whether this is the
	 * kernel's map — because its layout has no halves to ask about: the
	 * kernel sits at the top of every address space and the two are told
	 * apart by which map they arrived through.  On this architecture the
	 * address is the stronger answer of the two: it cannot mark a
	 * kernel-half page reachable even if the mapping arrives through a
	 * user's map, which the older test would happily do.
	 *
	 * Both are checked, because they are not the same question and each
	 * catches what the other lets past.  The address decides whether the
	 * bit is set; the map decides whether the mapping is legitimate at
	 * all.  The kernel's own map has no user half — anything appearing
	 * there is a mistake, and one that would otherwise arrive as a page
	 * ring 3 can read.
	 */
	if (!pmap_may_map(pmap, va))
		panic("pmap: the kernel map has no user half to map into");

	if (va_is_user(va))
		flags |= INTEL_PTE_USER;

	rc = pmap_map_page(pmap->root_pa, va, pa, flags);
	if (rc == PMAP_MAP_OK) {
		pv_enter(pa, pmap, va);
		/*
		 * The count the machine-independent tree reads through
		 * pmap_resident_count() (#453).  Kept here, where a mapping is
		 * actually created, rather than by the caller -- a counter
		 * maintained anywhere else drifts the first time somebody adds
		 * a second path in.
		 *
		 * Only on a fresh mapping: pmap_map_page() answers PMAP_MAP_OK
		 * for one that replaces nothing, so re-mapping a page that is
		 * already there does not count twice.
		 */
		pmap->resident_count++;
	}

	return rc;
}

int pmap_change_wiring(pmap_t pmap, uint64_t va, int wired)
{
	pt_entry_t *entry = pmap_walk(pmap->root_pa, va, 0);

	if (entry == PT_ENTRY_NULL)
		return 0;

	/*
	 * No invalidation: the hardware never looks at this bit, so nothing it
	 * has cached can be out of date because of it.
	 */
	if (wired)
		*entry |= INTEL_PTE_WIRED;
	else
		*entry &= ~INTEL_PTE_WIRED;

	return 1;
}

int pmap_is_wired(pmap_t pmap, uint64_t va)
{
	pt_entry_t *entry = pmap_walk(pmap->root_pa, va, 0);

	return entry != PT_ENTRY_NULL && (*entry & INTEL_PTE_WIRED) != 0;
}

uint64_t pmap_extract(pmap_t pmap, uint64_t va)
{
	uint64_t pa = 0;

	return pmap_resolve(pmap->root_pa, va, &pa, 0) ? pa : 0;
}

/*
 * Step a range one mapping at a time.  Advancing by the size the operation
 * reports skips a large page in one stride instead of touching its every
 * 4 KiB; where nothing is mapped, 4 KiB is the step that cannot overshoot a
 * mapping that starts partway through.
 */
void pmap_remove(pmap_t pmap, uint64_t s, uint64_t e)
{
	while (s < e) {
		uint64_t sz = pmap_forget(pmap, s);

		s += sz ? sz : PAGE_SIZE_4K;
	}
}

void pmap_protect(pmap_t pmap, uint64_t s, uint64_t e, vm_prot_t prot)
{
	uint64_t flags = pmap_flags_for_prot(prot);

	while (s < e) {
		uint64_t sz;

		if (prot == VM_PROT_NONE)
			sz = pmap_forget(pmap, s);
		else
			sz = pmap_protect_page(pmap->root_pa, s, flags);

		s += sz ? sz : PAGE_SIZE_4K;
	}
}

/*
 * Next free address in the device region.  A bump, because nothing is ever
 * returned: see the header.
 */
static uint64_t device_next = DEVICE_MAP_BASE;

uint64_t pmap_map_device(uint64_t pa, uint64_t size)
{
	uint64_t offset = pa & (PAGE_SIZE_4K - 1);
	uint64_t first = pa - offset;
	uint64_t last = (pa + size + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
	uint64_t va = device_next;
	uint64_t flags = INTEL_PTE_WRITE | INTEL_PTE_NX
		       | INTEL_PTE_NCACHE | INTEL_PTE_WTHRU;

	if (size == 0)
		return 0;

	if (va + (last - first) - DEVICE_MAP_BASE > DEVICE_MAP_MAX_SIZE)
		panic("pmap: the device region is full");

	/*
	 * PCD and PWT together are uncacheable in the absence of a page
	 * attribute table.  When PAT is set up these two bits become an index
	 * into it instead, and this is the line that has to change with it —
	 * quietly keeping the old encoding would leave device memory cached
	 * while looking untouched.
	 */
	for (uint64_t p = first; p < last; p += PAGE_SIZE_4K) {
		if (pmap_map_page(kernel_pmap_store.root_pa,
				  device_next, p, flags) != PMAP_MAP_OK)
			panic("pmap: could not map device registers");
		device_next += PAGE_SIZE_4K;
	}

	return va + offset;
}

/* ------------------------------------------------------------------ */
/*  Operations that start from a physical page                          */
/* ------------------------------------------------------------------ */

void pmap_page_protect(uint64_t pa, vm_prot_t prot)
{
	uint64_t flags;
	pv_entry_t pv;

	if (prot == VM_PROT_NONE) {
		/*
		 * Each removal rewrites the list — the head especially, whose
		 * successor is copied up into it — so take the head afresh
		 * every time rather than holding a pointer across the change.
		 */
		while ((pv = pv_head(pa)) != PV_ENTRY_NULL
		       && pv->pmap != PMAP_NULL)
			pmap_forget(pv->pmap, pv->va);
		return;
	}

	flags = pmap_flags_for_prot(prot);

	for (pv = pv_head(pa); pv != PV_ENTRY_NULL && pv->pmap != PMAP_NULL;
	     pv = pv->next)
		pmap_protect_page(pv->pmap->root_pa, pv->va, flags);
}

/*
 * True if any mapping of the page carries the bits.  The hardware sets them
 * in whichever entry it walked, so a page read through one mapping is
 * referenced even though the others say nothing.
 */
static int pv_test_bits(uint64_t pa, uint64_t bits)
{
	pv_entry_t pv;

	for (pv = pv_head(pa); pv != PV_ENTRY_NULL && pv->pmap != PMAP_NULL;
	     pv = pv->next) {
		pt_entry_t *e = pmap_walk(pv->pmap->root_pa, pv->va, 0);

		if (e != PT_ENTRY_NULL && (*e & bits))
			return 1;
	}

	return 0;
}

static void pv_change_bits(uint64_t pa, uint64_t bits, int set)
{
	pv_entry_t pv;

	for (pv = pv_head(pa); pv != PV_ENTRY_NULL && pv->pmap != PMAP_NULL;
	     pv = pv->next) {
		pt_entry_t *e = pmap_walk(pv->pmap->root_pa, pv->va, 0);

		if (e == PT_ENTRY_NULL)
			continue;

		if (set)
			*e |= bits;
		else
			*e &= ~bits;

		/*
		 * Dropping the TLB entry is not housekeeping here, it is the
		 * point: the hardware only writes these bits during a page
		 * walk, and a cached translation means no walk.  Clearing
		 * accessed without a flush would leave it clear however often
		 * the page is touched, and the pager would read that as a page
		 * nobody wants.
		 *
		 * On every processor, for the same reason: it is the processor
		 * holding the cached translation that will fail to record the
		 * next touch, and that is exactly the one this did not run on.
		 */
		tlb_flush_page(pv->va);
	}
}

int pmap_is_referenced(uint64_t pa)
{
	return pv_test_bits(pa, INTEL_PTE_REF);
}

void pmap_clear_reference(uint64_t pa)
{
	pv_change_bits(pa, INTEL_PTE_REF, 0);
}

int pmap_is_modified(uint64_t pa)
{
	return pv_test_bits(pa, INTEL_PTE_MOD);
}

void pmap_clear_modify(uint64_t pa)
{
	pv_change_bits(pa, INTEL_PTE_MOD, 0);
}

void pmap_set_modify(uint64_t pa)
{
	pv_change_bits(pa, INTEL_PTE_MOD, 1);
}

/*
 * Section bounds from the linker (boot.ld), page-aligned so each belongs to
 * exactly one protection.
 */
extern char __ktext_start[], __ktext_end[];
extern char __krodata_start[], __krodata_end[];
extern char __kdata_start[], __kernel_end[];

void pmap_protect_kernel(void)
{
	pmap_t k = pmap_kernel();
	uint64_t t0 = (uint64_t)(uintptr_t)__ktext_start;
	uint64_t end = (uint64_t)(uintptr_t)__kernel_end;
	uint64_t va;

	/*
	 * The image rode in on one large writable-and-executable page (a few,
	 * if it ever outgrows 2 MiB).  Break every large page across it to
	 * 4 KiB first, so section boundaries that fall mid-large-page can each
	 * get their own permission.  Splitting an already-small page is a
	 * no-op, so this is safe whatever the page size turns out to be.
	 */
	for (va = t0 & ~(PAGE_SIZE_2M - 1); va < end; va += PAGE_SIZE_2M)
		pmap_split_page(k->root_pa, va);

	pmap_protect(k, t0, (uint64_t)(uintptr_t)__ktext_end,
		     VM_PROT_READ | VM_PROT_EXECUTE);
	pmap_protect(k, (uint64_t)(uintptr_t)__krodata_start,
		     (uint64_t)(uintptr_t)__krodata_end, VM_PROT_READ);
	pmap_protect(k, (uint64_t)(uintptr_t)__kdata_start, end,
		     VM_PROT_READ | VM_PROT_WRITE);

	/*
	 * Until now the kernel could write a page marked read-only: CR0.WP
	 * makes the read-only bit bind at ring 0 too, which is what turns the
	 * .text and .rodata protections from advisory into real.
	 */
	write_cr0(read_cr0() | CR0_WP);
}

/*
 * Whether the bit was actually set, which is not the same as whether the
 * processor has it: a part without SMAP leaves this clear and the access
 * brackets below become nothing, because their instructions would be
 * invalid opcodes.
 */
static int smap_on;

int pmap_smap_enabled(void)
{
	return smap_on;
}

void pmap_user_access_begin(void)
{
	if (smap_on)
		__asm__ volatile("stac" ::: "cc", "memory");
}

void pmap_user_access_end(void)
{
	if (smap_on)
		__asm__ volatile("clac" ::: "cc", "memory");
}

uint64_t pmap_enable_smep_smap(void)
{
	uint64_t want = 0;

	if (cpu_has_smep())
		want |= CR4_SMEP;

	/*
	 * That prediction has since come true, which is worth recording
	 * because it is the argument for turning a protection on before there
	 * is anything for it to protect.
	 *
	 * When this was written no page-table entry carried the user bit, so
	 * SMAP policed nothing; the comment said the deliberate accesses would
	 * need stac and clac once such mappings arrived, and would fault
	 * loudly if they forgot.  #411 made lower-half mappings genuinely
	 * user-reachable, and the next boot on a processor that has SMAP
	 * faulted on exactly one line — the one place the kernel reads through
	 * a user address on purpose.  Not on a machine in the field, and not
	 * as a corruption: as a page fault with an address, in a selftest.
	 */
	if (cpu_has_smap()) {
		want |= CR4_SMAP;
		smap_on = 1;
	}

	if (want)
		write_cr4(read_cr4() | want);

	return want;
}
