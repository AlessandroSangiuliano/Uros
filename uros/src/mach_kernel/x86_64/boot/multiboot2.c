/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Reading the multiboot2 handoff (#406/#407).
 */

#include <stdint.h>

#include <boot/multiboot2.h>

const struct mb2_tag *mb2_find_tag(uint32_t info_pa, uint32_t type)
{
	const uint8_t *base = (const uint8_t *)(uintptr_t)info_pa;
	uint32_t total;
	uint32_t off = 8;			/* past total_size + reserved */

	if (info_pa == 0)
		return 0;

	total = *(const uint32_t *)base;

	while (off + sizeof(struct mb2_tag) <= total) {
		const struct mb2_tag *tag;

		tag = (const struct mb2_tag *)(base + off);
		if (tag->type == MB2_TAG_END)
			return 0;
		if (tag->type == type)
			return tag;

		/*
		 * A tag shorter than its own header would leave the offset
		 * standing still — refuse rather than loop forever on a
		 * malformed structure we did not write.
		 */
		if (tag->size < sizeof(struct mb2_tag))
			return 0;

		off += (tag->size + 7) & ~7u;	/* tags are 8-byte aligned */
	}

	return 0;
}

/*
 * Walk the memory-map entries, calling back with each available region.
 * entry_size comes from the tag rather than sizeof: the format is versioned
 * and explicitly allowed to grow, so striding by our own struct would
 * misread a loader newer than this code.
 */
static void mb2_for_each_ram(uint32_t info_pa,
			     void (*fn)(uint64_t addr, uint64_t len, void *arg),
			     void *arg)
{
	const struct mb2_tag_mmap *mm;
	const uint8_t *p, *end;

	mm = (const struct mb2_tag_mmap *)mb2_find_tag(info_pa,
						       MB2_TAG_MEMORY_MAP);
	if (mm == 0 || mm->entry_size < sizeof(struct mb2_mmap_entry))
		return;

	p = (const uint8_t *)mm + sizeof(*mm);
	end = (const uint8_t *)mm + mm->size;

	for (; p + mm->entry_size <= end; p += mm->entry_size) {
		const struct mb2_mmap_entry *e;

		e = (const struct mb2_mmap_entry *)p;
		if (e->type == MB2_MEM_AVAILABLE)
			fn(e->addr, e->len, arg);
	}
}

static void top_cb(uint64_t addr, uint64_t len, void *arg)
{
	uint64_t *top = arg;

	if (addr + len > *top)
		*top = addr + len;
}

static void sum_cb(uint64_t addr, uint64_t len, void *arg)
{
	uint64_t *sum = arg;

	(void)addr;
	*sum += len;
}

uint64_t mb2_top_of_ram(uint32_t info_pa)
{
	uint64_t top = 0;

	mb2_for_each_ram(info_pa, top_cb, &top);
	return top;
}

uint64_t mb2_usable_ram(uint32_t info_pa)
{
	uint64_t sum = 0;

	mb2_for_each_ram(info_pa, sum_cb, &sum);
	return sum;
}
