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
 * block_server.c — Modular block device server: main and PCI scan
 *
 * This is the entry point for the unified block device server.
 * It scans the PCI bus, matches devices to registered driver modules,
 * probes hardware, discovers partitions, and enters the message loop.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/bootstrap.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <mach/mig_errors.h>
#include <sa_mach.h>
#include <device/device.h>
#include <device/device_types.h>
#include <servers/netname.h>
#include <stdio.h>
#include "block_server.h"
#include "device_master.h"
#include "device_server.h"
#include "ahci_batch_server.h"
#include "dl_loader.h"

/* ================================================================
 * Global state
 * ================================================================ */

mach_port_t	host_port;
mach_port_t	device_port;
mach_port_t	master_device;
mach_port_t	security_port;
mach_port_t	root_ledger_wired;
mach_port_t	root_ledger_paged;
mach_port_t	port_set;

struct blk_controller	controllers[MAX_CONTROLLERS];
int			n_controllers;

struct blk_partition	partitions[MAX_PARTITIONS];
int			n_partitions;

/* ================================================================
 * Module table — dynamically loaded driver modules
 *
 * Populated at boot by blk_dl_load_class("block"), which fetches every
 * .so in /mach_servers/modules/block/ from the bootstrap over MIG and
 * dlopens each one.  NULL-terminated so the PCI scan loop can iterate
 * without a separate count.
 * ================================================================ */

#define MAX_MODULES	16

static const struct block_driver_ops *modules[MAX_MODULES + 1];
static int n_modules;

/* ================================================================
 * PCI configuration registers (common across all modules)
 * ================================================================ */

#define PCI_VENDOR_ID		0x00
#define PCI_CLASS_REV		0x08

/* Forward declaration */
static void pci_probe_module(const struct block_driver_ops *ops,
			     unsigned int bus, unsigned int slot,
			     unsigned int func);

/* ================================================================
 * PCI scan — find all block devices and match to modules
 * ================================================================ */

static void
pci_scan_all(void)
{
	unsigned int bus, slot, func;
	unsigned int vendor_device, class_rev;
	kern_return_t kr;
	int m;

	printf("blk: scanning PCI bus...\n");

	for (bus = 0; bus < 256; bus++) {
		for (slot = 0; slot < 32; slot++) {
			for (func = 0; func < 8; func++) {
				kr = device_pci_config_read(master_device,
					bus, slot, func, PCI_VENDOR_ID,
					&vendor_device);
				if (kr != KERN_SUCCESS)
					continue;
				if (vendor_device == 0xFFFFFFFF ||
				    vendor_device == 0)
					continue;

				kr = device_pci_config_read(master_device,
					bus, slot, func, PCI_CLASS_REV,
					&class_rev);
				if (kr != KERN_SUCCESS)
					class_rev = 0;

				/* Try each module */
				for (m = 0; modules[m] != NULL; m++) {
					const struct block_driver_ops *ops =
						modules[m];

					if (!ops->match(vendor_device,
							class_rev))
						continue;

					if (n_controllers >= MAX_CONTROLLERS) {
						printf("blk: too many "
						       "controllers\n");
						return;
					}

					printf("blk: PCI %u:%u.%u matched "
					       "by %s\n",
					       bus, slot, func, ops->name);

					pci_probe_module(ops, bus, slot, func);
					break;
				}
			}
		}
	}

	printf("blk: scan complete, %d controller(s) found\n",
	       n_controllers);
}

/*
 * Probe a matched PCI device: allocate IRQ port, call module probe,
 * enumerate disks, parse MBR for each disk.
 */
