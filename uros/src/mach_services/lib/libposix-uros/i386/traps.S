/*
 * libposix-uros — i386 Mach trap tail-call thunks.
 *
 * Each entry is a single `jmp` to the corresponding libmach_core
 * stub, which is the canonical SYSENTER fast path emitted by
 * <mach/syscall_sw.h>.  libposix-uros gets the namespaced
 * `__uros_trap_*` symbols handlers.c expects without duplicating
 * the SYSENTER sequence — single source of truth lives in libmach.
 * See project memory `mach-trap-sysenter-policy`.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

.text

#define UROS_TRAP_ALIAS(uros_name, mach_name)   \
.globl uros_name                            ;   \
.type  uros_name, @function                 ;   \
uros_name:                                      \
        jmp     mach_name

UROS_TRAP_ALIAS(__uros_trap_mach_print,      mach_print)
UROS_TRAP_ALIAS(__uros_trap_mach_task_self,  mach_task_self)
UROS_TRAP_ALIAS(__uros_trap_vm_allocate,     syscall_vm_allocate)
UROS_TRAP_ALIAS(__uros_trap_vm_deallocate,   syscall_vm_deallocate)
UROS_TRAP_ALIAS(__uros_trap_vm_protect,      syscall_vm_protect)
UROS_TRAP_ALIAS(__uros_trap_task_terminate,  syscall_task_terminate)

.section .note.GNU-stack,"",@progbits
