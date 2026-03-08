/*
 * Stubs for symbols normally provided by the host C library or
 * by libmach's ms_*.c wrappers (which we exclude because they
 * require sed-based MIG output renaming).
 *
 * The bootstrap server is linked with -nostdlib, so we must
 * supply these ourselves.
 */

#include <mach/kern_return.h>
#include <mach/port.h>
#include <mach/message.h>

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
 * The original build used an awk-based inliner; we just provide them here.
 */
void *
cthread_sp(void)
{
	void *sp;
	__asm__ __volatile__("movl %%esp, %0" : "=r" (sp));
	return sp;
}

/*
 * device_read_overwrite() — another ms_*.c wrapper we excluded.
 * The original adds MACH_SEND_INTERRUPTED retry; for the bootstrap
 * server's simple synchronous reads, the direct trap suffices.
 */
extern kern_return_t syscall_device_read_overwrite(
	mach_port_t, int, int, int, unsigned int, unsigned int *);

kern_return_t
device_read_overwrite(mach_port_t device, int mode, int recnum,
		      int bytes_wanted, unsigned int buffer,
		      unsigned int *data_count)
{
	return syscall_device_read_overwrite(device, mode, recnum,
					     bytes_wanted, buffer, data_count);
}
