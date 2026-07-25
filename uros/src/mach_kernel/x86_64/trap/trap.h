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

/*
 * Interrupt stack table slots, numbered as the gate field is: 1 to 7, with
 * zero meaning "whatever stack was current".
 *
 * These three get their own stack because they are the ones most likely to
 * fire when the current stack is the problem — a double fault often *is* a
 * stack that cannot be pushed to, and an NMI can arrive in the middle of
 * anything, including that.  Handling them on the broken stack is how a
 * fault becomes a reset with nothing on the wire.
 *
 * A machine check gets one for a different reason: it can arrive at any
 * point, including inside the handler for something else.
 */
#define IST_DOUBLE_FAULT	1
#define IST_NMI			2
#define IST_MACHINE_CHECK	3
#define IST_COUNT		3

struct tss64;

/*
 * Point the TSS at the stacks the vectors above will run on.  Must happen
 * before the TSS is loaded, and the TSS must be loaded before trap_init():
 * a gate naming an IST slot is a promise the CPU will keep by reading the
 * task register, and it has to have something to read.
 */
void trap_ist_setup(struct tss64 *tss);

/* Install the IDT on this CPU. */
void trap_init(void);

/* Entry from the assembly stubs. */
void trap_dispatch(struct trap_frame *frame);

/*
 * Arrange for one expected fault to be survived rather than reported and
 * halted on: if the next trap is `vector`, the handler rewrites the return
 * address to `resume_rip` and returns, so execution continues there instead
 * of at the instruction that faulted.
 *
 * The kernel already needs this shape of thing on i386, where copyin and
 * copyout recover from faults on user addresses rather than dying with them
 * (i386/trap.c). This is the same idea at its smallest: one armed
 * expectation, cleared as soon as it fires, with none of the address-range
 * table that the real mechanism will want.
 *
 * It is also what lets a deliberate fault be a test instead of the end of
 * the boot — a protection can only be shown to hold by provoking it.
 */
void trap_expect(uint64_t vector, uint64_t resume_rip);

/*
 * What the last expected trap turned out to be.  Reporting a fault is one
 * thing; being able to check that the frame carried the right vector, the
 * right error code and a plausible instruction pointer is what tells us the
 * entry path built it correctly — which for thirty-two hand-written stubs is
 * not something to assume.
 */
struct trap_record {
	uint64_t vector;
	uint64_t error;
	uint64_t rip;
	uint64_t cr2;
	int      caught;
};

const struct trap_record *trap_last(void);

/*
 * Store TRAP_PROBE_PATTERN at `addr` and report whether the machine allowed
 * it: 0 if the store completed, 1 if it faulted.  Checking for the pattern
 * afterwards distinguishes a store that happened from one that was never
 * needed, which a zero could not.  Arm trap_expect() with
 * trap_probe_faulted first — this is the store, not the arrangement to
 * survive it.
 *
 * The pair lives in trap/entry.S, where the resume point can be a symbol
 * rather than a label the compiler is entitled to optimise away.
 */
#define TRAP_PROBE_PATTERN	0x5a5a5a5a

int trap_probe_write(volatile void *addr);

/*
 * One probe per exception family, each returning 0 if the instruction
 * completed and 1 if the handler stepped over it.  Between them they cover
 * both sides of the error-code asymmetry the stubs exist to hide: the first
 * three arrive without one and must be given a zero, while a general
 * protection arrives with the offending selector and must keep it.
 */
int trap_probe_ud(void);	/* invalid opcode      */
int trap_probe_bp(void);	/* breakpoint          */
int trap_probe_de(void);	/* divide error        */
int trap_probe_gp(void);	/* general protection  */

extern char trap_probe_faulted[];

#endif	/* _X86_64_TRAP_TRAP_H_ */
