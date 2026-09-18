/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The two arithmetic faults, provoked on purpose (#515), x86-64.
 *
 * The i386 file beside this one carries the reasoning; this is the same two
 * routines in the other calling convention.  They are two files rather than
 * one with a preprocessor fork because the stack frames differ in every line
 * that touches them, and a file of #ifdefs would hide which instructions are
 * actually assembled for the target being tested.
 */

	.text

/*
 * fperr_provoke_x87 -- an unmasked zero-divide on the x87 stack.
 *
 * 🔑 The fault arrives at the `fwait', not at the division: x87 reports a
 * numeric error at the next waiting instruction.  Returning from here means
 * the kernel cleared the reported condition in the thread's saved state; not
 * returning means it left it armed and the wait faulted again.
 *
 * 🔴 NOTHING MAY FOLLOW THE WAIT BUT THE RETURN: GAS folds `fwait' into a
 * following FN-form instruction, so `fwait; fninit' assembles to one `finit'
 * and the faulting address stops being the one named here.  See the i386 file
 * beside this one, where it happened.
 */
	.globl	fperr_provoke_x87
fperr_provoke_x87:
	pushq	%rbp
	movq	%rsp, %rbp
	subq	$16, %rsp

	fninit				/* control word = 0x037f, all masked */
	fnstcw	-2(%rbp)
	andw	$0xfffb, -2(%rbp)	/* clear ZM (bit 2): unmask zero-divide */
	fldcw	-2(%rbp)

	fld1				/* st(0) = 1.0              */
	fldz				/* st(0) = 0.0, st(1) = 1.0 */
	fdivp	%st, %st(1)		/* st(0) = 1.0/0.0 -> ZE, no trap yet */

	fwait				/* <-- the fault is taken HERE */

	leave
	ret

/*
 * fperr_provoke_sse -- an unmasked zero-divide in the SIMD unit.
 *
 * #XF is precise, so the faulting address is the `divsd' and a thread resumed
 * at it divides by zero again.  This routine is not expected to return.
 */
	.globl	fperr_provoke_sse
fperr_provoke_sse:
	pushq	%rbp
	movq	%rsp, %rbp
	subq	$16, %rsp

	stmxcsr	-8(%rbp)
	andl	$0xfffffdff, -8(%rbp)	/* clear ZM (bit 9): unmask zero-divide */
	ldmxcsr	-8(%rbp)

	movabsq	$0x3ff0000000000000, %rax	/* 1.0 as a double */
	movq	%rax, %xmm0
	xorpd	%xmm1, %xmm1			/* 0.0 */
	divsd	%xmm1, %xmm0			/* <-- the fault is taken HERE */

	leave
	ret
