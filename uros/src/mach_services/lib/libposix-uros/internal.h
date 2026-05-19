/*
 * libposix-uros — internal header.  Not exported.
 *
 * Trap trampoline prototypes (implemented in i386/traps.S) and the
 * shared signature for syscall handlers.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#ifndef UROS_LIBPOSIX_INTERNAL_H
#define UROS_LIBPOSIX_INTERNAL_H

#include <stdint.h>

typedef int           __uros_kern_return_t;
typedef unsigned int  __uros_port_t;

/*
 * Mach trap trampolines (i386/traps.S).  These are 1-instruction
 * tail-call thunks into libmach_core's identically-numbered stubs —
 * libmach_core ships the SYSENTER fast path via the canonical
 * <mach/syscall_sw.h> chain, and libposix-uros just borrows it
 * under a namespaced name so handlers.c stays freestanding (no
 * mach.h, no kern_return_t / mach_port_t type clashes).  See
 * memory `mach-trap-sysenter-policy`.
 */
extern void                  __uros_trap_mach_print(const char *s);
extern __uros_port_t         __uros_trap_mach_task_self(void);
extern __uros_kern_return_t  __uros_trap_vm_allocate(__uros_port_t task,
                                                    unsigned long *addr,
                                                    unsigned long size,
                                                    int anywhere);
extern __uros_kern_return_t  __uros_trap_vm_deallocate(__uros_port_t task,
                                                      unsigned long addr,
                                                      unsigned long size);
extern __uros_kern_return_t  __uros_trap_vm_protect(__uros_port_t task,
                                                   unsigned long addr,
                                                   unsigned long size,
                                                   int set_max,
                                                   int new_prot);
extern __uros_kern_return_t  __uros_trap_task_terminate(__uros_port_t task);

/*
 * Handler signature.  All Linux syscalls go through a single 6-arg
 * dispatch — the C ABI tolerates passing extra unused arguments, and
 * this keeps the table flat and the dispatch loop branchless.
 */
typedef long (*__uros_handler_t)(long a1, long a2, long a3,
                                 long a4, long a5, long a6);

/*
 * Look up the handler for Linux syscall `n`.  Returns NULL when the
 * syscall is unknown; the dispatcher then returns -ENOSYS.
 */
__uros_handler_t __uros_lookup(long n);

#endif /* UROS_LIBPOSIX_INTERNAL_H */
