/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 physical-to-virtual index (#407, MD contract 2/6).
 */

#include <stdint.h>

#include <kern/lock.h>
#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/pte.h>
#include <pmap/pv.h>
#include <trap/trap.h>

static pv_entry_t pv_head_table;	/* one entry per physical page */
static uint64_t   pv_pages;		/* how many pages it covers */

/*
 * Entries for the second and later mappings of a page.
 *
 * 🔥 THIS LIST IS GLOBAL, AND UNTIL #558 IT HAD NO LOCK AT ALL (09/2026).
 *
 * `pv_alloc' popped and `pv_free' pushed with neither lock nor atomics, so two
 * processors mapping second-or-later mappings of DIFFERENT pages -- each under
 * its own object lock, which is why nothing else caught it -- could be handed
 * the SAME entry.  One page's list then grafts onto another's, and a walk from
 * one page arrives at a `pv->pmap' belonging to somewhere else entirely.  That
 * is a general protection fault when the value is wild enough to leave the
 * canonical range, and silent corruption when it is not.
 */
static pv_entry_t pv_free_list;
decl_simple_lock_data(static, pv_free_lock)

#define pa_index(pa)	((pa) >> PT_SHIFT)

/*
 * 🔥 THE LISTS THEMSELVES (#558).
 *
 * Half of this index was protected by accident and that is why it lasted: the
 * VM paths reach a page's list with that page's OBJECT locked.  `pmap_destroy'
 * does not -- it reaches pv_remove() through its own page tables, rewriting the
 * list of every page those tables map, pages belonging to arbitrary objects.
 * Two different locks are no exclusion at all, and pv_remove() removing the
 * head COPIES the next entry's contents up and frees that entry, so a reader
 * standing on the list can have the entry under it recycled mid-walk.
 *
 * 🔑 ONE LOCK PER PAGE WOULD COST 8 MB ON 4 GB of RAM, on a table that is
 * already 24 bytes a page.  A hashed array is the same protection for a fixed
 * size, and the size is chosen against CONCURRENT PAGES rather than against
 * processors: with N locks and k operations in flight the collision rate is
 * about k*k/2N, so 64 processors all in the index want thousands, not tens.
 * 1024 gives about half a collision at that width and costs nothing here.
 *
 * ⚠️ AND THEY ARE PADDED TO A CACHE LINE, which is where the memory goes:
 * simple_lock_data_t is ONE byte, so unpadded there would be sixty-four locks
 * per line and two processors taking two DIFFERENT locks would ping-pong the
 * line between them -- contention invented by the layout, invisible in the
 * code.  1024 * 64 B = 64 KB, a quarter of a per-cent of the table it guards.
 *
 * 🔴 LOCK ORDER, and there is only one: a page's head lock may be taken with
 * the free-list lock DOWN and then take it (pv_remove -> pv_free).  Never the
 * other way: pv_enter() takes its entry from pv_alloc() BEFORE locking the
 * head, which is also what keeps a blocking allocation out of the section.
 */
#define PV_LOCKS	1024		/* power of two: the index is a mask */

struct pv_lock {
	decl_simple_lock_data(, l)
	char	pad[1];			/* never an empty struct */
} __attribute__((aligned(64)));

static struct pv_lock pv_locks[PV_LOCKS];

static struct pv_lock *pv_lock_for(uint64_t pa)
{
	return &pv_locks[pa_index(pa) & (PV_LOCKS - 1)];
}

/*
 * For the walks that live in pmap.c and must hold the list still across a whole
 * traversal.  ⚠️ NOT for pmap_page_protect's VM_PROT_NONE loop: that one calls
 * pmap_forget(), which calls pv_remove() on the SAME pa, and holding this
 * across it is a self-deadlock -- the shape of #486.  That loop re-reads
 * pv_head(pa) every iteration instead, which is why it was written that way.
 */
void pv_lock_page(uint64_t pa)
{
	if (pv_managed(pa))
		simple_lock(&pv_lock_for(pa)->l);
}

void pv_unlock_page(uint64_t pa)
{
	if (pv_managed(pa))
		simple_unlock(&pv_lock_for(pa)->l);
}

void pv_bootstrap(uint64_t top_of_ram)
{
	uint64_t pages = top_of_ram >> PT_SHIFT;
	uint64_t bytes = pages * sizeof(struct pv_entry);
	uint64_t frames = (bytes + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
	uint64_t table_pa = boot_frames_alloc(frames);

	/*
	 * Without the table every pv_managed() is false, so pv_enter and
	 * pv_remove become no-ops and pmap_page_protect goes on to find no
	 * mappings for pages that have them.  Copy-on-write would arm
	 * nothing, and say so to nobody.
	 */
	if (table_pa == 0)
		panic("pv: no memory for the physical-to-virtual index");

	/*
	 * Reached through the direct map: the frames are consecutive, so the
	 * array is contiguous there too, and needs no mapping of its own.
	 * boot_frames_alloc() returned it zeroed, which is already the right
	 * initial state — a head with no pmap is a page with no mappings.
	 */
	pv_head_table = (pv_entry_t)(uintptr_t)phys_to_direct(table_pa);
	pv_pages = pages;

	simple_lock_init(&pv_free_lock, ETAP_VM_PMAP_FREE);
	for (unsigned i = 0; i < PV_LOCKS; i++)
		simple_lock_init(&pv_locks[i].l, ETAP_VM_PMAP);
}

int pv_managed(uint64_t pa)
{
	return pv_head_table != PV_ENTRY_NULL && pa_index(pa) < pv_pages;
}

pv_entry_t pv_head(uint64_t pa)
{
	if (!pv_managed(pa))
		return PV_ENTRY_NULL;

	return &pv_head_table[pa_index(pa)];
}

/*
 * Entries come from the free list, refilled a frame at a time.  A frame
 * holds many, and freed entries come back here — so the churn of mapping
 * and unmapping does not consume memory, even though the frame allocator
 * beneath has no free of its own.
 */
static pv_entry_t pv_alloc(void)
{
	pv_entry_t e;
	uint64_t frame;
	unsigned per_frame = PAGE_SIZE_4K / sizeof(struct pv_entry);

	simple_lock(&pv_free_lock);
	if (pv_free_list != PV_ENTRY_NULL) {
		e = pv_free_list;
		pv_free_list = e->next;
		simple_unlock(&pv_free_lock);
		return e;
	}
	simple_unlock(&pv_free_lock);

	/*
	 * pmap_table_frame(), same class as pmap_create()'s and the large-page
	 * split's (#422): the boot allocator is empty once the VM has taken
	 * physical memory over, and this runs on every second mapping of a page
	 * for as long as the system is up -- so with boot_frame_alloc() the
	 * panic in pv_enter() was not a safeguard, it was a wall a few mappings
	 * away.
	 *
	 * 🔴 AND IT IS CALLED WITH THE LOCK DOWN, WHICH IS THE WHOLE SHAPE OF
	 * THIS FUNCTION.  It can block -- after the VM is up it is vm_page_grab
	 * with a VM_PAGE_WAIT behind it -- and blocking while holding a spin
	 * lock stops every other processor that wants an entry, on a lock whose
	 * holder is asleep.  It is the same rule map.c states for the QSBR read
	 * section next to the large-page split, for the same reason and one
	 * step stronger: "THE FRAME IS TAKEN BEFORE THE SECTION OPENS".
	 *
	 * ⚠️ Two processors that both miss both take a frame, and both thread
	 * theirs in.  That is not a leak and needs no undoing: an extra frame's
	 * worth of entries is simply free entries, and the alternative -- hold
	 * the lock across the allocation to keep the second one from happening
	 * -- is the thing this arrangement exists to avoid.
	 */
	frame = pmap_table_frame();
	if (frame == 0)
		return PV_ENTRY_NULL;

	e = (pv_entry_t)(uintptr_t)phys_to_direct(frame);

	simple_lock(&pv_free_lock);
	for (unsigned i = 0; i < per_frame; i++) {
		e[i].next = pv_free_list;
		pv_free_list = &e[i];
	}
	e = pv_free_list;
	pv_free_list = e->next;
	simple_unlock(&pv_free_lock);

	return e;
}

static void pv_free(pv_entry_t e)
{
	e->pmap = PMAP_NULL;

	simple_lock(&pv_free_lock);
	e->next = pv_free_list;
	pv_free_list = e;
	simple_unlock(&pv_free_lock);
}

void pv_enter(uint64_t pa, pmap_t pmap, uint64_t va)
{
	pv_entry_t head = pv_head(pa);
	pv_entry_t e;

	if (head == PV_ENTRY_NULL)
		return;

	/*
	 * 🔴 THE ENTRY IS TAKEN BEFORE THE LOCK, ALWAYS -- even on the first
	 * mapping, which will not need it.
	 *
	 * pv_alloc() can refill, and refilling calls pmap_table_frame(), which
	 * BLOCKS.  Sleeping while holding this page's spin lock would stop
	 * every other processor that touches the page, on a lock whose holder
	 * is asleep.  So the allocation happens outside and the spare is handed
	 * back if the list turns out to be empty: map.c's large-page split does
	 * exactly this, and says why -- "allocate outside, decide inside, give
	 * the page back if you lost the argument".
	 *
	 * ⚠️ The cost is an alloc and a free on the common path, which is the
	 * price of not holding a lock across a wait.  It is paid once per
	 * mapping, inside a page fault.
	 *
	 * The mapping exists whether or not it is recorded, and an unrecorded
	 * one is invisible to every operation that starts from the physical
	 * page — so pmap_page_protect would leave it writable while reporting
	 * that it had protected the page.  There is no honest way to return
	 * from here.
	 */
	e = pv_alloc();
	if (e == PV_ENTRY_NULL)
		panic("pv: out of entries, a mapping would go unrecorded");

	pv_lock_page(pa);

	/* The common case: first mapping of this page, the spare goes back. */
	if (head->pmap == PMAP_NULL) {
		head->pmap = pmap;
		head->va = va;
		head->next = PV_ENTRY_NULL;
		pv_unlock_page(pa);
		pv_free(e);
		return;
	}

	/*
	 * Push in after the head rather than at the end: the head cannot move,
	 * since its address is what identifies the page.
	 */
	e->pmap = pmap;
	e->va = va;
	e->next = head->next;
	head->next = e;

	pv_unlock_page(pa);
}

void pv_remove(uint64_t pa, pmap_t pmap, uint64_t va)
{
	pv_entry_t head = pv_head(pa);
	pv_entry_t prev, e;

	if (head == PV_ENTRY_NULL)
		return;

	pv_lock_page(pa);

	if (head->pmap == PMAP_NULL) {
		pv_unlock_page(pa);
		return;
	}

	/*
	 * Removing the head keeps the head where it is: the next entry's
	 * contents are copied up into it and that entry is freed.
	 *
	 * 🔥 THOSE THREE STORES ARE WHY A READER NEEDS THIS LOCK AND NOT A
	 * GRACE PERIOD.  A reader walking without it can take `pmap' from the
	 * entry that was here and `va' from the one that replaced it -- an
	 * entry that never existed, naming a real address in the wrong space.
	 * That does not fault; it protects the wrong page, silently.  Deferring
	 * the free would stop the use-after-free and leave this untouched.
	 */
	if (head->pmap == pmap && head->va == va) {
		e = head->next;
		if (e == PV_ENTRY_NULL) {
			head->pmap = PMAP_NULL;
			pv_unlock_page(pa);
			return;
		}
		head->pmap = e->pmap;
		head->va = e->va;
		head->next = e->next;
		pv_unlock_page(pa);
		pv_free(e);
		return;
	}

	prev = head;
	for (e = head->next; e != PV_ENTRY_NULL; prev = e, e = e->next)
		if (e->pmap == pmap && e->va == va) {
			prev->next = e->next;
			pv_unlock_page(pa);
			pv_free(e);
			return;
		}

	pv_unlock_page(pa);
}

unsigned pv_count(uint64_t pa)
{
	pv_entry_t head = pv_head(pa);
	unsigned n = 0;

	if (head == PV_ENTRY_NULL)
		return 0;

	pv_lock_page(pa);
	if (head->pmap != PMAP_NULL)
		for (pv_entry_t e = head; e != PV_ENTRY_NULL; e = e->next)
			n++;
	pv_unlock_page(pa);

	return n;
}
