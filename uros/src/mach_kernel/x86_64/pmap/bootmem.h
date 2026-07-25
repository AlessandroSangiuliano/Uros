/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 boot-time frame allocator (#407, MD contract 2/6).
 *
 * The pmap cannot build a page table without a page to put it in, and the
 * VM's own page allocator does not exist until the VM is up — which needs
 * the pmap.  Something has to break that circle, and this is it: frames
 * handed out one at a time from the memory the loader reported, and never
 * given back.
 *
 * Still deliberately simple — a bump through the usable regions, with no
 * free — but no longer a fixed pool: what the machine has is what there is
 * to spend, which is what makes structures sized against physical memory
 * possible at all.  The real allocator, with a free, arrives with vm_page.
 */

#ifndef _X86_64_PMAP_BOOTMEM_H_
#define _X86_64_PMAP_BOOTMEM_H_

#include <stdint.h>

/*
 * Take the loader's memory map and work out what is free to hand out.
 *
 * Requires direct_map_init() first: frames are cleared through the direct
 * map, so the map has to cover them before any can be given away.
 */
void boot_frame_init(uint32_t info_pa);

/*
 * Physical address of a zeroed 4 KiB frame, or zero when nothing is left.
 * Zero is unambiguous as a failure: the first page of physical memory is
 * never handed out, being below the mark where usable memory starts.
 */
uint64_t boot_frame_alloc(void);

/*
 * Physical address of `count` consecutive zeroed frames, or zero if no one
 * region still has that many left.
 *
 * Consecutive matters: something indexed by page number has to be one array,
 * and through the direct map physically contiguous frames are contiguous
 * virtually too — so a big table needs no mapping of its own, only frames
 * that sit together.
 */
uint64_t boot_frames_alloc(uint64_t count);

uint64_t boot_frames_used(void);
uint64_t boot_frames_total(void);

/*
 * The address below which nothing is allocated: past the kernel image and
 * the loader's own structures.  Reported so the boot log can show what was
 * given up rather than leaving it implicit.
 */
uint64_t boot_frame_low_water(void);

#endif	/* _X86_64_PMAP_BOOTMEM_H_ */
