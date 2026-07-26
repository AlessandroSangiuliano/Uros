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
 * And the rest of the table, which the architecture leaves to us.
 *
 * Nothing has arrived through one of these yet — interrupts have been off
 * since the first instruction — but they are where everything that is not a
 * fault comes in: device interrupts, and before those, the messages one
 * processor sends another.  A processor cannot be interrupted at a vector
 * that has no gate, so the table has to reach this far before any of it can
 * be tried.
 *
 * The assignments below are the whole plan for the space:
 *
 *   0x00..0x1F  the architectural exceptions
 *   0x20..0x2F  the legacy interrupt requests, once they are remapped (#409)
 *   0xF0..0xFE  processor-to-processor messages
 *   0xFF        the local APIC's spurious vector
 *
 * The high end for the messages on purpose: the interrupt controller
 * prioritises by vector number, so a message that another processor is
 * waiting on outranks a device that is not.
 */
#define IDT_VECTORS		256
#define T_EXTERNAL_FIRST	T_VECTORS

/*
 * One vector reserved for exercising the entry path by hand.  Two hundred
 * and twenty-four stubs were written by a macro and none of them has ever
 * run; a software interrupt through one is the cheapest way to find out
 * whether the macro was right before an asynchronous interrupt arrives and
 * the answer is a reset.
 */
#define T_PROBE_VECTOR		0xE0

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
 * The first three get their own stack because they are the ones most likely
 * to fire when the current stack is the problem — a double fault often *is* a
 * stack that cannot be pushed to, and an NMI can arrive in the middle of
 * anything, including that.  Handling them on the broken stack is how a
 * fault becomes a reset with nothing on the wire.
 *
 * A machine check gets one for a different reason: it can arrive at any
 * point, including inside the handler for something else.
 *
 * ── And the debug exception, which is here because of #440 ────────────
 *
 * A trap that arrives at ring 0 does *not* switch stacks: the processor
 * changes stacks on a privilege transition, or when the gate names a slot in
 * this table, and neither applies.  Everywhere else that is exactly right,
 * because a kernel trapping at ring 0 was already standing on a kernel
 * stack.
 *
 * Not in the syscall window.  SYSCALL does not switch the stack either, so
 * for the few instructions before the entry loads one, the processor is at
 * ring 0 with %rsp still holding whatever a user program left in it — and a
 * user program may leave any value at all in %rsp, including the address of
 * something the kernel cares about.  A debug exception delivered there would
 * push its frame at that address, at ring 0, through the kernel's own
 * mapping.  That is not a leak of information; it is a write.
 *
 * Which is why the three vectors that could already arrive in that window
 * had stacks and this one has to as well.  It is the same list as the one in
 * entry.S, for the same reason, and the two must not drift apart: the vector
 * that decides %gs from the base is the vector that cannot trust %rsp
 * either.
 *
 * ⚠️ An IST slot is a fixed address, so a second exception on the same slot
 * lands on top of the first one's frame.  Nothing here arms a breakpoint
 * from inside a handler, so it cannot nest today — the day something does,
 * it inherits this.
 */
#define IST_DOUBLE_FAULT	1
#define IST_NMI			2
#define IST_MACHINE_CHECK	3
#define IST_DEBUG		4
#define IST_COUNT		4

struct tss64;

/*
 * Point a TSS at the stacks its vectors will run on: IST_COUNT consecutive
 * stacks of `stack_size` bytes, the lowest starting at `block`.
 *
 * The caller provides the memory and this decides which stack belongs to
 * which slot, because that is the part that follows from the table above and
 * would otherwise have to be duplicated by every caller.  Whose memory it is
 * matters: two processors sharing one of these would be writing over each
 * other's account of the fault that brought them here.
 *
 * Must happen before the TSS is loaded, and the TSS must be loaded before
 * trap_init(): a gate naming an IST slot is a promise the CPU will keep by
 * reading the task register, and it has to have something to read.
 */
void trap_ist_setup(struct tss64 *tss, uint64_t block, uint64_t stack_size);

/*
 * Stop, and say why.
 *
 * For the conditions where carrying on would mean the caller believing
 * something that is not true — a per-CPU block that was never built, a
 * mapping that went unrecorded, an index that silently tracks nothing.
 * Returning quietly from those leaves a kernel that looks like it works and
 * is wrong somewhere else, later, for reasons that no longer point back
 * here.
 *
 * This is the bootstrap answer, not the final one: once there is a caller
 * that can do something about a failure, these become returned errors. What
 * they must not be in the meantime is invisible.
 */
void panic(const char *what) __attribute__((noreturn));

/* Install the IDT on this CPU. */
void trap_init(void);

/* Entry from the assembly stubs. */
void trap_dispatch(struct trap_frame *frame);

/*
 * Entry from the four stubs that cannot decide from the saved code segment
 * (#440): #DB, NMI, #DF and #MC.  See trap/entry.S for why those four and no
 * others, and for how the decision is made.
 *
 * The two extra arguments are the evidence, passed along rather than
 * recomputed: `gs_on_entry` is the base the vector arrived with, before the
 * entry did anything about it, and `swapped` is what the entry decided.
 * Neither can be recovered afterwards — by the time C runs the base is the
 * kernel's whichever way the decision went, which is precisely the property
 * that makes it correct and also the reason it has to be carried.
 */
void trap_dispatch_paranoid(struct trap_frame *frame, uint64_t gs_on_entry,
			    uint64_t swapped);

/*
 * What a vector is called.  Exported because a number on a serial line is a
 * lookup performed at the worst possible moment, and the tests report faults
 * they provoked on purpose as often as the handler reports the others.
 */
const char *trap_name(uint64_t vector);

/*
 * Claim one of the vectors above T_EXTERNAL_FIRST.
 *
 * Only those: an exception vector already means something the kernel has to
 * decide about, and letting it be overwritten would turn a diagnosis into
 * whatever the last caller installed.
 *
 * The table is written once, by the boot processor, before any processor is
 * in a position to take an interrupt — so it is shared without a lock and
 * must stay that way, which is what the check in trap_set_handler() is for.
 */
typedef void (*trap_handler_t)(struct trap_frame *frame);

void trap_set_handler(unsigned vector, trap_handler_t handler);

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
 * Resume at the instruction the trap arrived on, rather than somewhere else.
 *
 * For the faults trap_expect() was written for, resuming where they happened
 * would repeat them forever, so a different address is the only useful
 * answer.  For the ones that are not the instruction's fault — a debug
 * exception armed from outside, an interrupt that is not maskable — the
 * instruction has not run yet or has run correctly, and continuing is what
 * the handler is supposed to do.  Zero is not an address a resume could
 * legitimately be, which is what lets it mean this.
 */
#define TRAP_RESUME_HERE	0UL

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
	uint64_t cs;		/* its low two bits are the ring it came from */
	uint64_t gs_base;	/* which block %gs reached the handler with  */
	int      caught;
};

