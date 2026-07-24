/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 boot-time frame allocator (#407, MD contract 2/6).
 *
 * The pmap cannot build a page table without a page to put it in, and the
 * VM's own page allocator does not exist until the VM is up — which needs
 * the pmap.  Something has to break that circle, and this is it: a small
 * fixed pool, handed out one frame at a time and never freed.
 *
 * Deliberately the dumbest allocator that can work.  It exists to get the
 * first tables built; the real physical allocator arrives with the memory
 * map and vm_page, and this one stops being called.
 */

#ifndef _X86_64_PMAP_BOOTMEM_H_
#define _X86_64_PMAP_BOOTMEM_H_

#include <stdint.h>

/*
 * Physical address of a zeroed 4 KiB frame, or zero when the pool is spent.
 * Zero doubles as the failure value here, unlike in the walk: this pool is
 * in the kernel image, so physical address zero is never one of its frames.
 *
 * Requires direct_map_init(), since the frame is cleared through the direct
 * map — the same way it will be when frames stop coming from the image.
 */
uint64_t boot_frame_alloc(void);

unsigned boot_frames_used(void);
unsigned boot_frames_total(void);

#endif	/* _X86_64_PMAP_BOOTMEM_H_ */
