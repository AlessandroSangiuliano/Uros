/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The public names of the trap-first calls, which the headers promised and no
 * archive delivered.
 *
 * libmach's build drops the ms_*.c wrappers, on BOTH targets and deliberately:
 * they call mig_<name>() variants that wanted a sed rename of MIG's output
 * which this build system does not do, and for almost every one of them the
 * generated stubs supply the public name directly instead, so the wrapper is
 * redundant.  "Almost" is the whole of this file.  A handful of these calls are
 * in no .defs at all -- they are Mach traps with an RPC fallback, not RPCs --
 * so no stub can supply them, and dropping the wrapper dropped the only
 * definition there was.
 *
 * 🔥 And the declarations stayed.  <mach.h> declares vm_read_overwrite and
 * device_read_overwrite to this day, so the gap did not show up as a missing
 * header or a warning: it showed up at link time, in whichever program was
 * unlucky enough to call one -- and what those programs did was write a private
 * copy.  bootstrap and ext_server each grew a device_read_overwrite() of their
 * own, byte for byte identical, each with a comment explaining the renamed
 * stubs that do not exist.  A promise in a header with nothing behind it does
 * not stay unused; it gets satisfied locally, once per caller.
 *
 * ⚠️ What is NOT here, and why, so the next person does not conclude it was
 * forgotten:
 *
 *   device_read_overwrite_request -- its fallback has to answer the reply port
 *     through ds_device_read_reply_overwrite(), which comes from
 *     device_reply.defs.  That .defs is in libmach's MIG list on i386 and not
 *     on x86-64, so the call can be defined on one target and not the other,
 *     and a libmach whose surface depends on the target is the defect this
 *     file exists to remove rather than a fix for it.  It belongs to #426,
 *     with the .defs added deliberately: every previous addition to that list
 *     uncovered a type whose width the two trees disagreed about.
 *
 *   thread_switch -- not missing, over-supplied.  Eight definitions exist in
 *     the tree already, seven of them in per-server nostdlib_stubs.c files and
 *     one in libpthreads.  A ninth here would be a duplicate symbol inside the
 *     --start-group every one of those programs links with, so consolidating
 *     them is a change to every server and needs to be one.
 *
 *   mach_port_allocate_rt -- nothing underneath it at all: no trap in
 *     syscall_sw.c and no routine in any .defs.  It is a name from a Mach this
 *     system never had, and there is nothing to make callable.
 */

#include <mach.h>
#include <mach/message.h>
#include <mach/mach_syscalls.h>
#include <device/device.h>
#include <string.h>

/*
 * Read another task's memory into a buffer the caller already owns.
 *
 * The trap does the whole job when the target is reachable from it.  The
 * fallback is vm_read(), which answers with out-of-line memory instead, so the
 * wrapper copies it where the caller asked and releases it -- which is the
 * entire reason the overwrite form is worth having: its caller does not have to
 * know that a page arrived, or unmap it afterwards.
 */
kern_return_t
vm_read_overwrite(
	mach_port_t	task,
	vm_address_t	address,
	vm_size_t	size,
	vm_address_t	data,
	vm_size_t	*data_count)
{
	kern_return_t	result;

	result = syscall_vm_read_overwrite(task, address, size, data,
					   data_count);
	if (result == MACH_SEND_INTERRUPTED) {
		vm_offset_t		buf;
		mach_msg_type_number_t	count;

		result = vm_read(task, address, size, &buf, &count);
		if (result == KERN_SUCCESS) {
			memcpy((char *) data, (const char *) buf, count);
			(void) vm_deallocate(mach_task_self(),
					     (vm_address_t) buf, count);
			*data_count = count;
		}
	}
	return result;
}

/*
 * Read from a device into a buffer the caller already owns.
 *
 * 🔑 MACH_SEND_INTERRUPTED here is not an error and not an interruption: the
 * trap answers with it when port_name_to_device() cannot resolve the port,
 * which is precisely the case of a device implemented by a userspace server
 * rather than by the kernel (kern/ipc_mig.c).  So the two arms are the two
 * kinds of device this system has, and the fallback is the normal path for
 * every driver that lives outside the kernel.
 *
 * ⚠️ That is what the private copies in bootstrap and ext_server got wrong in a
 * way that cost nothing until it did: they skipped the trap entirely, on the
 * argument that their device is always a userspace server.  True for them,
 * and it makes the call an RPC for everybody who is not them.
 *
 * The fallback calls device_read() where the original wrapper called
 * mig_device_read().  Those are the same stub: the mig_ prefix was what the
 * sed rename produced so that the wrapper could export the public name itself,
 * and with no rename the stub already IS the public name.
 */
kern_return_t
device_read_overwrite(
	mach_port_t		device,
	dev_mode_t		mode,
	recnum_t		recnum,
	io_buf_len_t		bytes_wanted,
	vm_address_t		data,
	mach_msg_type_number_t	*data_count)
{
	kern_return_t	result;

	result = syscall_device_read_overwrite(device, mode, recnum,
					       bytes_wanted, data, data_count);
	if (result == MACH_SEND_INTERRUPTED) {
		io_buf_ptr_t		buf;
		mach_msg_type_number_t	count;

		result = device_read(device, mode, recnum, bytes_wanted,
				     &buf, &count);
		if (result == KERN_SUCCESS) {
			memcpy((char *) data, (const char *) buf, count);
			(void) vm_deallocate(mach_task_self(),
					     (vm_offset_t) buf, count);
			*data_count = count;
		}
	}
	return result;
}
