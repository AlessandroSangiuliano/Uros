/*
 * libposix-uros — i386 Mach trap trampolines used by the syscall
 * dispatcher.  Each entry sets EAX to the trap number and issues the
 * Mach trap call gate (LCALL $0x7,$0).  We use the legacy LCALL path
 * (not SYSENTER) for simplicity: these are not on the IPC fast path
 * and Phase 2 cares about correctness, not speed.
 *
 * Symbol names are namespaced `__uros_trap_*` so they never collide
 * with libmach's identically-named public stubs — a binary may, during
 * the migration, link both libraries.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#define MACH_TRAP_GATE  .byte 0x9a; .long 0; .word 0x7  /* lcall $0x7,$0 */

.text

/* mach_print(const char *s)  — trap -14, debug console output */
.globl __uros_trap_mach_print
.type  __uros_trap_mach_print, @function
__uros_trap_mach_print:
        movl    $-14, %eax
        MACH_TRAP_GATE
        ret

/* mach_task_self(void) -> mach_port_t   — trap -28 */
.globl __uros_trap_mach_task_self
.type  __uros_trap_mach_task_self, @function
__uros_trap_mach_task_self:
        movl    $-28, %eax
        MACH_TRAP_GATE
        ret

/* syscall_vm_allocate(task, addr*, size, anywhere) -> kern_return_t — trap -65 */
.globl __uros_trap_vm_allocate
.type  __uros_trap_vm_allocate, @function
__uros_trap_vm_allocate:
        movl    $-65, %eax
        MACH_TRAP_GATE
        ret

/* syscall_vm_deallocate(task, addr, size) -> kern_return_t — trap -66 */
.globl __uros_trap_vm_deallocate
.type  __uros_trap_vm_deallocate, @function
__uros_trap_vm_deallocate:
        movl    $-66, %eax
        MACH_TRAP_GATE
        ret

/* syscall_vm_protect(task, addr, size, set_max, new_prot) -> kr — trap -91 */
.globl __uros_trap_vm_protect
.type  __uros_trap_vm_protect, @function
__uros_trap_vm_protect:
        movl    $-91, %eax
        MACH_TRAP_GATE
        ret

/* syscall_task_terminate(task) -> kern_return_t — trap -69 */
.globl __uros_trap_task_terminate
.type  __uros_trap_task_terminate, @function
__uros_trap_task_terminate:
        movl    $-69, %eax
        MACH_TRAP_GATE
        ret

/* Mark stack non-executable in the resulting ELF. */
.section .note.GNU-stack,"",@progbits
