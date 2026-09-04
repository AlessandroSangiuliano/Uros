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
#include <mach/notify.h>
#include <stdio.h>
#include <string.h>
#include "hal_server.h"
#include "device_master.h"	/* #513: ask the kernel who holds it */

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
		    unsigned int *irq)
{
	const struct hal_device_info *d;

	(void)hal_port;

	d = hal_registry_get(bus, slot, func);
	if (d == NULL)
		return KERN_INVALID_ARGUMENT;

	*vendor_device = d->vendor_device;
	*class_rev     = d->class_rev;
	*irq           = d->irq;
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

	/*
	 * Ask the kernel to tell us when driver_port becomes a dead
	 * name (driver task exits or drops its receive right); the
	 * handler in hal_driver_reg_handle_dead_name will release the
	 * subscription slot so we stop sending notifications to it.
	 */
	{
		mach_port_t prev = MACH_PORT_NULL;
		kern_return_t kr;

		kr = mach_port_request_notification(mach_task_self(),
			driver_port, MACH_NOTIFY_DEAD_NAME, 0,
			hal_service_port, MACH_MSG_TYPE_MAKE_SEND_ONCE,
			&prev);
		if (kr != KERN_SUCCESS)
			printf("hal: request_notification(DEAD_NAME) on "
			       "port 0x%x failed (kr=%d)\n",
			       (unsigned int)driver_port, kr);
		if (prev != MACH_PORT_NULL)
			mach_port_deallocate(mach_task_self(), prev);
	}

	hal_driver_reg_replay(slot);
	return KERN_SUCCESS;
}

/*
 * #427 — the device's regions, decoded, for the one device asked about.
 *
 * ⚠️ A device with no regions returns success and a null buffer, not an
 * error: "this device has no BARs" is an answer, and the host bridge and the
 * ISA bridge on the machine this was measured on give exactly that.  Making
 * it a failure would have every driver treat a true statement as a fault.
 */
kern_return_t
hal_get_device_bars(mach_port_t hal_port,
		    unsigned int bus, unsigned int slot, unsigned int func,
		    vm_offset_t *bars, mach_msg_type_number_t *bars_count,
		    unsigned int *n_bars)
{
	const struct hal_device_info *d;
	vm_size_t bytes;
	vm_offset_t buf;
	kern_return_t kr;

	(void)hal_port;

	d = hal_registry_get(bus, slot, func);
	if (d == NULL)
		return KERN_INVALID_ARGUMENT;

	bytes = (vm_size_t)d->n_bars * sizeof(struct pci_bar_region);
	if (bytes == 0) {
		*bars = 0;
		*bars_count = 0;
		*n_bars = 0;
		return KERN_SUCCESS;
	}

	buf = 0;
	kr = vm_allocate(mach_task_self(), &buf, bytes, TRUE);
	if (kr != KERN_SUCCESS)
		return kr;

	memcpy((void *)buf, d->bars, (size_t)bytes);

	*bars = buf;
	*bars_count = (mach_msg_type_number_t)bytes;
	*n_bars = d->n_bars;
	return KERN_SUCCESS;
}

kern_return_t
hal_rescan(mach_port_t hal_port)
{
	(void)hal_port;
	hal_run_discovery();
	return KERN_SUCCESS;
}

/*
 * ── A driver says what happened, and the HAL checks it (#513) ────────
 *
 * 🔴 THE ONE FACT THE HAL CANNOT GET ANYWHERE ELSE is whether the probe
 * worked.  The kernel knows who HOLDS a device and the bus knows what is
 * plugged into it; whether a controller came up is a fact about the driver, and
 * before this routine there was nowhere for it to go.
 *
 * 🔑 BELIEVED ONLY AS FAR AS IT CAN BE CHECKED, and the two halves are checked
 * against different parties:
 *
 *	the REPORTER -- against the HAL's own subscription table, so a report
 *	from a port this HAL never handed a device to is refused;
 *
 *	the CLAIM -- against the KERNEL, because device_claim() is a
 *	transaction with it and nobody else's record of it is authoritative.
 *
 * Without the second, this registry would be a second claim table free to
 * disagree with the one that actually gates DMA -- the failure #427 removed a
 * field for, moved one server along.
 *
 * ⚠️ device_dma_owned answers "is somebody OTHER THAN THE CALLER driving this",
 * and that is the right question here for a reason worth keeping: the HAL is
 * not a driver and never claims anything, so for it the answer is exactly "does
 * a driver hold this device".  It is the same call that answered nothing useful
 * in block_server.c -- where the caller WAS the driver -- and the difference is
 * who is asking, not what is asked.
 */
