/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 boot-time frame allocator (#407, MD contract 2/6).
 */

#include <stdint.h>

#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/pte.h>

/*
 * Enough frames to build the early tables several times over, and small
 * enough that the cost of being wrong is a quarter of a megabyte of .bss.
 * Running out is reported, not worked around: if the early pmap ever needs
 * more than this, the interesting question is why.
 */
#define BOOT_FRAMES	64

static uint8_t boot_pool[BOOT_FRAMES][PAGE_SIZE_4K]
	__attribute__((aligned(4096)));

static unsigned boot_next;

uint64_t boot_frame_alloc(void)
{
	uint64_t pa;
	volatile uint64_t *frame;

	if (boot_next >= BOOT_FRAMES)
		return 0;

	pa = kernel_va_to_phys(&boot_pool[boot_next][0]);
	boot_next++;

	/*
	 * Clear it through the direct map rather than through the pool's own
	 * address.  Both work today, but only one of them still works once
	 * frames come from anywhere in RAM instead of out of the image — so
	 * the allocator is written against that future from the start.
	 *
	 * A page table with a stale entry is far worse than a bad pointer: it
	 * sends the hardware walking into memory nobody accounted for.
	 */
	frame = (volatile uint64_t *)(uintptr_t)phys_to_direct(pa);
	for (unsigned i = 0; i < PAGE_SIZE_4K / sizeof(uint64_t); i++)
		frame[i] = 0;

	return pa;
}

unsigned boot_frames_used(void)
{
	return boot_next;
}

unsigned boot_frames_total(void)
{
	return BOOT_FRAMES;
}
