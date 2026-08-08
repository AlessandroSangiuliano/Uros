/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The syscall path (#411, MD contract 6/6).
 *
 * Every Mach trap and every POSIX call in this system passes through here,
 * so what this costs is a floor under everything above it.  That is the
 * reason the decisions below are decisions rather than conventions.
 *
 * ══ The register contract ═════════════════════════════════════════════
 *
 *   rax        the call number going in, its result coming out
 *   rdi rsi rdx r10 r8 r9      arguments one to six
 *
 *   rcx r11    consumed by the instruction itself — SYSCALL puts the
 *              return address in rcx and the flags in r11, so they cannot
 *              carry anything and are gone on return
 *
 *   rbx rbp r12 r13 r14 r15    preserved
 *   everything else            DESTROYED
 *
 * That last line is the choice, and it is not what Linux does.  Linux
 * preserves the argument registers, which obliges its entry path to save
 * six of them on the way in and restore them on the way out, every time,
 * for a compatibility contract thirty years old.  We control both sides and
 * have no such debt, so a syscall here behaves exactly like a call to a C
 * function: the caller assumes the call clobbers what a call clobbers.
 *
 * r10 rather than rcx for the fourth argument because the instruction takes
 * rcx away; this matches the register choice Linux made for the same
 * reason, which costs nothing and makes a musl port one substitution rather
 * than a rewrite.
 *
 * ══ What this costs us, and what it does not ══════════════════════════
 *
 * The registers a debugger needs in order to unwind — the callee-saved set,
 * and the stack and instruction pointers — are preserved, and preserved for
 * free: they are callee-saved in the C ABI, so the dispatcher's own compiled
 * code keeps them without the entry path storing anything.  Backtraces
 * across a syscall are unaffected.  So is signal delivery: the frame a
 * handler returns through resumes at the instruction after SYSCALL, where
 * the destroyed registers are already dead by this contract.
 *
 * What is genuinely lost is reading — or altering — a call's *arguments*
 * after it has begun.  That is what strace-shaped tooling wants, and what
 * ptrace-style syscall interception needs at the exit stop.  At the entry
 * stop they are still live; it is on the way out that they are gone.
 *
 * ══ How to get it back, when something needs it ═══════════════════════
 *
 * By saving the full register image on the way in when, and only when,
 * something is watching the thread — the shape Linux arrived at, and for
 * this exact tension.  The decisive property is that adding it later is
 * *not* an ABI change: userspace cannot tell whether the kernel kept a copy
 * of registers it was entitled to destroy.  So it can be added the day
 * there is a debugger that wants it, and not before.
 *
 * The reverse is the one-way door, which is why this contract is the one
 * chosen now: preserving first and stopping later would break every
 * userland wrapper that had come to rely on it.
 *
 * The place to put it is marked in the entry path.
 */

#ifndef _X86_64_SYSCALL_SYSCALL_H_
#define _X86_64_SYSCALL_SYSCALL_H_

/*
 * The call numbers that exist.
 *
 * One, so far, and it is not a placeholder for the others: it is the call
 * the boot uses to prove the path, in the same way trap_probe_ud() is the
 * instruction the boot uses to prove a vector.  The real numbering arrives
 * with the machine-independent tree, which is also what #411 waits on before
 * it can be closed.
 */
#define SYSCALL_NR_PROBE	0
#define SYSCALL_NR_MAX		1

/*
 * ── Where the Mach traps live in the number space (#411) ──────────────
 *
 * i386 puts them at NEGATIVE call numbers: `%eax' negative means a Mach trap
 * and its index is the negation.  That is not carried over, and the reason is
 * not taste.
 *
 * The entry path's bound check is one unsigned comparison, and the comment
 * beside it says why: "a number above the table and a number that looked
 * negative are the same test".  A sign convention takes that away — the entry
 * would have to test the sign, negate, and then bound-check, and the register
 * holding the result is the first value in the kernel that a user chose.  One
 * comparison that cannot be got wrong is worth more than a range of numbers
 * nobody was using.
 *
 * So the split is by RANGE, on an unsigned number, and each half keeps its own
 * single comparison:
 *
 *	0x000 .. SYSCALL_NR_MAX-1	this kernel's own calls
 *	0x100 + n			Mach trap n
 *
 * The base is far enough above the first range that the two can grow for a
 * long time without meeting, and low enough to be read as a number rather than
 * decoded.  It is not a bit test: a bit would make trap 0 and call 0 differ by
 * something invisible in a register dump.
 */
#define SYSCALL_MACH_BASE	0x100

/*
 * How many Mach traps the entry will dispatch, and it is a compile-time bound
 * on purpose.
 *
 * The machine-independent mach_trap_table[] is sized by its own contents and
 * counted at run time.  The entry cannot index it directly -- its elements are
 * a structure whose layout would then be written as constants in assembly,
 * which is the #448 class exactly: two halves that agree until one of them
 * gains a field.  So syscall_init() copies the function pointers into a plain
 * array of pointers, and the assembly indexes THAT, knowing only that its
 * elements are eight bytes.
 */
/*
 * 256, and the number is measured: mach_trap_table[] has 178 entries on this
 * build.  The first value here was 128, chosen as a round number, and the
 * kernel refused to boot saying "178 Mach traps, the entry table holds 128" --
 * which is the check working, and is why it refuses rather than truncating.
 * Dispatching the first 128 and answering ENOSYS for the rest would be a
 * kernel where some traps work and some do not, with a boundary that moves
 * whenever the table does.
 */
#define SYSCALL_MACH_MAX	256

/*
 * What a call number the table does not have answers with.
 *
 * A number, because there is nothing else to answer with: the caller gets
 * one register back and cannot be told out of band.  The negative-errno
 * convention is what makes it distinguishable — a successful call returning
 * -38 would be indistinguishable from this, so the contract is that call
 * numbers which can fail return small negative values and nothing else does.
 *
 * 38 is ENOSYS wherever anyone has had to pick a number for this, and there
 * is no reason to be different for the sake of it.
 */
#define SYSCALL_ENOSYS		(-38)

#ifndef __ASSEMBLER__

#include <stdint.h>

/*
 * Point this processor's SYSCALL machinery at the entry path.
 *
 * Per processor, not once: LSTAR, STAR, FMASK and the enable bit in EFER
 * are all per-processor registers, and an application processor comes out
 * of a startup interrupt with none of them set.  A processor that misses
 * this does not fail a syscall — it takes an invalid-opcode fault, because
 * without the enable bit the instruction does not exist.
 */
void syscall_init(void);

/*
 * The call the boot uses to prove the path.
 *
 * It answers with its six arguments packed one byte each, lowest argument
 * in the lowest byte.  A sum would have been shorter and would have hidden
 * the failure worth catching: two arguments arriving in each other's
 * registers add to the same total, and the entry path's whole job is to put
 * six registers in the right places.
 */
uint64_t syscall_probe(uint64_t a1, uint64_t a2, uint64_t a3,
		       uint64_t a4, uint64_t a5, uint64_t a6);

/*
 * Which per-CPU block the last such call was reached with — the entry
 * path's swapgs, made observable.  It leaves no other trace.
 */
uint64_t syscall_probe_gs(void);

/*
 * The first instruction of the syscall path, which is what LSTAR is pointed
 * at.  Declared here rather than in the one file that installs it, because
 * the swapgs window of #440 begins at this address and the test that arranges
 * it has to be able to name it.
 */
void syscall_entry(void);

/*
 * The Mach traps as plain pointers, and how many are usable (#411).  Read by
 * the entry path; see syscall.c for why this is a copy of mach_trap_table[]
 * rather than an indirection into it.
 */
extern void	*mach_syscall_table[SYSCALL_MACH_MAX];
extern uint64_t	 mach_syscall_count;

/*
 * What the last probe call found on the kernel stack, and which stack that was
 * (#409).  See syscall.c: for this entry these two words ARE the frame.
 */
uint64_t syscall_probe_saved_rip(void);
uint64_t syscall_probe_saved_flags(void);
uint64_t syscall_probe_kernel_rsp(void);

/*
 * And the backtrace, taken inside the call rather than handed out as a frame
 * pointer to walk afterwards: by then the kernel stack has been reused.
 */
unsigned syscall_probe_depth(void);
const char *syscall_probe_top(void);
int syscall_probe_reached_entry(void);

#endif	/* __ASSEMBLER__ */

#endif	/* _X86_64_SYSCALL_SYSCALL_H_ */
