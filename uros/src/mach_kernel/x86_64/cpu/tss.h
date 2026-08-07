/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The x86-64 task state segment.
 *
 * Almost nothing of the i386 TSS survives: no task switching, no register
 * save area.  What is left is a table of stacks, and that is the whole
 * point of it here — the stack a privilege transition lands on (RSP0), and
 * seven that a chosen interrupt vector can be made to land on regardless of
 * what the interrupted code was using (the interrupt stack table).
 *
 * It is shared plumbing: the descriptor-table setup builds and loads it,
 * and the trap layer decides which of its stacks belongs to which vector.
 * Per-CPU it must eventually be — the CPU entry area of ch.11 §11.2 exists
 * for that — but there is one CPU so far.
 */

#ifndef _X86_64_CPU_TSS_H_
#define _X86_64_CPU_TSS_H_

#include <stdint.h>

struct tss64 {
	uint32_t reserved0;
	uint64_t rsp0;			/* ring 3 -> ring 0 lands here */
	uint64_t rsp1;
	uint64_t rsp2;
	uint64_t reserved1;
	uint64_t ist[7];		/* IST1..IST7, indexed from zero here */
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t iomap_base;
} __attribute__((packed));

#endif	/* _X86_64_CPU_TSS_H_ */
