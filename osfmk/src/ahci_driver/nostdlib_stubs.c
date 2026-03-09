/*
 * Stubs for symbols normally provided by the host C library.
 * Same pattern as ipc_bench/nostdlib_stubs.c.
 */

#include <mach/kern_return.h>
#include <mach/port.h>
#include <mach/message.h>

extern kern_return_t syscall_thread_switch(mach_port_t, int, mach_msg_timeout_t);

kern_return_t
thread_switch(mach_port_t thread, int option, mach_msg_timeout_t option_time)
{
	return syscall_thread_switch(thread, option, option_time);
}

void *
cthread_sp(void)
{
	void *sp;
	__asm__ __volatile__("movl %%esp, %0" : "=r" (sp));
	return sp;
}
