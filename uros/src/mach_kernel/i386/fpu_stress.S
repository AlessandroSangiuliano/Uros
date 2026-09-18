/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * Does the switch really carry this thread's floating-point state? (#560)
 *
 *	void			fpu_stress_load(unsigned long long pattern);
 *	unsigned long long	fpu_stress_check(unsigned long long pattern);
 *	unsigned long long	fpu_stress_unload(unsigned long long pattern);
 *
 * load() writes `pattern' into all eight SSE registers and all eight x87 stack
 * registers.  check() re-reads the SSE ones and answers the OR of whatever
 * differed, without touching them.  unload() takes the x87 stack apart at the
 * end and answers the same way.  Zero means the state came back intact.
 *
 * 🔴 THREE ENTRY POINTS, AND THAT IS THE POINT.  The first version of this
 * file was one function that wrote the registers and then span re-reading them,
 * exactly like the x86-64 test it was modelled on.  It PASSED WITH THE RESTORE
 * REMOVED FROM fpu_load_context() -- a test that proves nothing.
 *
 * The reason is that the three threads ran one after another rather than
 * interleaved: each wrote its registers at the start of its own turn and read
 * them back inside it, so no read ever happened after a switch.  Splitting the
 * write from the read lets the thread SLEEP between them, which makes the
 * switch a certainty instead of something hoped for from quantum expiry.
 *
 * ⚠️ Nothing here reloads.  The registers are written once, by load(), and
 * every later check() asks the scheduler what it kept.
 *
 * 🔑 Both halves of the unit on purpose.  FXSAVE/XSAVE write x87 and SSE into
 * one area with one instruction, so a switch that drops one drops the other --
 * but i386 kernel code does use x87 (_doprnt_ext formats floats with it),
 * which x86-64's -mgeneral-regs-only rules out.  The x87 half is what would
 * notice that.
 *
 * ⚠️ The x87 stack holds eight registers and filling it leaves no room to
 * duplicate one for a non-destructive read, which is why that half is asked
 * once at the end by unloading it rather than on every check.
 */

	.section .text

/*
 * void fpu_stress_load(unsigned long long pattern);
 */
	.globl	fpu_stress_load
	.align	16
fpu_stress_load:
	/*
	 * A known control word before anything else.  The one a thread is born
	 * with leaves invalid-operation UNMASKED (fp_state_alloc_pcb), and the
	 * unload at the end converts back to integer: a register that came
	 * back as a NaN would raise #MF there instead of being reported as the
	 * damage it is.  The test says what went wrong; it does not take the
	 * thread down with it.
	 *
	 * fninit masks everything, and the control word is itself part of the
	 * state the switch has to carry.
	 */
	fninit

	movq	4(%esp), %xmm0
	movq	4(%esp), %xmm1
	movq	4(%esp), %xmm2
	movq	4(%esp), %xmm3
	movq	4(%esp), %xmm4
	movq	4(%esp), %xmm5
	movq	4(%esp), %xmm6
	movq	4(%esp), %xmm7

	fildll	4(%esp)
	fildll	4(%esp)
	fildll	4(%esp)
	fildll	4(%esp)
	fildll	4(%esp)
	fildll	4(%esp)
	fildll	4(%esp)
	fildll	4(%esp)
	ret

/*
 * unsigned long long fpu_stress_check(unsigned long long pattern);
 *
 * Each register compared on its own and OR'ed in separately.  Folding them
 * together first -- summing, or XOR'ing the lot into one word -- would let two
 * wrong registers cancel each other out.
 */
	.globl	fpu_stress_check
	.align	16
fpu_stress_check:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%ebx
	pushl	%edi
	subl	$8, %esp		/* scratch for one 64-bit read-back */

	movl	8(%ebp), %ebx		/* pattern, low half */
	movl	12(%ebp), %edi		/* pattern, high half */
	xorl	%eax, %eax		/* damage, low */
	xorl	%edx, %edx		/* damage, high */

	movq	%xmm0, (%esp)
	movl	(%esp), %ecx
	xorl	%ebx, %ecx
	orl	%ecx, %eax
	movl	4(%esp), %ecx
	xorl	%edi, %ecx
	orl	%ecx, %edx

	movq	%xmm1, (%esp)
	movl	(%esp), %ecx
	xorl	%ebx, %ecx
	orl	%ecx, %eax
	movl	4(%esp), %ecx
	xorl	%edi, %ecx
	orl	%ecx, %edx

	movq	%xmm2, (%esp)
	movl	(%esp), %ecx
	xorl	%ebx, %ecx
	orl	%ecx, %eax
	movl	4(%esp), %ecx
	xorl	%edi, %ecx
	orl	%ecx, %edx

	movq	%xmm3, (%esp)
	movl	(%esp), %ecx
	xorl	%ebx, %ecx
	orl	%ecx, %eax
	movl	4(%esp), %ecx
	xorl	%edi, %ecx
	orl	%ecx, %edx

	movq	%xmm4, (%esp)
	movl	(%esp), %ecx
	xorl	%ebx, %ecx
	orl	%ecx, %eax
	movl	4(%esp), %ecx
	xorl	%edi, %ecx
	orl	%ecx, %edx

	movq	%xmm5, (%esp)
	movl	(%esp), %ecx
	xorl	%ebx, %ecx
	orl	%ecx, %eax
	movl	4(%esp), %ecx
	xorl	%edi, %ecx
	orl	%ecx, %edx

	movq	%xmm6, (%esp)
	movl	(%esp), %ecx
	xorl	%ebx, %ecx
	orl	%ecx, %eax
	movl	4(%esp), %ecx
	xorl	%edi, %ecx
	orl	%ecx, %edx

	movq	%xmm7, (%esp)
	movl	(%esp), %ecx
	xorl	%ebx, %ecx
	orl	%ecx, %eax
	movl	4(%esp), %ecx
	xorl	%edi, %ecx
	orl	%ecx, %edx

	addl	$8, %esp
	popl	%edi
	popl	%ebx
	popl	%ebp
	ret

/*
 * unsigned long long fpu_stress_unload(unsigned long long pattern);
 *
 * The x87 stack, taken apart.  Eight registers all holding the same value, so
 * the order they come off in does not matter.
 */
	.globl	fpu_stress_unload
	.align	16
fpu_stress_unload:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	%ebx
	pushl	%edi
	pushl	%esi
	subl	$8, %esp		/* scratch for one 64-bit read-back */

	movl	8(%ebp), %ebx		/* pattern, low half */
	movl	12(%ebp), %edi		/* pattern, high half */
	xorl	%eax, %eax
	xorl	%edx, %edx
	movl	$8, %esi
1:
	fistpll	(%esp)
	movl	(%esp), %ecx
	xorl	%ebx, %ecx
	orl	%ecx, %eax
	movl	4(%esp), %ecx
	xorl	%edi, %ecx
	orl	%ecx, %edx
	decl	%esi
	jnz	1b

	addl	$8, %esp
	popl	%esi
	popl	%edi
	popl	%ebx
	popl	%ebp
	ret
