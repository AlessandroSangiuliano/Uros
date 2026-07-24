/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 direct map (#407, MD contract 2/6).
 *
 * All of physical memory reachable at a fixed offset, so that getting at a
 * physical page is an addition instead of a temporary mapping.  See
 * phys_to_direct() in <pmap/layout.h> for the translation itself.
 */

#ifndef _X86_64_PMAP_DIRECT_H_
#define _X86_64_PMAP_DIRECT_H_

#include <stdint.h>

/*
 * Build the direct map and install it, using the largest page the CPU
 * offers.  Enables EFER.NXE first, because the mapping is non-executable
 * and that bit is reserved until NXE is on.
 *
 * Bootstrap-only in one respect: it reaches the live PML4 through the low
 * identity map that boot.S left in place.  Once this has run, the pmap
 * reaches page tables through the direct map instead, which is the whole
 * point of having one.
 */
void direct_map_init(void);

/* Page size the mapping was built with: 1 GiB, or 2 MiB if the CPU has no
 * 1 GiB pages.  Zero before direct_map_init() runs. */
extern uint64_t direct_map_page_size;

/* How much physical memory the mapping currently covers. */
extern uint64_t direct_map_covered;

#endif	/* _X86_64_PMAP_DIRECT_H_ */