kern_return_t
hal_report_probe(mach_port_t hal_port,
		 unsigned int bus, unsigned int slot, unsigned int func,
		 mach_port_t driver_port, unsigned int outcome)
{
	const struct hal_device_info	*d;
	natural_t			by_other = 0;
	natural_t			bdf;

	(void)hal_port;

	/*
	 * 🔴 THE RIGHT IS GIVEN BACK ONLY ON THE PATH THAT RETURNS SUCCESS, and
	 * that is a rule about mach_msg_server rather than about this routine.
	 * When a demux answers anything but KERN_SUCCESS, the loop in
	 * libmach/mach_msg_server.c calls mach_msg_destroy() on the whole
	 * REQUEST -- every port right in it included.  A routine that also
	 * released one would be the second release of the same right.
	 *
	 * 🔥 Which is not theory: it took the subscription with it.  The right
	 * arriving here coalesces into the name the driver registered with, so
	 * one over-release destroyed that name -- and the next report from the
	 * same driver arrived under a fresh one and was refused as a stranger.
	 * One controller worked, the second did not, and nothing said why.
	 */

	if (!hal_driver_reg_known(driver_port)) {
		printf("hal: probe report for %u:%u.%u REFUSED — port 0x%x is "
		       "not a registered driver\n", bus, slot, func,
		       (unsigned int)driver_port);
		return KERN_NO_ACCESS;
	}

	d = hal_registry_get(bus, slot, func);
	if (d == NULL) {
		printf("hal: probe report for %u:%u.%u REFUSED — no such "
		       "device in the registry\n", bus, slot, func);
		return KERN_INVALID_ARGUMENT;
	}

	if (outcome != HAL_DEV_BOUND && outcome != HAL_DEV_PROBE_FAILED)
		return KERN_INVALID_ARGUMENT;

	if (outcome == HAL_DEV_BOUND) {
		bdf = (natural_t)((bus << 8) | (slot << 3) | func);

		if (device_dma_owned(master_device, bdf, &by_other)
		    != KERN_SUCCESS || by_other == 0) {
			printf("hal: %u:%u.%u reported BOUND and the kernel "
			       "does not have it claimed — not recorded\n",
			       bus, slot, func);
			return KERN_FAILURE;
		}
	}

	(void) hal_registry_set_driver_state(bus, slot, func,
					     (uint32_t)outcome, driver_port);

	printf("hal: %u:%u.%u is %s\n", bus, slot, func,
	       outcome == HAL_DEV_BOUND
	       ? "bound to a driver, and the kernel agrees it is held"
	       : "unclaimed again — its driver probed and failed");

	/*
	 * ⚠️ And on THIS path it must be released, because nothing else will.
	 * The subscription table already holds the right that matters; a driver
	 * reporting once per controller per boot would otherwise leave one
	 * behind every time.
	 */
	(void) mach_port_deallocate(mach_task_self(), driver_port);
	return KERN_SUCCESS;
}

/*
 * ── Which devices have drivers (#513) ────────────────────────────────
 *
 * ⚠️ A device that is present and unclaimed answers HAL_DEV_UNCLAIMED and
 * SUCCESS.  "Nothing has bound it" is an answer, and the servers that will ask
 * this -- to decide whether re-probing a device is safe -- need to tell it apart
 * from "no such device", which is the error.
 */
kern_return_t
hal_get_device_state(mach_port_t hal_port,
		     unsigned int bus, unsigned int slot, unsigned int func,
		     unsigned int *state)
{
	uint32_t s = HAL_DEV_UNCLAIMED;

	(void)hal_port;

	if (hal_registry_driver_state(bus, slot, func, &s) < 0)
		return KERN_INVALID_ARGUMENT;

	*state = (unsigned int)s;
	return KERN_SUCCESS;
}
