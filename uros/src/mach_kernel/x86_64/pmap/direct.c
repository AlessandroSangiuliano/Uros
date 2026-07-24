/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 direct map (#407, MD contract 2/6).
 *
 * Maps physical memory into the kernel half at a fixed offset, with the
 * largest page the CPU offers.  The size of the page is the whole point:
 * one 1 GiB entry covers what would otherwise be 262144 entries of 4 KiB,
 * so a handful of TLB entries span all of RAM and the kernel touching
 * physical memory stops evicting everything else.
 *
 * The region is uniform — writable, never executable, no per-page policy —
 * which is exactly what makes huge pages appropriate here and inappropriate
 * for the mappings the VM actually manages, where 4 KiB granularity buys
 * copy-on-write, per-section W^X and paging.
 */

#include <stdint.h>

#include <cpu/regs.h>
#include <pmap/direct.h>
#include <pmap/layout.h>
#include <pmap/pte.h>

/*
 * How much physical memory the boot-time map covers.  A fixed window for
 * now: sizing it to the machine means reading the multiboot memory map,
 * which is a separate concern and a later increment.  Four gigabytes is
 * more than enough to bring the VM up, and bounds the fallback path's
 * static tables to something reasonable.
 */
#define DIRECT_MAP_BOOT_GIB	4

uint64_t direct_map_page_size;
uint64_t direct_map_covered;

/*
 * Page tables live in the kernel image's .bss.  The 4 KiB alignment is not
 * decoration: the hardware takes the frame out of bits 51:12 and ignores
 * the rest, so a misaligned table is silently read from the wrong address.
 */
static pt_entry_t direct_pdpt[PTES_PER_TABLE]
	__attribute__((aligned(4096)));

/* Only used when the CPU has no 1 GiB pages; one PD per gigabyte. */
static pt_entry_t direct_pd[DIRECT_MAP_BOOT_GIB][PTES_PER_TABLE]
	__attribute__((aligned(4096)));

/* The direct map hangs off exactly one PML4 slot. */
_Static_assert(pml4_index(DIRECT_MAP_BASE) == 256,
	       "direct map is not where the layout says it is");

/*
 * The window has to fit under one PDPT, which spans 512 GiB.
 */
_Static_assert(DIRECT_MAP_BOOT_GIB <= PTES_PER_TABLE,
	       "boot window does not fit in one PDPT");

static void fill_1g(void)
{
	for (unsigned i = 0; i < DIRECT_MAP_BOOT_GIB; i++)
		direct_pdpt[i] = ((uint64_t)i * PAGE_SIZE_1G)
			       | INTEL_PTE_VALID | INTEL_PTE_WRITE
			       | INTEL_PTE_PS | INTEL_PTE_NX;

	direct_map_page_size = PAGE_SIZE_1G;
}

static void fill_2m(void)
{
	for (unsigned i = 0; i < DIRECT_MAP_BOOT_GIB; i++) {
		for (unsigned j = 0; j < PTES_PER_TABLE; j++)
			direct_pd[i][j] = ((uint64_t)i * PAGE_SIZE_1G
					   + (uint64_t)j * PAGE_SIZE_2M)
					| INTEL_PTE_VALID | INTEL_PTE_WRITE
					| INTEL_PTE_PS | INTEL_PTE_NX;

		direct_pdpt[i] = kernel_va_to_phys(&direct_pd[i][0])
			       | INTEL_PTE_VALID | INTEL_PTE_WRITE;
	}

	direct_map_page_size = PAGE_SIZE_2M;
}

void direct_map_init(void)
{
	pt_entry_t *pml4;

	/*
	 * Execute-disable is reserved until NXE is set — writing bit 63
	 * before this point would fault rather than protect anything.
	 */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);

	/*
	 * Start from a clean table rather than trusting .bss to be zero:
	 * nothing in this kernel has cleared it, and an accidental present
	 * bit here would send a walk somewhere arbitrary.
	 */
	for (unsigned i = 0; i < PTES_PER_TABLE; i++)
		direct_pdpt[i] = 0;

	if (cpu_has_1gb_pages())
		fill_1g();
	else
		fill_2m();

	direct_map_covered = (uint64_t)DIRECT_MAP_BOOT_GIB * PAGE_SIZE_1G;

	/*
	 * Reach the live PML4 the only way available before the direct map
	 * exists: CR3 gives its physical address, and boot.S left the low
	 * identity map in place, so that address is dereferenceable as is.
	 */
	pml4 = (pt_entry_t *)(uintptr_t)(read_cr3() & INTEL_PTE_PFN);

	/*
	 * NX on the PML4 entry makes the whole region non-executable no
	 * matter what sits below it — the leaves carry it too, but this is
	 * the one place it cannot be forgotten.
	 */
	pml4[pml4_index(DIRECT_MAP_BASE)] = kernel_va_to_phys(direct_pdpt)
					  | INTEL_PTE_VALID | INTEL_PTE_WRITE
					  | INTEL_PTE_NX;

	/*
	 * Reloading CR3 flushes the non-global TLB.  Strictly, a previously
	 * not-present entry cannot be cached, so this is belt and braces at
	 * a point in boot where cheap certainty beats a clever argument.
	 */
	write_cr3(read_cr3());
}
