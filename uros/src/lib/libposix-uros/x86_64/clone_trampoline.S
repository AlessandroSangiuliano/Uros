/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * libposix-uros -- where a pthread's Mach thread starts, on x86-64 (#414).
 *
 * __uros_clone has put __uros_thread_bootstrap's four arguments in their
 * registers and left the stack as a call would have left it, so all that is
 * left is to zero %rbp, for a debugger to stop unwinding at the thread's root.
 */
	.text
	.globl	__uros_clone_trampoline
	.type	__uros_clone_trampoline, @function
__uros_clone_trampoline:
	xorl	%ebp, %ebp
	jmp	__uros_thread_bootstrap
	.size	__uros_clone_trampoline, . - __uros_clone_trampoline

	.section .note.GNU-stack,"",@progbits
