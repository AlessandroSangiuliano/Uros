/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 boot-time frame allocator (#407, MD contract 2/6).
 */

#include <stdint.h>

#include <boot/multiboot2.h>
#include <pmap/bootmem.h>
#include <pmap/direct.h>
#include <pmap/layout.h>
#include <pmap/pte.h>

/*
 * Usable regions, in the order they will be spent.  Sixteen is well past
 * what any machine reports: the memory map is a handful of entries, not a
 * catalogue of pages.
 */
#define BOOT_MAX_REGIONS	16

struct boot_region {
	uint64_t next;			/* first frame not yet handed out */
	uint64_t end;
};

static struct boot_region regions[BOOT_MAX_REGIONS];
static unsigned nregions;
static unsigned current;

static uint64_t frames_total;
static uint64_t frames_used;
static uint64_t low_water;

/* Frames handed back, threaded through their own first word. */
static uint64_t free_list;

/* Physical extent of the loaded image, from the linker. */
extern char __kernel_end[];

static uint64_t round_up_page(uint64_t v)
{
	return (v + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
}

/*
 * Everything below one mark is abandoned rather than carved around.
 *
 * Below it lie the things that must not be handed out: the first megabyte,
 * which belongs to the firmware and to the real-mode world an AP will start
 * in; the loaded image, boot page tables and boot stack included; and the
 * loader's own boot-information structure, which is still being read.
 *
 * Subtracting those individually would mean a range-splitting machine used
 * once and never again.  A single mark costs the memory below it — a
 * rounding error against any real machine's RAM — and cannot get the
 * arithmetic wrong.
 */
static uint64_t compute_low_water(uint32_t info_pa)
{
	uint64_t mark = 1024 * 1024;
	uint64_t image_end = kernel_va_to_phys(__kernel_end);
	uint64_t info_end;

	if (image_end > mark)
		mark = image_end;

	if (info_pa != 0) {
		/* First word of the structure is its own total size. */
		info_end = info_pa + *(const uint32_t *)(uintptr_t)info_pa;
		if (info_end > mark)
			mark = info_end;
	}

	return round_up_page(mark);
}

void boot_frame_init(uint32_t info_pa)
{
	const struct mb2_tag_mmap *mm;
	const uint8_t *p, *end;

	low_water = compute_low_water(info_pa);

	mm = (const struct mb2_tag_mmap *)mb2_find_tag(info_pa,
						       MB2_TAG_MEMORY_MAP);
	if (mm == 0 || mm->entry_size < sizeof(struct mb2_mmap_entry))
		return;

	p = (const uint8_t *)mm + sizeof(*mm);
	end = (const uint8_t *)mm + mm->size;

	for (; p + mm->entry_size <= end; p += mm->entry_size) {
		const struct mb2_mmap_entry *e;
		uint64_t start, stop;

		e = (const struct mb2_mmap_entry *)p;
		if (e->type != MB2_MEM_AVAILABLE)
			continue;

		start = round_up_page(e->addr);
		stop = (e->addr + e->len) & ~(PAGE_SIZE_4K - 1);

		if (start < low_water)
			start = low_water;

		/*
		 * Only what the direct map reaches: a frame is cleared, and
		 * later read, through that mapping, so one beyond it could be
		 * allocated but never touched.
		 */
		if (stop > direct_map_covered)
			stop = direct_map_covered;

		if (start >= stop || nregions == BOOT_MAX_REGIONS)
			continue;

		regions[nregions].next = start;
		regions[nregions].end = stop;
		nregions++;
		frames_total += (stop - start) / PAGE_SIZE_4K;
	}
}

uint64_t boot_frames_alloc(uint64_t count)
{
	uint64_t pa, bytes = count * PAGE_SIZE_4K;
	volatile uint64_t *frames;

	if (count == 0)
		return 0;

	/* Retire regions that are spent; they will never serve again. */
	while (current < nregions
	       && regions[current].next >= regions[current].end)
		current++;

	/*
	 * Then take the run from the first region with room for all of it —
	 * consecutive is the point, and a run cannot straddle the gap between
	 * two regions.  Searching rather than advancing past the ones that are
	 * merely too small keeps their remainder available to the next request
	 * that does fit.
	 */
	{
		unsigned i = current;

		while (i < nregions
		       && regions[i].end - regions[i].next < bytes)
			i++;

		if (i >= nregions)
			return 0;

		pa = regions[i].next;
		regions[i].next += bytes;
	}

	frames_used += count;

	/*
	 * Clear it through the direct map.  A page table with a stale entry
	 * is far worse than a bad pointer: it sends the hardware walking into
	 * memory nobody accounted for.
	 */
	frames = (volatile uint64_t *)(uintptr_t)phys_to_direct(pa);
	for (uint64_t i = 0; i < bytes / sizeof(uint64_t); i++)
		frames[i] = 0;

	return pa;
}

void boot_frame_free(uint64_t pa)
{
	uint64_t *link = (uint64_t *)(uintptr_t)phys_to_direct(pa);

	*link = free_list;
	free_list = pa;
	frames_used--;
}

uint64_t boot_frame_alloc(void)
{
	uint64_t pa;
	volatile uint64_t *frame;

	if (free_list == 0)
		return boot_frames_alloc(1);

	pa = free_list;
	free_list = *(const uint64_t *)(uintptr_t)phys_to_direct(pa);
	frames_used++;

	/*
	 * Cleared on the way out, not on the way in: a frame on the free list
	 * still holds the link that put it there, and whatever the previous
	 * owner left.  Callers are promised a zeroed frame, and a page table
	 * built on stale entries walks into memory nobody accounted for.
	 */
	frame = (volatile uint64_t *)(uintptr_t)phys_to_direct(pa);
	for (unsigned i = 0; i < PAGE_SIZE_4K / sizeof(uint64_t); i++)
		frame[i] = 0;

	return pa;
}

uint64_t boot_frames_used(void)
{
	return frames_used;
}

uint64_t boot_frames_total(void)
{
	return frames_total;
}

uint64_t boot_frame_low_water(void)
{
	return low_water;
}
