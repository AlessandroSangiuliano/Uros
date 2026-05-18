/*
 * libposix-uros — syscall dispatcher.
 *
 * Entry points called by musl's patched arch/i386/syscall_arch.h.
 * All seven arities forward to the same internal __dispatch(), passing
 * unused arguments as zero — the C ABI handles this cleanly and the
 * handler table sees a uniform 6-arg signature.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <errno.h>

#include "uros/libposix.h"
#include "internal.h"

static long __dispatch(long n,
                       long a1, long a2, long a3,
                       long a4, long a5, long a6)
{
    __uros_handler_t h = __uros_lookup(n);
    if (!h)
        return -ENOSYS;
    return h(a1, a2, a3, a4, a5, a6);
}

long __uros_syscall0(long n)
{ return __dispatch(n, 0, 0, 0, 0, 0, 0); }

long __uros_syscall1(long n, long a1)
{ return __dispatch(n, a1, 0, 0, 0, 0, 0); }

long __uros_syscall2(long n, long a1, long a2)
{ return __dispatch(n, a1, a2, 0, 0, 0, 0); }

long __uros_syscall3(long n, long a1, long a2, long a3)
{ return __dispatch(n, a1, a2, a3, 0, 0, 0); }

long __uros_syscall4(long n, long a1, long a2, long a3, long a4)
{ return __dispatch(n, a1, a2, a3, a4, 0, 0); }

long __uros_syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{ return __dispatch(n, a1, a2, a3, a4, a5, 0); }

long __uros_syscall6(long n, long a1, long a2, long a3,
                     long a4, long a5, long a6)
{ return __dispatch(n, a1, a2, a3, a4, a5, a6); }
