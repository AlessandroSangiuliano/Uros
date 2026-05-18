/*
 * Uros patch (#250 / Phase 2): replace musl's inline `int $128` /
 * `call *%%gs:16` syscall stubs with a C-ABI call into libposix-uros's
 * dispatcher.  This keeps musl's source side identical for callers
 * (every libc file still calls __syscallN as static inline) while
 * routing the actual transition through Uros instead of Linux.
 *
 * Rationale for not patching individual sites: musl uses __syscallN
 * in hundreds of places.  Centralising the redirect here is the
 * minimum-surface, easiest-to-rebase change against future musl
 * releases.  See uros/src/contrib/musl/UROS_PATCHES.md.
 *
 * The vDSO macros are dropped — Uros has its own fast-path mechanism
 * (#236) which is wired up in a later phase.
 */

extern long __uros_syscall0(long n);
extern long __uros_syscall1(long n, long a1);
extern long __uros_syscall2(long n, long a1, long a2);
extern long __uros_syscall3(long n, long a1, long a2, long a3);
extern long __uros_syscall4(long n, long a1, long a2, long a3, long a4);
extern long __uros_syscall5(long n, long a1, long a2, long a3, long a4, long a5);
extern long __uros_syscall6(long n, long a1, long a2, long a3,
                            long a4, long a5, long a6);

#define __SYSCALL_LL_E(x) \
((union { long long ll; long l[2]; }){ .ll = x }).l[0], \
((union { long long ll; long l[2]; }){ .ll = x }).l[1]
#define __SYSCALL_LL_O(x) __SYSCALL_LL_E((x))

static inline long __syscall0(long n)
{
	return __uros_syscall0(n);
}

static inline long __syscall1(long n, long a1)
{
	return __uros_syscall1(n, a1);
}

static inline long __syscall2(long n, long a1, long a2)
{
	return __uros_syscall2(n, a1, a2);
}

static inline long __syscall3(long n, long a1, long a2, long a3)
{
	return __uros_syscall3(n, a1, a2, a3);
}

static inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	return __uros_syscall4(n, a1, a2, a3, a4);
}

static inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	return __uros_syscall5(n, a1, a2, a3, a4, a5);
}

static inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	return __uros_syscall6(n, a1, a2, a3, a4, a5, a6);
}
