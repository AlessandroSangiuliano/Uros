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
 * libposix-uros doesn't ship its own Mach trap stubs — every
 * musl-linked Uros server already links libmach_core, which emits
 * the SYSENTER fast path via `#include <mach/syscall_sw.h>`.  Files
 * that need Mach traps either include `<mach.h>` & friends (signals.c,
 * exceptions.c, posix_fork.c, posix_clone.c — all already include
 * the proper Mach headers) or declare the few they reach for locally
 * (handlers.c, see top of file).
 *
 * No __uros_trap_* aliases — same names as libmach, fewer indirections.
 * See memory `mach-trap-sysenter-policy`.
 */

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
