/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The syscall entry path (#411, MD contract 6/6).
 *
 * The register contract this implements is written down in
 * <syscall/syscall.h> and is the thing to read first; what follows is how
 * few instructions it takes to honour it.
 *
 * Every Mach trap and every POSIX call in the system passes through these
 * instructions, so each one of them is paid for by everything above.
 */

#include <cpu/desc.h>
#include <cpu/percpu.h>
#include <syscall/syscall.h>

	.section .text
	.code64

/*
 * Entry from ring 3.  The processor has already done, in the instruction
 * itself and without touching memory:
 *
 *   rcx = the return address        r11 = the caller's flags
 *   rflags &= ~FMASK                cs, ss = from STAR
 *
 * and nothing else.  In particular it has *not* switched the stack — unlike
 * a trap gate, which takes one from the task-state segment.  So the stack
 * pointer still belongs to a user program, and everything below has to
 * happen before anything is pushed.
 *
 * That absence is a saving, not a burden: it is precisely the per-switch
 * write of IA32_SYSENTER_ESP that #348 built a per-CPU trampoline on i386
 * to avoid, measured there at about 144 cycles. There is no such register
 * here and nothing to keep up to date, so that machinery is not ported —
 * the cost it removed does not exist on this architecture.
 *
 * Aligned to a cache line because the first instruction of a syscall is
 * fetched cold more often than almost anything else in the kernel.
 */
	.globl	syscall_entry
	.align	64
syscall_entry:
	/*
	 * %gs becomes the kernel's.  Until this instruction retires there is
	 * no way to reach per-CPU state at all, which is why it is first and
	 * why nothing above it may fault.
	 */
	swapgs

	movq	%rsp, %gs:PERCPU_USER_RSP
	movq	%gs:PERCPU_KERNEL_RSP, %rsp

	/*
	 * The two words the return needs, and no others.  The contract says
	 * the argument registers are destroyed, so they are not saved — that
	 * is six stores and six loads that this path does not perform, on
	 * every call in the system.
	 *
	 * ── Where the full register image would go ────────────────────────
	 *
	 * If something ever needs to see a thread's registers at a syscall
	 * boundary — a debugger, ptrace-style interception — this is the
	 * place: push the rest here and pop them before the return below.
	 * Doing it conditionally, on a flag in the block %gs already points
	 * at, keeps it off this path when nobody is watching. It is not an
	 * ABI change, which is the reason the contract could be chosen
	 * without deciding this now.
	 */
	pushq	%rcx			/* where ring 3 resumes */
	pushq	%r11			/* the flags it resumes with */

	/*
	 * The fourth argument moves into the register the C ABI expects.  The
	 * instruction took rcx for the return address, which is why the
	 * caller was asked for r10 instead — one move, here, rather than a
	 * different convention for every call.
	 */
	movq	%r10, %rcx

	/*
	 * An index from ring 3 into a table of function pointers, checked.
	 * Unsigned, so one comparison covers both ends: a number above the
	 * table and a number that looked negative are the same test.
	 *
	 * This is the boundary. Everything before it was arranged by the
	 * processor; %rax is the first value in the kernel that a user chose.
	 */
	/*
	 * Mach traps live above SYSCALL_MACH_BASE (#411).  Taken first because
	 * it is the branch that will carry the traffic: every message in the
	 * system comes through here, and the kernel's own calls are a handful.
	 *
	 * ⚠️ The subtraction happens AFTER the range test, not before.  Doing it
	 * first and testing the result would be arithmetic on a number a user
	 * chose, and a number below the base would wrap to an enormous one --
	 * still caught by the unsigned bound below, but caught by accident
	 * rather than by the test that is written.
	 */
	cmpq	$SYSCALL_MACH_BASE, %rax
	jb	.Lown_call

	subq	$SYSCALL_MACH_BASE, %rax
	cmpq	mach_syscall_count(%rip), %rax
	jae	.Lbad_call

	leaq	mach_syscall_table(%rip), %r11
	callq	*(%r11, %rax, 8)
	jmp	.Lreturn

.Lown_call:
	cmpq	$SYSCALL_NR_MAX, %rax
	jae	.Lbad_call

	leaq	syscall_table(%rip), %r11
	callq	*(%r11, %rax, 8)

.Lreturn:
	popq	%r11			/* flags */
	popq	%rcx			/* resume address */

	/*
	 * SYSRET takes the return address from rcx, and on Intel parts it
	 * faults if that address is not canonical — after switching to the
	 * user's stack pointer and before leaving ring 0.  A general
	 * protection fault at kernel privilege, on a stack the caller chose,
	 * with an address the caller chose: that is CVE-2012-0217, and it is
	 * a privilege escalation rather than a crash.
	 *
	 * The address is checked instead, which is three instructions and a
	 * branch never taken. Canonical here means bit 47 sign-extended,
	 * because the tables are four levels deep; five-level paging moves
	 * the bit to 56 and this shift with it (#405 keeps that door open).
	 *
	 * r10 is scratch: the contract already declares it destroyed.
	 */
	movq	%rcx, %r10
	shlq	$16, %r10
	sarq	$16, %r10
	cmpq	%rcx, %r10
	jne	.Lreturn_the_slow_way

	movq	%gs:PERCPU_USER_RSP, %rsp
	swapgs
	sysretq

/*
 * A call number the table does not have.  Answering with a value would be
 * answering, and a caller cannot tell -1 from a syscall that returned -1 —
 * so this is the one case where the number itself is the answer, and the
 * convention has to be stated somewhere it cannot be missed.
 *
 * It is stated in <syscall/syscall.h> with the rest of the contract, and it
 * is the same value POSIX has always used for this.
 */
.Lbad_call:
	movq	$SYSCALL_ENOSYS, %rax	/* and nothing that looks plausible */
	jmp	.Lreturn

/*
 * The return that cannot use SYSRET.
 *
 * IRETQ costs several times what SYSRET does, which is exactly why it is
 * here and not above: it is correct for every case, and reaching for it
 * always would make every syscall in the system pay for a case that only
 * arises when a user program has already done something it had no reason
 * to do.
 *
 * The frame is built by hand from what the entry saved, in the order IRETQ
 * pops it.
 */
.Lreturn_the_slow_way:
	movq	%gs:PERCPU_USER_RSP, %r10
	swapgs

	pushq	$USER_DS_RPL3		/* ss     */
	pushq	%r10			/* rsp    */
	pushq	%r11			/* rflags */
	pushq	$USER_CS_RPL3		/* cs     */
	pushq	%rcx			/* rip    */
	iretq
