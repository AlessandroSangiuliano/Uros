/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * act_test, arm twenty-six (#771): a Mach trap called the way every program
 * calls one, with a pattern of this file's own in each of the six registers
 * the C ABI and <syscall/syscall.h> preserve -- and what each of them holds
 * when the call comes back.
 *
 * In assembly because C cannot say it.  The compiler owns rbx, rbp and
 * r12-r15, and keeps what it put there across a call by whatever means it
 * likes, so a C function cannot promise that a value is IN r14 while the call
 * runs.  Here the six are loaded, the call is made, and the six are read, with
 * nothing of the compiler's in between.
 *
 * The patterns are arm26_patterns[], and the C side compares against those
 * words rather than a second copy of them.
 */

	.section .rodata
	.align	8
	.globl	arm26_patterns
arm26_patterns:
	.quad	0x2601b1b1b1b1b1b1	/* rbx */
	.quad	0x2602b9b9b9b9b9b9	/* rbp */
	.quad	0x2603c3c3c3c3c3c3	/* r12 */
	.quad	0x2604d4d4d4d4d4d4	/* r13 */
	.quad	0x2605e5e5e5e5e5e5	/* r14 */
	.quad	0x2606f6f6f6f6f6f6	/* r15 */

	.text

/*
 * mach_msg_return_t
 * arm26_trap_with_patterns(const unsigned long args[9], unsigned long got[6])
 *
 *   args	mach_msg_overwrite_trap()'s nine arguments, in order
 *   got	rbx, rbp, r12, r13, r14, r15 as the call gave them back
 *
 * Through libmach's stub and not a bare SYSCALL, because the stub is half of
 * what a program gets: it keeps rbx, r12 and r13 itself, around the three
 * arguments it carries in them, and the kernel keeps the other three.
 */
	.globl	arm26_trap_with_patterns
arm26_trap_with_patterns:
	/* Ours to give back to our own caller, whatever the call does. */
	pushq	%rbx
	pushq	%rbp
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15

	/*
	 * Six pushes on top of the return address leave %rsp eight past a
	 * sixteen-byte boundary.  Five words more put it on one for the call:
	 * arguments seven to nine where the C ABI wants them, a word of
	 * padding, and `got', kept for after the call.
	 */
	subq	$40, %rsp
	movq	%rsi, 32(%rsp)		/* got                      */
	movq	48(%rdi), %rax
	movq	%rax, 0(%rsp)		/* seven: notify            */
	movq	56(%rdi), %rax
	movq	%rax, 8(%rsp)		/* eight: rcv_msg           */
	movq	64(%rdi), %rax
	movq	%rax, 16(%rsp)		/* nine: rcv_limit          */

	movq	8(%rdi), %rsi		/* two: option              */
	movq	16(%rdi), %rdx		/* three: send_size         */
	movq	24(%rdi), %rcx		/* four: rcv_size           */
	movq	32(%rdi), %r8		/* five: rcv_name           */
	movq	40(%rdi), %r9		/* six: timeout             */
	movq	0(%rdi), %rdi		/* one: msg -- last, rdi held the array */

	movq	arm26_patterns+0(%rip), %rbx
	movq	arm26_patterns+8(%rip), %rbp
	movq	arm26_patterns+16(%rip), %r12
	movq	arm26_patterns+24(%rip), %r13
	movq	arm26_patterns+32(%rip), %r14
	movq	arm26_patterns+40(%rip), %r15

	call	mach_msg_overwrite_trap

	movq	32(%rsp), %rcx		/* got: rcx is the call's to destroy */
	movq	%rbx, 0(%rcx)
	movq	%rbp, 8(%rcx)
	movq	%r12, 16(%rcx)
	movq	%r13, 24(%rcx)
	movq	%r14, 32(%rcx)
	movq	%r15, 40(%rcx)

	addq	$40, %rsp
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbp
	popq	%rbx
	ret				/* %eax: the call's answer */
