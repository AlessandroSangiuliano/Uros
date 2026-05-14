/*
 * Copyright (c) 2026 Alessandro Sangiuliano <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * thread_switch() wrapper for libpthreads.
 *
 * The spin lock assembly (i386_lock.S) calls thread_switch() to yield,
 * but libmach only exports syscall_thread_switch().  This thin wrapper
 * bridges the gap without pulling in the full RPC glue vector.
 */

#include <mach.h>
#include <mach/mach_syscalls.h>
#include <mach/thread_switch.h>

kern_return_t
thread_switch(
	mach_port_t		thread,
	int			option,
	mach_msg_timeout_t	option_time)
{
	return syscall_thread_switch(thread, option, option_time);
}

/*
 * sched_yield() / pthread_yield() — POSIX yield (#155).
 *
 * Both are wrappers over thread_switch with no target thread and
 * SWITCH_OPTION_NONE: relinquish the CPU and let the scheduler pick
 * whoever is next.  POSIX says sched_yield returns 0 on success, -1
 * on failure (with errno set); we never fail because the underlying
 * trap can't either.
 */
int
sched_yield(void)
{
	(void)syscall_thread_switch(MACH_PORT_NULL,
				    SWITCH_OPTION_NONE, 0);
	return 0;
}

int
pthread_yield(void)
{
	return sched_yield();
}
