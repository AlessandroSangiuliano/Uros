/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 kernel address-space layout (#407, MD contract 2/6).
 *
 * The map decided in #405 and written down in docs/uros_design.md ch.11,
 * expressed the way that chapter requires it: every region is an offset
 * from one anchor, KERNEL_HALF_BASE, and none of them is a hardcoded
 * constant.
 *
 * That is not tidiness for its own sake.  The canonical higher-half base
 * *moves* when a paging level is added — from the bit-47 sign extension at
 * 0xffff800000000000 to the bit-56 one at 0xff00000000000000.  With the
 * regions written as offsets, five-level paging is a change to this single
 * anchor plus the walk depth, and the whole map relocates consistently.
 * Written as literals, it would be a region-by-region repaint, which is
 * exactly the kind of promise that quietly stops being true.
 *
 * The gaps between regions are deliberate too: the bases are spaced far
 * wider than the regions need, so an overrun walks into a hole with no
 * mapping rather than into the next region.  The guard is the layout.
 *
 *   0xffff800000000000  direct map      all of physical RAM, 1 GiB pages, NX
 *   0xffffc00000000000  kernel heap     dynamic allocation, NX by default
 *   0xffffe00000000000  per-CPU         %gs-based per-CPU areas
 *   0xfffff00000000000  CPU entry area  entry text, IST stacks, GDT, TSS
 *   0xffffffff80000000  kernel image    .text/.rodata/.data/.bss, W^X
 */

#ifndef _X86_64_LAYOUT_H_
#define _X86_64_LAYOUT_H_

#include <stdint.h>

/*
 * Four-level paging: 48 significant bits, so an address is canonical only
 * when bits 63:48 repeat bit 47.  This is the number that becomes 57 the
 * day PML5 arrives.
 */
#define VA_BITS			48
#define VA_SIGN_SHIFT		(64 - VA_BITS)

/* Sign-extend bit VA_BITS-1 the way the hardware does. */
#define va_canonical(va)						\
	((uint64_t)(((int64_t)((uint64_t)(va) << VA_SIGN_SHIFT))		\
		    >> VA_SIGN_SHIFT))

#define va_is_canonical(va)	(va_canonical(va) == (uint64_t)(va))

/*
 * The anchor: the first address of the kernel half.  Everything below is
 * measured from here.
 */
#define KERNEL_HALF_BASE	0xffff800000000000ULL

/*
 * Which half an address belongs to, which on this architecture is the same
 * question as who it belongs to: §11.1 gives the lower half to the address
 * space and the upper half to the kernel, with nothing in between that is
 * addressable at all.
 *
 * So a mapping's side is a property of its address and does not have to be
 * passed alongside it — which matters at the one place that has to decide
 * whether a page is reachable from ring 3, and would otherwise be trusting
 * a caller to say so.
 */
#define va_is_kernel(va)	((uint64_t)(va) >= KERNEL_HALF_BASE)
#define va_is_user(va)		(!va_is_kernel(va))

/* Offsets of each region from the anchor. */
#define DIRECT_MAP_OFFSET	0x0000000000000000ULL
#define KERNEL_HEAP_OFFSET	0x0000400000000000ULL
#define PERCPU_OFFSET		0x0000600000000000ULL
#define CPU_ENTRY_OFFSET	0x0000700000000000ULL
#define DEVICE_MAP_OFFSET	0x0000780000000000ULL
#define KERNEL_IMAGE_OFFSET	0x00007fff80000000ULL

#define DIRECT_MAP_BASE		(KERNEL_HALF_BASE + DIRECT_MAP_OFFSET)
#define KERNEL_HEAP_BASE	(KERNEL_HALF_BASE + KERNEL_HEAP_OFFSET)
#define PERCPU_BASE		(KERNEL_HALF_BASE + PERCPU_OFFSET)
#define CPU_ENTRY_BASE		(KERNEL_HALF_BASE + CPU_ENTRY_OFFSET)
#define KERNEL_IMAGE_BASE	(KERNEL_HALF_BASE + KERNEL_IMAGE_OFFSET)

/*
 * Where device registers are made reachable.
 *
 * Not RAM, and so not the direct map's business twice over: the physical
 * addresses lie outside it on any machine with less memory than the local
 * APIC's address, and the direct map is cached, which for registers is not
 * a performance choice but a wrong answer — a read that comes from a cache
 * line is a read of the past, and a write that sits in one is an order the
 * device never received.
 *
 * The local APIC is the first tenant; the IOAPIC, the HPET and the PCI
 * windows of #427 are the rest.
 */
#define DEVICE_MAP_BASE		(KERNEL_HALF_BASE + DEVICE_MAP_OFFSET)
#define DEVICE_MAP_MAX_SIZE	(KERNEL_IMAGE_OFFSET - DEVICE_MAP_OFFSET)

/*
 * The image occupies the top 2 GiB, which is not a choice: it is exactly
 * the span -mcmodel=kernel can address with sign-extended 32-bit offsets.
 */
#define KERNEL_IMAGE_SIZE	0x0000000080000000ULL

/* How far the direct map may run before it would reach the heap. */
#define DIRECT_MAP_MAX_SIZE	(KERNEL_HEAP_OFFSET - DIRECT_MAP_OFFSET)

/*
 * Containment test, written as a subtraction rather than the obvious
 * (va >= base && va < base + size).
 *
 * The kernel image ends at the very top of the address space, so its
 * base + size is 2^64 and wraps to zero — the obvious form then rejects
 * every address in the region.  Unsigned wraparound makes the subtraction
 * correct for that region and every other one, so it is the only form
 * offered here.
 */
#define va_in_region(va, base, size)					\
	(((uint64_t)(va) - (uint64_t)(base)) < (uint64_t)(size))

#define va_in_kernel_image(va)						\
	va_in_region(va, KERNEL_IMAGE_BASE, KERNEL_IMAGE_SIZE)

#define va_in_direct_map(va)						\
	va_in_region(va, DIRECT_MAP_BASE, DIRECT_MAP_MAX_SIZE)

/*
 * Physical address of something in the kernel image.
 *
 * boot.ld links the image at KERNEL_IMAGE_BASE while setting each section's
 * load address to VMA - KERNEL_IMAGE_BASE, so the bootloader deposits the
 * image at that physical address and the translation is this one
 * subtraction.  It holds for the image and nothing else: the direct map has
 * its own offset, and an arbitrary mapping has no closed form at all — for
 * those, walk the tables.
 */
#define kernel_va_to_phys(va)	((uint64_t)(va) - KERNEL_IMAGE_BASE)
#define kernel_phys_to_va(pa)	((uint64_t)(pa) + KERNEL_IMAGE_BASE)

/*
 * Any physical address, through the direct map.  This is the translation
 * that replaces the i386 machinery for reaching physical memory: HIGHMEM
 * (#70), the pmap self-map (#333), the user page-table alias (#334) and the
 * temporary mappings in the fault path all exist because reaching a
 * physical page needed a mapping first.  Here it is an addition.
 *
 * Only valid once direct_map_init() has run, and only for physical
 * addresses it actually covers.
 */
#define phys_to_direct(pa)	((uint64_t)(pa) + DIRECT_MAP_BASE)
#define direct_to_phys(va)	((uint64_t)(va) - DIRECT_MAP_BASE)

/*
 * Nail the arithmetic to the chapter at build time.  If an offset is ever
 * mistyped, the build stops here instead of the fault landing somewhere far
 * away with no obvious cause.
 */
_Static_assert(DIRECT_MAP_BASE   == 0xffff800000000000ULL, "direct map moved");
_Static_assert(KERNEL_HEAP_BASE  == 0xffffc00000000000ULL, "kernel heap moved");
_Static_assert(PERCPU_BASE       == 0xffffe00000000000ULL, "per-CPU area moved");
_Static_assert(CPU_ENTRY_BASE    == 0xfffff00000000000ULL, "CPU entry area moved");
_Static_assert(DEVICE_MAP_BASE   == 0xfffff80000000000ULL, "device map moved");
_Static_assert(KERNEL_IMAGE_BASE == 0xffffffff80000000ULL, "kernel image moved");

/* Every region base must itself be a canonical address. */
_Static_assert(va_canonical(KERNEL_HALF_BASE) == KERNEL_HALF_BASE,
	       "kernel half base is not canonical");
_Static_assert(va_canonical(KERNEL_IMAGE_BASE) == KERNEL_IMAGE_BASE,
	       "kernel image base is not canonical");

/* The image must be the last thing in the space, ending exactly at the top. */
_Static_assert(KERNEL_IMAGE_BASE + KERNEL_IMAGE_SIZE - 1 == 0xffffffffffffffffULL,
	       "kernel image does not end at the top of the address space");

#endif	/* _X86_64_LAYOUT_H_ */
