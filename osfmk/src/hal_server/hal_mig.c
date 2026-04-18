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
 * hal_mig.c — MIG server-side implementations for hal.defs.
 *
 * Routine names follow the `hal_` server prefix declared in hal.defs,
 * so MIG wires the demux in hal_server.c straight to these functions.
 */

#include <mach.h>
#include <mach/kern_return.h>
#include <string.h>
#include "hal_server.h"

kern_return_t
hal_list_devices(mach_port_t hal_port,
		 vm_offset_t *devices, mach_msg_type_number_t *devices_count,
		 unsigned int *n_devices)
{
	int n = hal_registry_count();
	vm_size_t bytes;
	vm_offset_t buf;
	kern_return_t kr;

	(void)hal_port;

	bytes = (vm_size_t)n * sizeof(struct hal_device_info);
	if (bytes == 0) {
		*devices = 0;
		*devices_count = 0;
		*n_devices = 0;
		return KERN_SUCCESS;
	}

	buf = 0;
	kr = vm_allocate(mach_task_self(), &buf, bytes, TRUE);
	if (kr != KERN_SUCCESS)
		return kr;

	(void)hal_registry_copy_all((struct hal_device_info *)buf,
				    (unsigned int)n);

	*devices = buf;
	*devices_count = (mach_msg_type_number_t)bytes;
	*n_devices = (unsigned int)n;
	return KERN_SUCCESS;
}

kern_return_t
hal_get_device_info(mach_port_t hal_port,
		    unsigned int bus, unsigned int slot, unsigned int func,
		    unsigned int *vendor_device, unsigned int *class_rev,
		    unsigned int *irq, unsigned int *status)
{
	const struct hal_device_info *d;

	(void)hal_port;

	d = hal_registry_get(bus, slot, func);
	if (d == NULL)
		return KERN_INVALID_ARGUMENT;

	*vendor_device = d->vendor_device;
	*class_rev     = d->class_rev;
	*irq           = d->irq;
	*status        = d->status;
	return KERN_SUCCESS;
}

kern_return_t
hal_register_driver(mach_port_t hal_port,
		    unsigned int class_mask, unsigned int class_match,
		    mach_port_t driver_port)
{
	int slot;

	(void)hal_port;

	slot = hal_driver_reg_add(class_mask, class_match, driver_port);
	if (slot < 0)
		return KERN_RESOURCE_SHORTAGE;

	/* Replay existing registry to the new subscriber. */
	hal_driver_reg_replay(slot);
	return KERN_SUCCESS;
}
