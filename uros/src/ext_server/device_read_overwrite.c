/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * device_read_overwrite() for ext2_server.
 *
 * libsa_fs calls device_read_overwrite() which expects to copy data into a
 * caller-provided buffer.  The standard libmach ms_device_read_overwrite.c
 * tries the kernel syscall first, then falls back to mig_device_read() — but
 * those renamed MIG stubs don't exist in our CMake build.
 *
 * Since ext2_server always talks to a userspace ahci_driver via IPC, we
 * skip the syscall entirely and call device_read() (the MIG stub) directly,
 * then memcpy the OOL data into the caller's buffer.
 */

#include <mach.h>
#include <mach/message.h>
#include <device/device.h>

kern_return_t
device_read_overwrite(
	mach_port_t		device,
	dev_mode_t		mode,
	recnum_t		recnum,
	io_buf_len_t		bytes_wanted,
	vm_address_t		data,
	mach_msg_type_number_t	*data_count)
{
	kern_return_t		result;
	io_buf_ptr_t		buf;
	mach_msg_type_number_t	count;

	result = device_read(device, mode, recnum, bytes_wanted, &buf, &count);
	if (result == KERN_SUCCESS) {
		memcpy((void *)data, (const void *)buf, count);
		(void)vm_deallocate(mach_task_self(),
				    (vm_offset_t)buf, count);
		*data_count = count;
	}
	return result;
}
