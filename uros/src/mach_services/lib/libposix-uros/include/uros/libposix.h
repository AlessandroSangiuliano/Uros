/*
 * uros/libposix.h — public header for libposix-uros
 *
 * libposix-uros is the POSIX-personality shim that sits between musl
 * libc and the Uros multiserver: it owns the __uros_syscallN dispatcher
 * called by musl's patched syscall_arch.h, translating Linux i386 SVR4
 * syscall numbers into Mach traps and (eventually) MIG RPCs to
 * proc_server / vfs / cap_server / exec_server.
 *
 * Phase 2 status (#250): infrastructure + ~15 handlers (write/writev/exit/
 * exit_group/mmap2/munmap real; the rest stubbed).  Anything beyond this
 * minimal set returns -ENOSYS and is the job of later phases.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#ifndef UROS_LIBPOSIX_H
#define UROS_LIBPOSIX_H

#define LIBPOSIX_UROS_VERSION_MAJOR  0
#define LIBPOSIX_UROS_VERSION_MINOR  1
#define LIBPOSIX_UROS_VERSION_PATCH  0
#define LIBPOSIX_UROS_VERSION_STRING "0.1.0"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Dispatcher entrypoints invoked by musl's patched
 * uros/src/contrib/musl/arch/i386/syscall_arch.h.  Arity matches the
 * static inline __syscallN shims musl uses; the n argument carries the
 * Linux i386 syscall number (see musl arch/i386/bits/syscall.h.in).
 *
 * Return is a `long` following Linux convention: nonnegative on success,
 * negative errno on failure (NOT the POSIX -1 + errno pattern — musl
 * itself translates that at the libc boundary).
 */
long __uros_syscall0(long n);
long __uros_syscall1(long n, long a1);
long __uros_syscall2(long n, long a1, long a2);
long __uros_syscall3(long n, long a1, long a2, long a3);
long __uros_syscall4(long n, long a1, long a2, long a3, long a4);
long __uros_syscall5(long n, long a1, long a2, long a3, long a4, long a5);
long __uros_syscall6(long n, long a1, long a2, long a3,
                     long a4, long a5, long a6);

#ifdef __cplusplus
}
#endif

#endif /* UROS_LIBPOSIX_H */
