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
 * hal_driver_reg.c — driver subscription table.
 *
 * Drivers subscribe to a PCI class (mask + match) and get their port
 * recorded.  When a matching device is added to the registry, the HAL
 * delivers a hal_device_added() notification on the subscribed port
 * (fire-and-forget MIG simpleroutine — see hal_notify.defs).
 */

#include <mach.h>
#include <mach/kern_return.h>
#include <stdio.h>
#include "hal_server.h"
#include "hal_notify.h"

static struct hal_driver_reg regs[HAL_MAX_DRIVER_REGS];

int
hal_driver_reg_add(uint32_t class_mask, uint32_t class_match,
		   mach_port_t driver_port)
{
	int i;
	for (i = 0; i < HAL_MAX_DRIVER_REGS; i++) {
		if (!regs[i].in_use) {
			regs[i].class_mask  = class_mask;
			regs[i].class_match = class_match;
			regs[i].driver_port = driver_port;
			regs[i].in_use      = 1;
			return i;
		}
	}
	printf("hal: driver registration table full\n");
	return -1;
}

static void
notify_one(const struct hal_driver_reg *reg,
	   const struct hal_device_info *dev)
{
	kern_return_t kr;

	kr = hal_device_added(reg->driver_port,
			      dev->bus, dev->slot, dev->func,
			      dev->vendor_device, dev->class_rev,
			      dev->irq, dev->status);
	if (kr != KERN_SUCCESS) {
		printf("hal: notify %u:%u.%u on port 0x%x failed (kr=%d)\n",
		       dev->bus, dev->slot, dev->func,
		       (unsigned int)reg->driver_port, kr);
		return;
	}
	printf("hal: notified %u:%u.%u (vendor=0x%08x class=0x%08x) "
	       "on port 0x%x\n",
	       dev->bus, dev->slot, dev->func,
	       dev->vendor_device, dev->class_rev,
	       (unsigned int)reg->driver_port);
}

void
hal_driver_reg_notify_match(const struct hal_device_info *dev)
{
	int i;

	if (dev == NULL)
		return;

	for (i = 0; i < HAL_MAX_DRIVER_REGS; i++) {
		if (!regs[i].in_use)
			continue;
		if ((dev->class_rev & regs[i].class_mask) != regs[i].class_match)
			continue;
		notify_one(&regs[i], dev);
	}
}

/*
 * Replay the current registry to a newly-registered driver: called from
 * hal_register_driver after the subscription is stored so the driver gets
 * one hal_device_added() per matching device that was already discovered.
 */
void
hal_driver_reg_replay(int reg_slot)
{
	static struct hal_device_info snapshot[HAL_MAX_DEVICES];
	const struct hal_driver_reg *reg;
	int got, i;

	if (reg_slot < 0 || reg_slot >= HAL_MAX_DRIVER_REGS)
		return;
	reg = &regs[reg_slot];
	if (!reg->in_use)
		return;

	got = hal_registry_copy_all(snapshot, HAL_MAX_DEVICES);
	for (i = 0; i < got; i++) {
		if ((snapshot[i].class_rev & reg->class_mask) != reg->class_match)
			continue;
		notify_one(reg, &snapshot[i]);
	}
}
