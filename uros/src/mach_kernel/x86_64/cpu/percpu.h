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

/*
 * The offsets the entry path needs, before anything C.  Assembly cannot ask
 * the compiler where a field is, so the numbers live here and the assertions
 * below keep them honest.
 */
#define PERCPU_SELF		0
#define PERCPU_CPU_ID		8
#define PERCPU_KERNEL_RSP	16
#define PERCPU_USER_RSP		24

#ifndef __ASSEMBLER__

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

	/*
	 * The two words the syscall entry touches, kept together and kept
	 * near the front.
	 *
	 * SYSCALL does not switch the stack — that is the kernel's job, and
	 * it is the first thing the entry does, before it has anywhere to put
	 * anything.  So the stack to switch *to* and the place to park the
	 * one being switched *from* both have to be reachable through %gs
	 * alone, and they are read and written within a few instructions of
	 * each other on the hottest path in the system.  Adjacent means one
	 * cache line pays for both.
	 */
	uint64_t kernel_rsp;		/* what a syscall switches to */
	uint64_t user_rsp;		/* where the user's waits meanwhile */

	/*
	 * The interrupt priority level, and what arrived while it was raised
	 * (#409/#322).
	 *
	 * Here rather than in a static array indexed by processor, because the
	 * whole point of the software level is that reading and writing it
	 * costs one %gs-relative instruction — an array would cost the index
	 * first, and the index is the thing %gs already is.
	 *
	 * One bit per vector, so deferral is exact: a class-wide flag would
	 * replay every vector in a class because one of them arrived.
	 */
	uint32_t ipl;
	uint32_t reserved_ipl;
	uint64_t pending[4];
	uint64_t deferred;
	uint64_t replayed;
};

/*
 * Checked against the structure rather than trusted: a field added in the
 * middle would otherwise move the offsets above silently, and the first
 * symptom would be a syscall running on a stack that is not a stack.
 */
_Static_assert(__builtin_offsetof(struct percpu, self) == PERCPU_SELF,
	       "percpu self moved");
_Static_assert(__builtin_offsetof(struct percpu, cpu_id) == PERCPU_CPU_ID,
	       "percpu cpu_id moved");
_Static_assert(__builtin_offsetof(struct percpu, kernel_rsp) == PERCPU_KERNEL_RSP,
	       "percpu kernel_rsp moved");
_Static_assert(__builtin_offsetof(struct percpu, user_rsp) == PERCPU_USER_RSP,
	       "percpu user_rsp moved");

/*
 * Two halves, and the split is not tidiness.
 *
 * percpu_alloc() maps a processor's page, which means editing the kernel's
 * page tables.  percpu_activate() fills the block and points %gs at it,
 * which touches only that processor's own page and its own MSR.
 *
 * If a processor did both for itself on the way in, every one of them would
 * be editing the same page tables at the same moment — two arriving
 * together could both find an intermediate table missing and both build
 * one.  So the boot processor allocates for everybody before waking anyone,
 * and an arriving processor only activates.  Prepare, then release.
 *
 * ⚠️ percpu_activate() must run after the last load of %gs as a segment
 * register — the GDT setup does one — because that zeroes the base it
 * writes.  Ordered the other way, %gs:0 reads address zero, which early in
 * boot is mapped, so it fails quietly rather than faulting.
 */
void percpu_alloc(uint32_t cpu_id);
void percpu_activate(uint32_t cpu_id);

/* This CPU's block, via the pointer it keeps at offset zero. */
static inline struct percpu *percpu(void)
{
	struct percpu *p;

	__asm__ volatile("movq %%gs:0, %0" : "=r"(p));
	return p;
}

#endif	/* __ASSEMBLER__ */

#endif	/* _X86_64_CPU_PERCPU_H_ */
