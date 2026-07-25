/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 trap and interrupt entry (#409, MD contract 4/6).
 *
 * Until this exists, every fault is a triple fault: the CPU tries to report
 * the problem, finds no descriptor to report it through, fails to report
 * that, and resets.  What reaches the serial line is silence, and the last
 * banner printed is the only clue.  An IDT turns the same event into a
 * vector, a faulting address and an instruction pointer.
 */

#ifndef _X86_64_TRAP_TRAP_H_
#define _X86_64_TRAP_TRAP_H_

#include <stdint.h>

/*
 * The architectural exception vectors.  Named because a number on a serial
 * line is a lookup, and the lookup happens at the worst possible moment.
 */
#define T_DIVIDE_ERROR		0
#define T_DEBUG			1
#define T_NMI			2
#define T_BREAKPOINT		3
#define T_OVERFLOW		4
#define T_BOUND_RANGE		5
#define T_INVALID_OPCODE	6
#define T_NO_FPU		7
#define T_DOUBLE_FAULT		8
#define T_FPU_OVERRUN		9
#define T_INVALID_TSS		10
#define T_SEGMENT_NOT_PRESENT	11
#define T_STACK_FAULT		12
#define T_GENERAL_PROTECTION	13
#define T_PAGE_FAULT		14
#define T_FPU_ERROR		16
#define T_ALIGNMENT_CHECK	17
#define T_MACHINE_CHECK		18
#define T_SIMD_ERROR		19

#define T_VECTORS		32	/* the architecture reserves 0..31 */

/*
 * The page-fault error code, which says what was attempted rather than what
 * is wrong — the distinction that separates "not mapped" from "mapped and
 * refused", and so a missing page from a protection violation.
 */
#define PF_PRESENT		0x01	/* clear: nothing mapped there   */
#define PF_WRITE		0x02	/* set: it was a write           */
#define PF_USER			0x04	/* set: from ring 3              */
#define PF_RESERVED		0x08	/* a reserved bit was set        */
#define PF_INSTRUCTION		0x10	/* an instruction fetch          */

/*
 * What the handler is handed.  The order is the order the registers reach
 * the stack in trap/entry.S, which is why the two must be read together:
 * the CPU pushes the last five, the stub supplies the vector and — for the
 * exceptions that do not have one — a zero in place of the error code, so
 * that every vector arrives in the same shape.
 */
struct trap_frame {
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
	uint64_t vector;
	uint64_t error;
	uint64_t rip;
	uint64_t cs;
	uint64_t rflags;
	uint64_t rsp;
	uint64_t ss;
};

/* Install the IDT on this CPU. */
void trap_init(void);

/* Entry from the assembly stubs. */
void trap_dispatch(struct trap_frame *frame);

#endif	/* _X86_64_TRAP_TRAP_H_ */
