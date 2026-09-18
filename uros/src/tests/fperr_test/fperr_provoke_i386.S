/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The two arithmetic faults, provoked on purpose (#515), i386.
 *
 * Assembly and not C, for two reasons that are both about control.  A compiler
 * is free to fold 1.0/0.0 at compile time, to reorder the division away from
 * the instruction that is supposed to report it, or to reach for SSE where
 * this file wants x87 -- and every one of those would turn a test of the
 * kernel into a test of the optimiser.  And the x87 arm needs the reporting
 * instruction to be a NAMED one, which C has no way to say.
 */

	.text

/*
 * fperr_provoke_x87 -- an unmasked zero-divide on the x87 stack.
 *
 * 🔑 The fault does NOT arrive at the division.  x87 reports a numeric error
 * at the next WAITING floating-point instruction, so the divide sets ZE in the
 * status word and carries on, and the `fwait' below is where #MF (or, on a
 * kernel with CR0.NE clear, where the IRQ that stands in for it) is taken.
 * The wait is written out rather than left to whatever instruction happened to
 * come next, so the faulting address is a known one.
 *
 * ⚠️ Which also makes this the arm that asks whether the kernel CLEARED the
 * condition.  A handler that answers KERN_SUCCESS sends the thread back to the
 * `fwait'.  If the reported exception flag is still set in the thread's saved
 * state, that wait faults again, and again, and this routine never returns --
 * which is the whole of the check, and needs nothing but the caller noticing
 * that it did not come back.
 *
 * 🔴 NOTHING MAY FOLLOW THE WAIT BUT THE RETURN, and it is not style.  GAS
 * folds `fwait' into a following FN-form instruction and emits the waiting
 * form instead -- this routine ended in `fwait; fninit' and assembled to
 * `9b db e3', a single `finit'.  The fault still arrived, at an address that
 * was no longer the one named here, which is an instrument quietly describing
 * something other than what it does.  `leave' and `ret' cannot be folded into,
 * so ending here makes that impossible rather than remembered.
 *
 * ⚠️ So the unit is left as the fault found it: an unmasked exception aborts
 * the operation without updating the stack, so 0.0 and 1.0 are still on it
 * when this returns.  Harmless -- the caller's next act is to end the thread.
 */
	.globl	fperr_provoke_x87
fperr_provoke_x87:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$8, %esp

	fninit				/* control word = 0x037f, all masked */
	fnstcw	-2(%ebp)
	andw	$0xfffb, -2(%ebp)	/* clear ZM (bit 2): unmask zero-divide */
	fldcw	-2(%ebp)

	fld1				/* st(0) = 1.0            */
	fldz				/* st(0) = 0.0, st(1) = 1.0 */
	fdivp	%st, %st(1)		/* st(0) = 1.0/0.0 -> ZE, no trap yet */

	fwait				/* <-- the fault is taken HERE */

	leave
	ret

/*
 * fperr_provoke_sse -- an unmasked zero-divide in the SIMD unit.
 *
 * 🔑 Unlike the x87 arm this fault is PRECISE: #XF is raised by the division
 * itself, and the faulting address is the `divsd'.  Which means a handler that
 * answers KERN_SUCCESS sends the thread back into the same division and it
 * faults again whatever the kernel did -- so this routine is NOT expected to
 * return, and its caller says so rather than calling that a failure.
 *
 * ⚠️ 1.0 is built on the stack rather than loaded from a constant in .rodata,
 * so that this file needs no relocation and works the same whether the image
 * is position-independent or not.
 */
	.globl	fperr_provoke_sse
fperr_provoke_sse:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$16, %esp

	stmxcsr	-8(%ebp)
	andl	$0xfffffdff, -8(%ebp)	/* clear ZM (bit 9): unmask zero-divide */
	ldmxcsr	-8(%ebp)

	pushl	$0x3ff00000		/* the high half of 1.0 as a double */
	pushl	$0x00000000
	movsd	(%esp), %xmm0
	addl	$8, %esp

	xorpd	%xmm1, %xmm1		/* 0.0, and not a load either */
	divsd	%xmm1, %xmm0		/* <-- the fault is taken HERE */

	leave
	ret
