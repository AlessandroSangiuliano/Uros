/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 pmap: the machine-dependent physical map (#407, MD contract 2/6).
 */

#include <stdint.h>

#include <cpu/regs.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>

/*
 * The kernel pmap is a single object, not allocated: it has to exist before
 * any allocator does, and it lives as long as the kernel.  Its tables are
 * the ones boot.S and the direct map already built.
 */
static struct pmap kernel_pmap_store;

void pmap_bootstrap(void)
{
	kernel_pmap_store.root_pa = read_cr3() & INTEL_PTE_PFN;
	kernel_pmap_store.ref_count = 1;
}

pmap_t pmap_kernel(void)
{
	return &kernel_pmap_store;
}
