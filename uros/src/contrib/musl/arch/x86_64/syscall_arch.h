/*
 * Uros patch (#414): the i386 patch of #250 on this target.  Every __syscallN
 * is a C call into libposix-uros's dispatcher instead of the SYSCALL
 * instruction, which on Uros reaches the Mach trap table and nothing that
 * knows a Linux number.  musl's callers stay as they are: the redirect is here
 * and only here.  See uros/src/contrib/musl/UROS_PATCHES.md.
 *
 * The vDSO macros are dropped, as on i386: Uros maps no Linux vDSO.
 */

extern long __uros_syscall0(long n);
extern long __uros_syscall1(long n, long a1);
extern long __uros_syscall2(long n, long a1, long a2);
extern long __uros_syscall3(long n, long a1, long a2, long a3);
extern long __uros_syscall4(long n, long a1, long a2, long a3, long a4);
extern long __uros_syscall5(long n, long a1, long a2, long a3, long a4, long a5);
extern long __uros_syscall6(long n, long a1, long a2, long a3,
                            long a4, long a5, long a6);

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

static __inline long __syscall0(long n)
{
	return __uros_syscall0(n);
}

static __inline long __syscall1(long n, long a1)
{
	return __uros_syscall1(n, a1);
}

static __inline long __syscall2(long n, long a1, long a2)
{
	return __uros_syscall2(n, a1, a2);
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
	return __uros_syscall3(n, a1, a2, a3);
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	return __uros_syscall4(n, a1, a2, a3, a4);
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	return __uros_syscall5(n, a1, a2, a3, a4, a5);
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	return __uros_syscall6(n, a1, a2, a3, a4, a5, a6);
}

#define IPC_64 0
