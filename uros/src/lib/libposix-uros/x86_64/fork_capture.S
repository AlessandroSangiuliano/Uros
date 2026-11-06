/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * libposix-uros -- the register state of a fork(), on x86-64 (#414).
 *
 * int __uros_capture_state(struct x86_64_thread_state *state)
 *
 * Writes what the caller will still be relying on after the call: the
 * registers the C ABI preserves, the stack pointer as it will be after the
 * `ret', and the return address.  Answers 0.  The child resumes at that return
 * address with what the parent has put in rax, and that is how it tells itself
 * from the parent.
 *
 * Everything else in the structure is the caller's to fill: it starts it at
 * zero, and the kernel imposes the selectors and the flags in any case.
 *
 * ⚠️ The offsets are those of struct x86_64_thread_state, and posix_fork.c
 * asserts the same four numbers against the structure.
 */
#define	STATE_RBX	8
#define	STATE_RBP	48
#define	STATE_RSP	56
#define	STATE_R12	96
#define	STATE_R13	104
#define	STATE_R14	112
#define	STATE_R15	120
#define	STATE_RIP	128

	.text
	.globl	__uros_capture_state
	.type	__uros_capture_state, @function
__uros_capture_state:
	movq	%rbx, STATE_RBX(%rdi)
	movq	%rbp, STATE_RBP(%rdi)
	movq	%r12, STATE_R12(%rdi)
	movq	%r13, STATE_R13(%rdi)
	movq	%r14, STATE_R14(%rdi)
	movq	%r15, STATE_R15(%rdi)
	movq	(%rsp), %rax
	movq	%rax, STATE_RIP(%rdi)
	leaq	8(%rsp), %rax
	movq	%rax, STATE_RSP(%rdi)
	xorl	%eax, %eax
	ret
	.size	__uros_capture_state, . - __uros_capture_state

	.section .note.GNU-stack,"",@progbits
