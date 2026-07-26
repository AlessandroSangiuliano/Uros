/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Switching from one thread to another (#408, MD contract 3/6).
 *
 * The reasoning for what is and is not saved here is in
 * <thread/context.h>; this is the shape it takes.
 */

	.section .text
	.code64

/*
 * context_switch_raw(uint64_t *old_rsp, uint64_t fresh_rsp)
 *
 * Six pushes, one store, one load, six pops, and a return that goes
 * somewhere else.  The instruction that makes it a switch rather than a
 * call is the load: after it, the values popped belong to a different
 * thread and the return address is wherever that thread stopped.
 *
 * The registers are pushed in the order the initial frame in context_init()
 * lays them out, and the two must be read together — a thread that has
 * never run is resumed by exactly this code, so its stack has to be a
 * forgery good enough to fool it.
 */
	.globl	context_switch_raw
	.align	16
context_switch_raw:
	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15

	movq	%rsp, (%rdi)		/* the old thread is now this word */
	movq	%rsi, %rsp		/* and from here we are the new one */

	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ret

/*
 * Where a thread that has never run begins.
 *
 * It is reached by the `ret` above, from a frame context_init() wrote — so
 * it is not called, and there is no caller to return to.  What would be the
 * return address is a deliberate zero, which is what stops a backtrace here
 * rather than letting it walk into whatever the stack held before.
 *
 * The entry point and its argument arrive in callee-saved registers because
 * those are the only ones the switch restores; every other register in the
 * machine is undefined at this instant.
 */
	.globl	context_thread_start
	.align	16
context_thread_start:
	movq	%r12, %rdi		/* the argument */
	callq	*%r13			/* the thread's own function */

	/*
	 * A thread function that returns has nowhere to go: there is no
	 * caller, and the stack below is a frame that was invented.  Falling
	 * through would return into the zero left there deliberately.
	 *
	 * So it stops here and says why, rather than faulting at an address
	 * that would tell nobody anything.
	 */
	leaq	.Lreturned(%rip), %rdi
	call	panic

	.section .rodata
.Lreturned:
	.asciz	"thread: a thread function returned, and there is nowhere to return to"

/*
 * context_probe(self, other, pattern, rounds) — the check the switch exists
 * to pass.
 *
 * Fill every callee-saved register with a pattern of this thread's own,
 * switch away, and on the way back look at all six.  Any that changed is
 * folded into the answer, so a single wrong register makes the result
 * unrecognisable rather than merely wrong by a bit.
 *
 * In assembly because C cannot express it: the compiler owns these
 * registers and will not let a function observe them across a call it can
 * see through.  The patterns differ per thread, so a switch that saved to
 * the wrong place hands back a neighbour's values rather than nothing.
 */
	.section .text
	.globl	context_probe
	.align	16
context_probe:
	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15

	/*
	 * A frame for the three things that must survive the call, since
	 * every register that could have held them is either part of the
	 * experiment or destroyed by the call.  Forty bytes rather than
	 * twenty-four so the stack is sixteen-byte aligned at the call.
	 */
	subq	$40, %rsp
	movq	%rdi, 0(%rsp)		/* self    */
	movq	%rsi, 8(%rsp)		/* other   */
	movl	%ecx, 16(%rsp)		/* rounds  */
	movq	%rdx, 24(%rsp)		/* pattern */

	movq	%rdx, %rbx		/* the pattern, in every one of them */
	movq	%rdx, %rbp
	movq	%rdx, %r12
	movq	%rdx, %r13
	movq	%rdx, %r14
	movq	%rdx, %r15

1:
	movq	0(%rsp), %rdi
	movq	8(%rsp), %rsi
	call	context_switch

	movl	16(%rsp), %eax
	decl	%eax
	movl	%eax, 16(%rsp)
	jnz	1b

	/*
	 * The answer is what *differs*, folded: zero means all six came back.
	 *
	 * The obvious version — OR the six together and compare against the
	 * pattern — was written first and was blind, which a control caught
	 * before this test was ever believed.  A register restored as zero
	 * disappears into an OR, because x | 0 is x, so the one failure mode
	 * a forgotten push actually produces was the one the check could not
	 * see.  Comparing each against the pattern first has no such blind
	 * spot: any deviation in any register, in either direction, survives
	 * into the result.
	 */
	movq	24(%rsp), %rcx		/* the pattern, from the frame */

	movq	%rbx, %rax
	xorq	%rcx, %rax
	movq	%rbp, %rdx
	xorq	%rcx, %rdx
	orq	%rdx, %rax
	movq	%r12, %rdx
	xorq	%rcx, %rdx
	orq	%rdx, %rax
	movq	%r13, %rdx
	xorq	%rcx, %rdx
	orq	%rdx, %rax
	movq	%r14, %rdx
	xorq	%rcx, %rdx
	orq	%rdx, %rax
	movq	%r15, %rdx
	xorq	%rcx, %rdx
	orq	%rdx, %rax

	addq	$40, %rsp
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ret
