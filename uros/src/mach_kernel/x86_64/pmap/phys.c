/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 physical page zero and copy (#407, MD contract 2/6).
 *
 * The i386 twin of this file (i386/phys.c) wraps every one of these in
 * kmap()/kunmap(): a physical page above the low-memory limit has no
 * permanent virtual address there, so one has to be borrowed and given back
 * around each access — and pmap_copy_page needs two at once, which is why
 * i386/kmap.h says so out loud.
 *
 * Here the direct map has already given every physical page an address, so
 * a page copy is a copy.  This is the HIGHMEM machinery (#70) losing its
 * reason to exist, exactly as ch.11 §11.4 said it would.
 *
 * Preconditions are the caller's, matching the machine-independent
 * contract: addresses are of real pages, and an offset plus a length stays
 * within the page it names.
 */

#include <stdint.h>

#include <pmap/layout.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>

static inline uint8_t *direct(uint64_t pa)
{
	return (uint8_t *)(uintptr_t)phys_to_direct(pa);
}

/*
 * Whole pages move eight bytes at a time.  Both ends are page-aligned by
 * construction and the length is a multiple of the word, so there is no
 * head or tail to fix up — and the word is twice what the i386 loop could
 * carry, which the port gets for nothing.
 */
static void copy_page_words(uint64_t dst_pa, uint64_t src_pa)
{
	const uint64_t *s = (const uint64_t *)direct(src_pa);
	uint64_t *d = (uint64_t *)direct(dst_pa);

	for (unsigned i = 0; i < PAGE_SIZE_4K / sizeof(uint64_t); i++)
		d[i] = s[i];
}

static void zero_page_words(uint64_t pa)
{
	uint64_t *d = (uint64_t *)direct(pa);

	for (unsigned i = 0; i < PAGE_SIZE_4K / sizeof(uint64_t); i++)
		d[i] = 0;
}

/*
 * Partial runs get a byte loop: neither end is known to be aligned, and
 * these are the rarer paths — the page-sized ones above are what copy-on-
 * write leans on.
 */
static void copy_bytes(uint8_t *d, const uint8_t *s, uint64_t len)
{
	while (len--)
		*d++ = *s++;
}

static void zero_bytes(uint8_t *d, uint64_t len)
{
	while (len--)
		*d++ = 0;
}

void pmap_zero_page(uint64_t p)
{
	zero_page_words(p);
}

void pmap_zero_part_page(uint64_t p, uint64_t offset, uint64_t len)
{
	zero_bytes(direct(p) + offset, len);
}

void pmap_copy_page(uint64_t src, uint64_t dst)
{
	copy_page_words(dst, src);
}

void pmap_copy_part_page(uint64_t src, uint64_t src_offset,
			 uint64_t dst, uint64_t dst_offset, uint64_t len)
{
	copy_bytes(direct(dst) + dst_offset, direct(src) + src_offset, len);
}

/*
 * The asymmetric pair: one side is a physical page reached through the
 * direct map, the other is an address the caller already holds.
 */
void pmap_copy_part_lpage(uint64_t src_va, uint64_t dst,
			  uint64_t dst_offset, uint64_t len)
{
	copy_bytes(direct(dst) + dst_offset,
		   (const uint8_t *)(uintptr_t)src_va, len);
}

void pmap_copy_part_rpage(uint64_t src, uint64_t src_offset,
			  uint64_t dst_va, uint64_t len)
{
	copy_bytes((uint8_t *)(uintptr_t)dst_va,
		   direct(src) + src_offset, len);
}
