/*
 * Uros patch (#251 / Phase 3): replace the gs:0 TLS lookup with a
 * lookup through a global pointer __uros_tp.  Uros doesn't program
 * set_thread_area for single-threaded tasks, so a TLS-based
 * __pthread_self() would SEGV the moment musl touches stdio or the
 * SSP canary.
 *
 * __uros_tp must be initialised by __uros_libc_init() (see
 * src/internal/uros_main_thread.c) before any musl code runs.  Real
 * pthread support, with one TP per thread, returns in Phase 6 — at
 * which point this patch is replaced by an actual set_thread_area
 * path.  See uros/src/contrib/musl/UROS_PATCHES.md.
 */

extern unsigned long __uros_tp;

static inline uintptr_t __get_tp(void)
{
	return (uintptr_t)__uros_tp;
}

#define MC_PC gregs[REG_EIP]