static void
pci_probe_module(const struct block_driver_ops *ops,
		 unsigned int bus, unsigned int slot, unsigned int func)
{
	struct blk_controller *ctrl = &controllers[n_controllers];
	kern_return_t kr;
	mach_port_t irq;
	void *priv;
	int nd, d;

	/* Allocate per-controller IRQ port */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &irq);
	if (kr != KERN_SUCCESS) {
		printf("blk: IRQ port alloc failed for %s\n", ops->name);
		return;
	}
	kr = mach_port_insert_right(mach_task_self(), irq, irq,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("blk: IRQ port right failed for %s\n", ops->name);
		return;
	}
	mach_port_move_member(mach_task_self(), irq, port_set);

	/* Call module probe */
	if (ops->probe(bus, slot, func, master_device, irq, &priv) < 0) {
		printf("blk: %s probe failed at PCI %u:%u.%u\n",
		       ops->name, bus, slot, func);
		return;
	}

	ctrl->ops = ops;
	ctrl->priv = priv;
	ctrl->pci_bus = bus;
	ctrl->pci_slot = slot;
	ctrl->pci_func = func;
	ctrl->irq_port = irq;

	/* Enumerate disks */
	nd = ops->get_disks(priv, ctrl->disks, MAX_DISKS_PER_CTRL);
	if (nd < 0)
		nd = 0;
	ctrl->n_disks = nd;

	printf("blk: %s at PCI %u:%u.%u — %d disk(s)\n",
	       ops->name, bus, slot, func, nd);

	n_controllers++;

	/* Parse MBR on each disk */
	for (d = 0; d < nd; d++)
		blk_read_mbr(ctrl, d, ops->name);
}

/* ================================================================
 * Find controller by IRQ port (for demux)
 * ================================================================ */

static struct blk_controller *
find_controller_by_irq(mach_port_t local_port)
{
	int i;
	for (i = 0; i < n_controllers; i++) {
		if (controllers[i].irq_port == local_port)
			return &controllers[i];
	}
	return NULL;
}

/* ================================================================
 * Combined demux: device RPC + batch RPC + IRQ notifications
 * ================================================================ */

static boolean_t
blk_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
	/* Try MIG device server demux (ds_device_read/write/etc.) */
	if (device_server(in, out))
		return TRUE;

	/* Try AHCI batch write subsystem */
	if (ahci_batch_server(in, out))
		return TRUE;

	/* IRQ notification */
	if (in->msgh_id >= IRQ_NOTIFY_MSGH_BASE) {
		struct blk_controller *ctrl =
			find_controller_by_irq(in->msgh_local_port);
		if (ctrl && ctrl->ops->irq_handler)
			ctrl->ops->irq_handler(ctrl->priv);
		((mig_reply_error_t *)out)->RetCode = MIG_NO_REPLY;
		((mig_reply_error_t *)out)->Head.msgh_size =
			sizeof(mig_reply_error_t);
		return TRUE;
	}

	return FALSE;
}

/* ================================================================
 * Main entry point
 * ================================================================ */

int
main(int argc, char **argv)
{
	kern_return_t kr;

	kr = bootstrap_ports(bootstrap_port,
			     &host_port, &device_port,
			     &root_ledger_wired, &root_ledger_paged,
			     &security_port);
	if (kr != KERN_SUCCESS)
		_exit(1);

	printf_init(device_port);
	panic_init(host_port);
	master_device = device_port;

	printf("\n=== Block device server (modular) ===\n");

	/* Global port set for IRQ + partition ports */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_PORT_SET, &port_set);
	if (kr != KERN_SUCCESS) {
		printf("blk: port set alloc failed\n");
		return 1;
	}

	/* Initialise dynamic module loader and pull every module in
	 * the "block" class from the bootstrap over MIG. */
	blk_dl_init();
	n_modules = blk_dl_load_class("block", modules, MAX_MODULES);
	modules[n_modules] = NULL;

	if (n_modules == 0) {
		printf("blk: no driver modules loaded, exiting\n");
		return 1;
	}

	/* Scan PCI, probe modules, enumerate disks and partitions */
	pci_scan_all();

	if (n_partitions == 0) {
		printf("blk: no partitions found, exiting\n");
		return 1;
	}

	/* Register partitions (port alloc, PP, netname, port set) */
	blk_register_partitions();

	printf("blk: init complete, %d partition(s), "
	       "entering message loop\n", n_partitions);

	mach_msg_server(blk_demux, 8192, port_set,
			MACH_MSG_OPTION_NONE);

	printf("blk: mach_msg_server exited unexpectedly\n");
	return 1;
}
