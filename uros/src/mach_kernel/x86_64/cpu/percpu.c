/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Per-CPU data, reached through %gs (#409/#408).
 */

#include <stdint.h>

#include <cpu/percpu.h>
#include <cpu/regs.h>
#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>

void percpu_init(uint32_t cpu_id)
{
	uint64_t va = PERCPU_BASE + (uint64_t)cpu_id * PAGE_SIZE_4K;
	uint64_t frame = boot_frame_alloc();
	struct percpu *p;

	if (frame == 0)
		return;

	/*
	 * A page per CPU, at a fixed stride from the region's base, so a
	 * block's address is a calculation rather than a lookup — which
	 * matters at the points where the lookup would need the block it is
	 * trying to find.
	 */
	pmap_enter(pmap_kernel(), va, frame, VM_PROT_READ | VM_PROT_WRITE, 0);

	p = (struct percpu *)(uintptr_t)va;
	p->self = p;
	p->cpu_id = cpu_id;

	wrmsr(MSR_GS_BASE, va);

	/*
	 * The other half of the pair.  swapgs exchanges the two, so what sits
	 * here is what %gs will hold after the swap — the kernel's block while
	 * user code runs, so that a single instruction on kernel entry brings
	 * it back.  Both halves hold the same value now: with no ring 3 there
	 * is nothing to swap away from, and an entry that swapped would only
	 * find the same block.
	 */
	wrmsr(MSR_KERNEL_GS_BASE, va);
}
