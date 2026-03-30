/*
 * thread_switch() wrapper for libpthreads.
 *
 * The spin lock assembly (i386_lock.S) calls thread_switch() to yield,
 * but libmach only exports syscall_thread_switch().  This thin wrapper
 * bridges the gap without pulling in the full RPC glue vector.
 */

#include <mach.h>
#include <mach/mach_syscalls.h>

kern_return_t
thread_switch(
	mach_port_t		thread,
	int			option,
	mach_msg_timeout_t	option_time)
{
	return syscall_thread_switch(thread, option, option_time);
}
