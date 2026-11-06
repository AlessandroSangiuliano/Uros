/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * libposix-uros -- a thread gives back the stack it stands on, x86-64 (#414).
 *
 * void __uros_unmap_and_exit(mach_port_t task, vm_offset_t base, vm_size_t size,
 *                            mach_port_t thread)
 *
 * Between the two traps there is no stack, so nothing here may touch one: that
 * rules out a call, which is why the traps are written out and not the stubs.
 * The fourth argument moves out of %rcx first, because SYSCALL takes %rcx for
 * the return address.  %rbx is the caller's, and the caller never sees it again.
 *
 * The numbers come from the one list of traps, so that there is no second one.
 */
#include <mach/machine/syscall_sw.h>

#undef	kernel_trap
#define	kernel_trap(name, number, nargs)	.set TRAP_##name, SYSCALL_MACH_BASE - (number)

#include <mach/syscall_sw.h>

	.text
	.globl	__uros_unmap_and_exit
	.type	__uros_unmap_and_exit, @function
__uros_unmap_and_exit:
	movq	%rcx, %rbx
	movl	$TRAP_syscall_vm_deallocate, %eax
	syscall
	movq	%rbx, %rdi
	movl	$TRAP_syscall_thread_terminate, %eax
	syscall
	hlt
	.size	__uros_unmap_and_exit, . - __uros_unmap_and_exit

	.section .note.GNU-stack,"",@progbits
