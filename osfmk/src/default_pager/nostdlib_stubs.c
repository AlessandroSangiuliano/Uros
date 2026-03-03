/*
 * Stubs for symbols normally provided by the host C library or
 * by libmach's ms_*.c wrappers (which we exclude because they
 * require sed-based MIG output renaming).
 *
 * The default_pager server is linked with -nostdlib, so we must
 * supply these ourselves.  Same pattern as bootstrap/nostdlib_stubs.c.
 */

#include <mach/kern_return.h>
#include <mach/port.h>
#include <mach/message.h>

/* GCC stack-smashing protector callback (called on stack overflow). */
void __stack_chk_fail_local(void)
{
	extern void panic(const char *);
	panic("stack smashing detected");
}

void __stack_chk_fail(void)
{
	__stack_chk_fail_local();
}

/*
 * thread_switch() — thin wrapper over the syscall_thread_switch() Mach trap.
 * The original ms_thread_switch.c checked _rpc_glue_vector for RPC
 * short-circuiting, but standalone Mach programs always use the trap.
 */
extern kern_return_t syscall_thread_switch(mach_port_t, int, mach_msg_timeout_t);

kern_return_t
thread_switch(mach_port_t thread, int option, mach_msg_timeout_t option_time)
{
	return syscall_thread_switch(thread, option, option_time);
}

/*
 * Out-of-line copies of functions declared as "extern __inline__" in
 * i386/cthreads.h.  In GNU C89 mode, extern inline does NOT emit an
 * out-of-line body — the linker expects one from somewhere else.
 */
void *
cthread_sp(void)
{
	void *sp;
	__asm__ __volatile__("movl %%esp, %0" : "=r" (sp));
	return sp;
}
