/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 physical-to-virtual index (#407, MD contract 2/6).
 */

#include <stdint.h>

#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/pte.h>
#include <pmap/pv.h>
#include <trap/trap.h>

static pv_entry_t pv_head_table;	/* one entry per physical page */
static uint64_t   pv_pages;		/* how many pages it covers */

/* Entries for the second and later mappings of a page. */
static pv_entry_t pv_free_list;

#define pa_index(pa)	((pa) >> PT_SHIFT)

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

	if (pv_free_list == PV_ENTRY_NULL) {
		/*
		 * pmap_table_frame(), same class as pmap_create()'s and the
		 * large-page split's (#422): the boot allocator is empty once
		 * the VM has taken physical memory over, and this runs on every
		 * second mapping of a page for as long as the system is up --
		 * so with boot_frame_alloc() the panic below was not a
		 * safeguard, it was a wall a few mappings away.
		 *
		 * ⚠️ It can block, and today that is safe because nothing holds
		 * a lock across it: pmap_enter() calls pv_enter() with none
		 * taken, this pmap having no locking yet.  When it gains some
		 * (#455), this call is the first place to look.
		 */
		frame = pmap_table_frame();
		if (frame == 0)
			return PV_ENTRY_NULL;

		e = (pv_entry_t)(uintptr_t)phys_to_direct(frame);
		for (unsigned i = 0; i < per_frame; i++) {
			e[i].next = pv_free_list;
			pv_free_list = &e[i];
		}
	}

	e = pv_free_list;
	pv_free_list = e->next;
	return e;
}

static void pv_free(pv_entry_t e)
{
	e->pmap = PMAP_NULL;
	e->next = pv_free_list;
	pv_free_list = e;
}

void pv_enter(uint64_t pa, pmap_t pmap, uint64_t va)
{
	pv_entry_t head = pv_head(pa);
	pv_entry_t e;

	if (head == PV_ENTRY_NULL)
		return;

	/* The common case: first mapping of this page, no allocation. */
	if (head->pmap == PMAP_NULL) {
		head->pmap = pmap;
		head->va = va;
		head->next = PV_ENTRY_NULL;
		return;
	}

	/*
	 * The mapping exists whether or not it is recorded, and an unrecorded
	 * one is invisible to every operation that starts from the physical
	 * page — so pmap_page_protect would leave it writable while reporting
	 * that it had protected the page.  There is no honest way to return
	 * from here.
	 */
	e = pv_alloc();
	if (e == PV_ENTRY_NULL)
		panic("pv: out of entries, a mapping would go unrecorded");

	/*
	 * Push in after the head rather than at the end: the head cannot move,
	 * since its address is what identifies the page.
	 */
	e->pmap = pmap;
	e->va = va;
	e->next = head->next;
	head->next = e;
}

void pv_remove(uint64_t pa, pmap_t pmap, uint64_t va)
{
	pv_entry_t head = pv_head(pa);
	pv_entry_t prev, e;

	if (head == PV_ENTRY_NULL || head->pmap == PMAP_NULL)
		return;

	/*
	 * Removing the head keeps the head where it is: the next entry's
	 * contents are copied up into it and that entry is freed.
	 */
	if (head->pmap == pmap && head->va == va) {
		e = head->next;
		if (e == PV_ENTRY_NULL) {
			head->pmap = PMAP_NULL;
			return;
		}
		head->pmap = e->pmap;
		head->va = e->va;
		head->next = e->next;
		pv_free(e);
		return;
	}

	prev = head;
	for (e = head->next; e != PV_ENTRY_NULL; prev = e, e = e->next)
		if (e->pmap == pmap && e->va == va) {
			prev->next = e->next;
			pv_free(e);
			return;
		}
}

unsigned pv_count(uint64_t pa)
{
	pv_entry_t head = pv_head(pa);
	unsigned n = 0;

	if (head == PV_ENTRY_NULL || head->pmap == PMAP_NULL)
		return 0;

	for (pv_entry_t e = head; e != PV_ENTRY_NULL; e = e->next)
		n++;

	return n;
}
