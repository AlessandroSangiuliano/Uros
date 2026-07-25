/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 trap entry stubs (#409, MD contract 4/6).
 *
 * One stub per architectural vector, all funnelling into a single path that
 * builds the frame trap.h describes and calls C.
 *
 * The stubs exist because of an asymmetry: some exceptions push an error
 * code and the rest do not, so the stack the handler inherits has a
 * different shape depending on which one fired.  Each stub that fires
 * without an error code pushes a zero in its place, after which every
 * vector looks the same from here on and nothing downstream has to
 * remember which is which.
 */

	.section .text
	.code64

/*
 * Vectors 8, 10-14 and 17 push an error code; 21 does too but belongs to a
 * feature we do not enable.  Everything else does not.
 */
	.macro ISR_NOERR vec
	.align 16
isr_\vec:
	pushq	$0			/* stand in for the missing error */
	pushq	$\vec
	jmp	trap_common
	.endm

	.macro ISR_ERR vec
	.align 16
isr_\vec:
	pushq	$\vec			/* the CPU already pushed the error */
	jmp	trap_common
	.endm

	ISR_NOERR 0
	ISR_NOERR 1
	ISR_NOERR 2
	ISR_NOERR 3
	ISR_NOERR 4
	ISR_NOERR 5
	ISR_NOERR 6
	ISR_NOERR 7
	ISR_ERR   8
	ISR_NOERR 9
	ISR_ERR   10
	ISR_ERR   11
	ISR_ERR   12
	ISR_ERR   13
	ISR_ERR   14
	ISR_NOERR 15
	ISR_NOERR 16
	ISR_ERR   17
	ISR_NOERR 18
	ISR_NOERR 19
	ISR_NOERR 20
	ISR_NOERR 21
	ISR_NOERR 22
	ISR_NOERR 23
	ISR_NOERR 24
	ISR_NOERR 25
	ISR_NOERR 26
	ISR_NOERR 27
	ISR_NOERR 28
	ISR_NOERR 29
	ISR_NOERR 30
	ISR_NOERR 31

/*
 * The common path.  Registers are pushed in reverse of the order they are
 * declared in struct trap_frame, because a push moves down: whatever goes
 * last ends up at the lowest address, which is the first field.
 *
 * The stack is 16-byte aligned at the call by construction — the CPU aligns
 * it before pushing its own five words, and the frame this builds is a
 * multiple of sixteen — so no fixup is needed and none is done, which keeps
 * %rsp pointing at the frame itself.
 */
trap_common:
	pushq	%rax
	pushq	%rbx
	pushq	%rcx
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	pushq	%rbp
	pushq	%r8
	pushq	%r9
	pushq	%r10
	pushq	%r11
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15

	movq	%rsp, %rdi		/* the frame is the argument */
	call	trap_dispatch

	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%r11
	popq	%r10
	popq	%r9
	popq	%r8
	popq	%rbp
	popq	%rdi
	popq	%rsi
	popq	%rdx
	popq	%rcx
	popq	%rbx
	popq	%rax

	addq	$16, %rsp		/* drop the vector and error code */
	iretq

/*
 * trap_probe_write(addr) — store a recognisable pattern at addr and report
 * whether the machine allowed it.  Returns 0 when the store completed and 1
 * when it faulted, the handler having been told to resume at
 * trap_probe_faulted.
 *
 * A pattern rather than a zero, so the caller can check that an allowed
 * store actually landed.  Storing zero would leave a caller unable to tell a
 * store that happened from one that never needed to: most of what it might
 * probe is zero already.
 *
 * In assembly on purpose.  The obvious C version — take the address of a
 * label, arm the handler with it, fault, resume there — is exactly the case
 * the compiler manual warns about: a label whose address escapes into
 * another function is not reachable in the control-flow graph, so the block
 * is optimised away and the address resolves to whatever is nearby.  It did
 * here, landing back in the prologue and looping.
 *
 * Two instructions and a known symbol have no such opinions.  The resume
 * point is in this same frame, which is what makes the return safe: iretq
 * restores the stack pointer too, and nothing here has moved it.
 */
	.globl	trap_probe_write
	.globl	trap_probe_faulted
	.align	16
trap_probe_write:
	movl	$0x5a5a5a5a, (%rdi)
	xorl	%eax, %eax		/* the store was allowed */
	ret
trap_probe_faulted:
	movl	$1, %eax		/* the store was refused */
	ret

/*
 * The stub addresses, in vector order, so trap_init() can install them
 * without needing thirty-two declarations of things that differ only by a
 * number.
 */
	.section .rodata
	.align	8
	.globl	isr_table
isr_table:
	.quad	isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7
	.quad	isr_8,  isr_9,  isr_10, isr_11, isr_12, isr_13, isr_14, isr_15
	.quad	isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23
	.quad	isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31
