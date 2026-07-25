/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Per-CPU data, reached through %gs (#409/#408).
 *
 * Long mode drops the segmentation i386 used for this and replaces it with
 * a base held in an MSR, so %gs:0 is simply "this CPU's block" with no
 * descriptor involved.  The region it lives in is the per-CPU area of
 * docs/uros_design.md ch.11 §11.2, which the layout has reserved since #407
 * and nothing has used until now.
 *
 * Shared ground between two contracts: the trap path needs it because
 * swapgs has to have something to swap to, and the context switch needs it
 * for everything that is "this CPU's" rather than "this thread's".  #409
 * lands it; #408 reviews it, as the issue asks.
 */

#ifndef _X86_64_CPU_PERCPU_H_
#define _X86_64_CPU_PERCPU_H_

#include <stdint.h>

struct percpu {
	/*
	 * First on purpose: %gs-relative addressing can reach a field, but
	 * getting the block's own address needs it stored somewhere, and
	 * offset zero makes that one load.
	 */
	struct percpu *self;

	uint32_t cpu_id;
	uint32_t reserved;
};

/*
 * Build this CPU's block and point %gs at it.
 *
 * ⚠️ Must run after the last load of %gs as a segment register — the GDT
 * setup does one — because that zeroes the base this then writes.  Ordering
 * the other way leaves %gs:0 reading address zero, which is mapped early in
 * boot and so fails quietly rather than faulting.
 */
void percpu_init(uint32_t cpu_id);

/* This CPU's block, via the pointer it keeps at offset zero. */
static inline struct percpu *percpu(void)
{
	struct percpu *p;

	__asm__ volatile("movq %%gs:0, %0" : "=r"(p));
	return p;
}

#endif	/* _X86_64_CPU_PERCPU_H_ */
