/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Holding vector state across a switch nobody asked for (#408).
 *
 * ⚠️ HOW THIS DIFFERS FROM fpu_probe() IN context.S, WHICH ALREADY EXISTS.
 * That one builds two contexts by hand and calls context_switch() between
 * them, on the boot processor, before there is a scheduler.  It proves the
 * switch saves and restores the register file WHEN THE THREAD ASKS FOR IT --
 * and that was the only kind of switch this machine had when it was written.
 *
 * Preemption is the other kind, and it is a different claim: the switch
 * arrives at an instruction boundary this code did not choose, from an
 * interrupt, on a processor that may not be the one it started on.  Nothing
 * has ever tested that here, because until #459 gave this target a clock and
 * #461 gave it processors, an involuntary switch could not happen at all.
 *
 * Necessarily assembly.  The kernel is compiled -mgeneral-regs-only so that it
 * never dirties state it is holding for somebody else, so these registers are
 * named nowhere else in the tree.
 *
 *	uint64_t fpu_stress(uint64_t pattern, volatile int *stop);
 *
 * Fills all sixteen vector registers with `pattern', then re-reads them until
 * `*stop' is set, and answers with the OR of everything that ever differed.
 * Zero means the state came back intact from every switch that happened in
 * between; any other value names, bit by bit, what came back wrong.
 *
 * ⚠️ It never reloads.  The registers are written ONCE, before the loop, so
 * every later read is asking the scheduler what it kept -- a reload inside the
 * loop would repair the damage it is trying to measure.
 *
 * ⚠️ And it calls nothing.  A call would put the compiler's code between the
 * write and the read, and although this kernel emits no vector instructions,
 * "although" is the word that makes a test prove something else.
 */

	.section .text
	.globl	fpu_stress
	.align	16
fpu_stress:
	movq	%rdi, %xmm0
	movq	%rdi, %xmm1
	movq	%rdi, %xmm2
	movq	%rdi, %xmm3
	movq	%rdi, %xmm4
	movq	%rdi, %xmm5
	movq	%rdi, %xmm6
	movq	%rdi, %xmm7
	movq	%rdi, %xmm8
	movq	%rdi, %xmm9
	movq	%rdi, %xmm10
	movq	%rdi, %xmm11
	movq	%rdi, %xmm12
	movq	%rdi, %xmm13
	movq	%rdi, %xmm14
	movq	%rdi, %xmm15

	xorq	%rax, %rax		/* everything that ever differed */

	/*
	 * Sixteen registers compared every turn, and each one OR'ed in
	 * separately.  Folding them together first -- summing, or XOR'ing the
	 * lot into one word -- would let two wrong registers cancel, and would
	 * make a register restored as zero look exactly like one restored
	 * right when the pattern is zero.  The same criterion fpu_probe() uses,
	 * and for the same reason.
	 */
1:
	movq	%xmm0, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax
	movq	%xmm1, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax
	movq	%xmm2, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax
	movq	%xmm3, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax
	movq	%xmm4, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax
	movq	%xmm5, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax
	movq	%xmm6, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax
	movq	%xmm7, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax
	movq	%xmm8, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax
	movq	%xmm9, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax
	movq	%xmm10, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax
	movq	%xmm11, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax
	movq	%xmm12, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax
	movq	%xmm13, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax
	movq	%xmm14, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax
	movq	%xmm15, %rdx
	xorq	%rdi, %rdx
	orq	%rdx, %rax

	cmpl	$0, (%rsi)
	je	1b

	ret
