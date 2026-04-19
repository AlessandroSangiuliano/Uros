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
 * pci_scan.c — PCI discovery module for the HAL server.
 *
 * Brute-force enumeration of bus 0..255, slot 0..31, func 0..7 via
 * device_pci_config_read.  Each valid device is written into the
 * caller-supplied array as a fully populated hal_device_info record
 * (BARs and IRQ line included).
 *
 * This module is pure discovery — it reads PCI configuration space
 * but never programs BARs or writes command registers.  Ownership of
 * the device is later handed off to a driver server, which will enable
 * memory/bus-master and map the BARs itself.
 */

#include <mach.h>
#include <device/device.h>
#include <stdio.h>
#include <string.h>
#include "device_master.h"
#include "hal_server.h"

/* PCI configuration-space offsets */
#define PCI_VENDOR_ID		0x00
#define PCI_CLASS_REV		0x08
#define PCI_BAR0		0x10
#define PCI_INTERRUPT_LINE	0x3C

static mach_port_t	pci_master_device;

static int
pci_scan_init(mach_port_t master_device)
{
	pci_master_device = master_device;
	return 0;
}

/*
 * Read one PCI device's configuration into `dev` (vendor, class, BARs,
 * IRQ).  Returns 1 if a device is present, 0 if the slot is empty,
 * and -1 on a kernel RPC error.
 */
static int
read_pci_device(unsigned int bus, unsigned int slot, unsigned int func,
		struct hal_device_info *dev)
{
	unsigned int vendor_device, class_rev, irq_reg, bar;
	kern_return_t kr;
	int i;

	kr = device_pci_config_read(pci_master_device,
				    bus, slot, func,
				    PCI_VENDOR_ID, &vendor_device);
	if (kr != KERN_SUCCESS)
		return -1;
	if (vendor_device == 0xFFFFFFFFu || vendor_device == 0)
		return 0;

	memset(dev, 0, sizeof(*dev));
	dev->bus = bus;
	dev->slot = slot;
	dev->func = func;
	dev->vendor_device = vendor_device;
	dev->status = HAL_DEV_UNBOUND;

	kr = device_pci_config_read(pci_master_device,
				    bus, slot, func,
				    PCI_CLASS_REV, &class_rev);
	dev->class_rev = (kr == KERN_SUCCESS) ? class_rev : 0;

	kr = device_pci_config_read(pci_master_device,
				    bus, slot, func,
				    PCI_INTERRUPT_LINE, &irq_reg);
	dev->irq = (kr == KERN_SUCCESS) ? (irq_reg & 0xFFu) : 0;

	for (i = 0; i < HAL_MAX_BARS; i++) {
		kr = device_pci_config_read(pci_master_device,
					    bus, slot, func,
					    PCI_BAR0 + i * 4u, &bar);
		dev->bars[i] = (kr == KERN_SUCCESS) ? bar : 0;
	}

	return 1;
}

static int
pci_scan_scan(struct hal_device_info *out, unsigned int max)
{
	unsigned int bus, slot, func;
	unsigned int count = 0;

	if (pci_master_device == MACH_PORT_NULL) {
		printf("pci_scan: init not called\n");
		return -1;
	}

	for (bus = 0; bus < 256; bus++) {
		for (slot = 0; slot < 32; slot++) {
			for (func = 0; func < 8; func++) {
				int r;

				if (count >= max)
					return (int)count;

				r = read_pci_device(bus, slot, func,
						    &out[count]);
				if (r < 0)
					return -1;
				if (r == 0)
					continue;

				count++;
			}
		}
	}

	return (int)count;
}

/*
 * Symbol looked up by libmodload after loading "pci_scan.so":
 *   basename("pci_scan.so") + "_discovery_ops" → "pci_scan_discovery_ops"
 */
const struct hal_discovery_ops pci_scan_discovery_ops = {
	.name = "pci_scan",
	.init = pci_scan_init,
	.scan = pci_scan_scan,
};
