/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Reading the stack pointer, which is how a thread finds itself (#425).
 *
 * pthread_self() is STACK_SELF(_sp()): the struct _pthread sits at the top of
 * the thread's own stack, so masking the stack pointer gives its address.
 * These two functions are the only part of that which a machine has to
 * supply, and they exist as assembly rather than as an asm() inside C for one
 * reason -- a C compiler is entitled to move the frame pointer around before
 * the first statement, and what the caller needs is the stack pointer as it
 * stands at the call.
 */

/*
 * ⚠️ No <machine/asm.h> and no ENTRY() macro, which every other machine's copy
 * of this file uses.  There is no asm.h for x86-64 in this tree and there is
 * not going to be one: the kernel's own x86-64 assembly writes .globl/.type/
 * .size directly, and so does the trap-stub macro in
 * <mach/x86_64/syscall_sw.h>.  A macro whose whole body is three directives is
 * a layer that hides which symbols a file defines.
 */

/*
 * vm_address_t _sp(void)
 *
 * ⚠️ %rsp and not %esp.  The assembler refuses the mismatch rather than
 * returning an address with its top half missing, which is the good outcome:
 * a truncated stack pointer would be masked into a struct _pthread address
 * that is not one, and pthread_self() would answer with a pointer into
 * nothing rather than fail.
 */
	.text
	.globl	_sp
	.type	_sp,@function
_sp:
	movq	%rsp, %rax
	ret
	.size	_sp, . - _sp

/*
 * vm_address_t _adjust_sp(vm_address_t sp)
 *
 * A fixed step away from the given stack pointer, used when a thread has to
 * name a point on its stack that its own frame does not reach.  The argument
 * arrives in %rdi here rather than at 4(%esp): on this machine the first six
 * arguments never touch the stack, which is also why there is nothing to load.
 */
	.globl	_adjust_sp
	.type	_adjust_sp,@function
_adjust_sp:
	movq	%rdi, %rax
	subq	$0x100, %rax
	ret
	.size	_adjust_sp, . - _adjust_sp
