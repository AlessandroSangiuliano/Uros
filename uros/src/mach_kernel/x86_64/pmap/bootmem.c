/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 boot-time frame allocator (#407, MD contract 2/6).
 */

#include <stdint.h>

#include <boot/multiboot2.h>
#include <ddb/ksym.h>
#include <ddb/ramoops.h>	/* #373: the zone cut out */
#include <kern/misc_protos.h>
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

/*
 * #373: the one range cut out of a region rather than left below the low
 * water mark: the megabyte Linux reserves at ramoops=ADDR and reads back
 * after this kernel's reset (ddb/ramoops.c).  It is near the top of memory,
 * where a mark cannot reach, so the region holding it is split in two around
 * it -- the range-splitting low_water's comment below declines, needed for
 * this one range.  Zero end: not cut.
 */
static uint64_t cut_start, cut_end;

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

	/*
	 * The symbol tables the loader placed in low memory (#428).
	 *
	 * ⚠️ Measured, and it is currently a no-op: this GRUB puts the
	 * information structure immediately *above* the symbol data, so the
	 * check below already covers it. Under gdb — image_end 0x16b000,
	 * syms_end 0x1bef29, info 0x1bef30 + 0x760 = 0x1bf690.
	 *
	 * It stays because that adjacency is the loader's arrangement and not
	 * a promise. If it ever changes, handing out the page holding the
	 * symbol table would make every later backtrace name functions read
	 * out of a page table — and a wrong name is worse than no name, since
	 * it sends the reader somewhere and looks exactly like a right one.
	 *
	 * Insurance, in other words, and labelled as insurance: an earlier
	 * version of this comment claimed it was fixing a live problem, on the
	 * strength of a comparison between two builds that differed in two
	 * things at once.
	 */
	{
		uint64_t syms_end = ksym_data_end(info_pa);

		if (syms_end > mark)
			mark = syms_end;
	}

	if (info_pa != 0) {
		/* First word of the structure is its own total size. */
		info_end = info_pa + *(const uint32_t *)(uintptr_t)info_pa;
		if (info_end > mark)
			mark = info_end;
	}

	/*
	 * 🔥 AND THE BOOT MODULES, which were not here and had to be (#422).
	 *
	 * The loader puts modules where it likes.  Everything above accounts for
	 * things the kernel or the loader placed in a known relation to the
	 * image; a module is placed in no relation to anything, and this GRUB
	 * puts it ABOVE the information structure — so the mark stopped below it
	 * and boot_frame_alloc() handed out the pages holding the first user
	 * program.
	 *
	 * ⚠️ What that looks like is not an allocation failure.  The module is
	 * still described, its address and size are still right, and the memory
	 * at that address reads as whatever the kernel has since put there —
	 * zeroes, in every boot measured.  The first thing to notice it was the
	 * ELF check three layers up saying "boot image is not ELF (starts 00 00
	 * 00 00)", and without that check it would have been a task that started
	 * and died somewhere else entirely.
	 *
	 * The same class the symbol-table clause above guards against, with one
	 * difference worth stating: that one is insurance against an arrangement
	 * that happens to hold, and this one was a live defect from the first
	 * boot that carried a module.
	 */
	for (unsigned n = 0; ; n++) {
		uint64_t start, len;

		if (!mb2_module_range(n, &start, &len))
			break;

		if (start + len > mark)
			mark = start + len;
	}

	return round_up_page(mark);
}

static void add_region(uint64_t start, uint64_t stop)
{
	if (start >= stop || nregions == BOOT_MAX_REGIONS)
		return;

	regions[nregions].next = start;
	regions[nregions].end = stop;
	nregions++;
	frames_total += (stop - start) / PAGE_SIZE_4K;
}

void boot_frame_init(uint32_t info_pa)
{
	const struct mb2_tag_mmap *mm;
	const uint8_t *p, *end;
	uint64_t zone = 0;
	int want_zone = ramoops_wanted(&zone) == 1;

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

		/*
		 * #373: the zone, cut out of the region that holds all of it,
		 * here where every frame this kernel will own is first counted
		 * -- the VM takes its pages through boot_frame_alloc() and
		 * nothing else -- so none of its frames is ever handed out,
		 * cleared or made a page table.  Only whole: a zone that runs
		 * past a region's end, or below the low water mark into the
		 * kernel and its modules, is not cut, and ddb/ramoops.c then
		 * writes nothing there.  Nor when the split would need a slot
		 * there is not: the zone goes rather than the memory past it.
		 */
		if (want_zone && cut_end == 0 && zone >= start
		    && zone + RAMOOPS_RESERVED <= stop) {
			unsigned need = (zone > start)
					+ (zone + RAMOOPS_RESERVED < stop);

			if (nregions + need <= BOOT_MAX_REGIONS) {
				add_region(start, zone);
				add_region(zone + RAMOOPS_RESERVED, stop);
				cut_start = zone;
				cut_end = zone + RAMOOPS_RESERVED;
				continue;
			}
		}

		add_region(start, stop);
	}
}

int boot_frames_cut(uint64_t pa, uint64_t len)
{
	return cut_end != 0 && pa == cut_start && pa + len == cut_end;
}

/*
 * Zero `bytes' of physical memory at `pa', through the direct map.  The two
 * allocators below each had this loop, and differed only in the bound (#605).
 */
static void zero_frames(uint64_t pa, uint64_t bytes)
{
	volatile uint64_t *w = (volatile uint64_t *)(uintptr_t)phys_to_direct(pa);

	for (uint64_t i = 0; i < bytes / sizeof(uint64_t); i++)
		w[i] = 0;
}

uint64_t boot_frames_alloc(uint64_t count)
{
	uint64_t pa, bytes = count * PAGE_SIZE_4K;

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

	/*
	 * #373: never a frame of the zone.  Once it is cut out nothing here can
	 * reach one, so this is what says the cut took: a cut recorded and not
	 * made leaves the frames in their region, the VM's startup takes every
	 * frame there is through this function, and the boot stops here naming
	 * them -- which is how ablations/373-zone-not-cut.patch is caught.
	 */
	if (cut_end != 0 && pa < cut_end && pa + bytes > cut_start)
		panic("boot_frames_alloc: frames 0x%llx..0x%llx are inside the "
		      "megabyte cut out for ramoops at 0x%llx (#373)",
		      (unsigned long long)pa, (unsigned long long)(pa + bytes - 1),
		      (unsigned long long)cut_start);

	frames_used += count;

	/*
	 * Clear it through the direct map.  A page table with a stale entry
	 * is far worse than a bad pointer: it sends the hardware walking into
	 * memory nobody accounted for.
	 */
	zero_frames(pa, bytes);

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
	zero_frames(pa, PAGE_SIZE_4K);

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
