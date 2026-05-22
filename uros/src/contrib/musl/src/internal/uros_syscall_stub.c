/*
 * uros_syscall_stub.c — weak placeholder syscall dispatcher (#234 incr 2a).
 *
 * Building musl as a shared library links libc.so with -Wl,--no-undefined,
 * so the patched arch/i386/syscall_arch.h (which routes every __syscallN to
 * an external __uros_syscallN, see #250) would otherwise fail to link: the
 * real dispatcher lives in libposix-uros, a *separate* archive that the
 * dynamic linker (== libc.so itself) cannot depend on — the ldso must be
 * self-contained.
 *
 * Increment 2a only needs libc.so / ld-musl-i386.so.1 to *link* so the
 * shared-build glue and the exec_server PT_INTERP hand-off can be exercised
 * structurally.  These definitions are therefore WEAK and return -ENOSYS:
 * any real Uros image links libposix-uros, whose strong __uros_syscallN
 * symbols override these stubs.  Increment 2b (the umbrella libc.so) bundles
 * the real dispatcher + Mach/VFS stack directly into libc.so and these
 * weak stubs simply drop out.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <errno.h>

#define UROS_SYS_STUB __attribute__((__weak__))

UROS_SYS_STUB long __uros_syscall0(long n)
{ (void)n; return -ENOSYS; }

UROS_SYS_STUB long __uros_syscall1(long n, long a1)
{ (void)n; (void)a1; return -ENOSYS; }

UROS_SYS_STUB long __uros_syscall2(long n, long a1, long a2)
{ (void)n; (void)a1; (void)a2; return -ENOSYS; }

UROS_SYS_STUB long __uros_syscall3(long n, long a1, long a2, long a3)
{ (void)n; (void)a1; (void)a2; (void)a3; return -ENOSYS; }

UROS_SYS_STUB long __uros_syscall4(long n, long a1, long a2, long a3, long a4)
{ (void)n; (void)a1; (void)a2; (void)a3; (void)a4; return -ENOSYS; }

UROS_SYS_STUB long __uros_syscall5(long n, long a1, long a2, long a3,
                                   long a4, long a5)
{ (void)n; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; return -ENOSYS; }

UROS_SYS_STUB long __uros_syscall6(long n, long a1, long a2, long a3,
                                   long a4, long a5, long a6)
{ (void)n; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
  return -ENOSYS; }

/*
 * Two more externs the patched asm stubs reach for (clone.s →
 * __uros_clone, __set_thread_area.s → __uros_set_thread_area_tp; see
 * UROS_PATCHES.md #256/#258).  Same weak-stub treatment so libc.so
 * links standalone in 2a; libposix-uros provides the strong versions.
 * Signatures must match libposix-uros so 2b overrides cleanly.
 */
UROS_SYS_STUB int __uros_clone(int (*fn)(void *), void *stack, int flags,
                               void *arg, int *ptid, void *newtls, int *ctid)
{ (void)fn; (void)stack; (void)flags; (void)arg;
  (void)ptid; (void)newtls; (void)ctid; return -ENOSYS; }

UROS_SYS_STUB int __uros_set_thread_area_tp(void *tp)
{ (void)tp; return -ENOSYS; }