const struct trap_record *trap_last(void);

/*
 * What the last vector that took the paranoid entry found there (#440).
 *
 * A record of its own rather than two more fields on the one above, because
 * only one of the two paths can fill them in.  A field that is meaningful
 * after some traps and stale after the others is worse than no field: it
 * reads the same either way, and the reader has no way to ask which it got.
 *
 * `cs` is here because it is the evidence *against* the rule this path
 * replaces — a window trap carries a ring-0 code segment, so recording it
 * alongside a user base is what makes the two disagree in the record rather
 * than only in the argument.
 */
struct trap_paranoid_record {
	uint64_t vector;
	uint64_t rip;			/* and where in the window it landed  */
	uint64_t cs;			/* what the ring rule would have read */
	uint64_t gs_on_entry;		/* the base the window left loaded    */
	uint64_t gs_on_dispatch;	/* and the one the handler ran with   */
	uint64_t swapped;		/* whether the entry exchanged them   */
	int      taken;
};

const struct trap_paranoid_record *trap_last_paranoid(void);

/* Forget the last one, so that "it happened" can be distinguished from "it
 * happened earlier".  A test that only checks the contents of this record
 * would pass on a stale one from a previous experiment. */
void trap_paranoid_forget(void);

/*
 * Arrange for the next fault *from ring 3* to return to the kernel instead
 * of being reported and halted on.
 *
 * A separate arrangement from trap_expect(), because the recovery is a
 * different operation.  That one rewrites the instruction pointer and lets
 * the return continue on the same stack at the same privilege; there is no
 * such stack here.  A fault from ring 3 arrives on the task-state segment's
 * stack, and the frame the processor pushed describes a user context in
 * every field — code segment, stack segment, stack pointer, flags.  Coming
 * back means rewriting all of them, which is the same thing the real user
 * fault path will do once there is a thread to return *to* rather than a
 * saved kernel frame.
 *
 * Any vector, deliberately: what makes a fault recoverable here is where it
 * came from, not what it was.  A kernel that only expected the one it meant
 * to provoke would report the others from a context it cannot resume.
 */
void trap_expect_user(uint64_t resume_rip, uint64_t resume_rsp);

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
