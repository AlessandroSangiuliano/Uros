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
 * recorded; the match notification is a stub today (logs only) and will
 * become an async Mach message in the hotplug phase.
 */

#include <stdio.h>
#include "hal_server.h"

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

		/*
		 * Phase 1: log only — the asynchronous match-notification
		 * message is part of the hotplug work (separate issue).
		 */
		printf("hal: match — device %u:%u.%u (vendor=0x%08x, "
		       "class=0x%08x) would be notified on port 0x%x\n",
		       dev->bus, dev->slot, dev->func,
		       dev->vendor_device, dev->class_rev,
		       (unsigned int)regs[i].driver_port);
	}
}
