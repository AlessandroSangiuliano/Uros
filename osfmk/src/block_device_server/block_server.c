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
 * block_server.c — Modular block device server: main + HAL subscription
 *
 * The server no longer walks the PCI bus itself.  At boot it looks up the
 * HAL service in the name server and calls hal_register_driver() with the
 * mass-storage class mask.  The HAL replays the current device registry
 * over the hal_notify subsystem (one hal_device_added() simpleroutine per
 * matching device); each notification lands in hal_device_added() below,
 * which runs the loaded driver modules and probes the matching one.
 *
 * After the initial replay has been drained, partitions are registered
 * with the name server and the server enters its regular message loop
 * (device RPC + ahci_batch + IRQ notifications + hotplug notifications).
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
#include <string.h>
#include "block_server.h"
#include "device_master.h"
#include "device_server.h"
#include "ahci_batch_server.h"
#include "hal.h"
#include "hal_notify_server.h"
#include "cap_revoke_server.h"
#include "modload.h"

extern kern_return_t cap_subscribe_revoke(mach_port_t notify_port);

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

/* HAL subscription */
static mach_port_t	hal_service_port;	/* HAL server send right */
static mach_port_t	hal_driver_port;	/* our receive right */

/* Mass-storage PCI class (byte 3 of class_rev = 0x01).  class_mask picks
 * the high byte, class_match asks for 0x01xxxxxx. */
#define BLK_CLASS_MASK		0xFF000000u
#define BLK_CLASS_MATCH		0x01000000u

/* ================================================================
 * Module table — dynamically loaded driver modules
 * ================================================================ */

#define MAX_MODULES	16

static const struct block_driver_ops *modules[MAX_MODULES + 1];
static int n_modules;

/* Forward declaration */
static void pci_probe_module(const struct block_driver_ops *ops,
			     unsigned int bus, unsigned int slot,
			     unsigned int func);

/* ================================================================
 * HAL notification callback — invoked by hal_notify_server()
 * whenever the HAL delivers a hal_device_added() RPC.
 *
 * For every loaded module, run its match() against the device class;
 * on the first hit, probe the hardware and parse its disks' MBRs.
 * ================================================================ */

kern_return_t
hal_device_added(mach_port_t driver_port,
		 unsigned int bus, unsigned int slot, unsigned int func,
		 unsigned int vendor_device, unsigned int class_rev,
		 unsigned int irq, unsigned int status)
{
	int m;

	(void)driver_port;
	(void)irq;
	(void)status;

	if (n_controllers >= MAX_CONTROLLERS) {
		printf("blk: controller table full, ignoring %u:%u.%u\n",
		       bus, slot, func);
		return KERN_SUCCESS;
	}

	for (m = 0; modules[m] != NULL; m++) {
		const struct block_driver_ops *ops = modules[m];

		if (!ops->match(vendor_device, class_rev))
			continue;

		printf("blk: HAL event %u:%u.%u vendor=0x%08x "
		       "class=0x%08x matched by %s\n",
		       bus, slot, func, vendor_device, class_rev, ops->name);

		pci_probe_module(ops, bus, slot, func);
		break;
	}

	return KERN_SUCCESS;
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

	nd = ops->get_disks(priv, ctrl->disks, MAX_DISKS_PER_CTRL);
	if (nd < 0)
		nd = 0;
	ctrl->n_disks = nd;

	printf("blk: %s at PCI %u:%u.%u — %d disk(s)\n",
	       ops->name, bus, slot, func, nd);

	n_controllers++;

	/*
	 * Stable disk numbering (Issue #184): give every disk a global
	 * monotonically-increasing index that doesn't depend on which
	 * driver (AHCI, virtio-blk, …) discovered it.  Used to publish
	 * the "disk<N><letter>" netname aliases.
	 */
	static int next_stable_disk_idx = 0;
	for (d = 0; d < nd; d++)
		blk_read_mbr(ctrl, d, ops->name, next_stable_disk_idx++);
}

/* ================================================================
 * HAL subscription — look up "hal", register, drain initial replay
 * ================================================================ */

static int
hal_subscribe(void)
{
	kern_return_t kr;

	kr = netname_look_up(name_server_port, "", "hal", &hal_service_port);
	if (kr != KERN_SUCCESS) {
		printf("blk: netname_look_up(\"hal\") failed (kr=%d)\n", kr);
		return -1;
	}

	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &hal_driver_port);
	if (kr != KERN_SUCCESS) {
		printf("blk: driver port alloc failed\n");
		return -1;
	}
	kr = mach_port_insert_right(mach_task_self(),
		hal_driver_port, hal_driver_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("blk: driver port send-right failed\n");
		return -1;
	}

	kr = hal_register_driver(hal_service_port,
				 BLK_CLASS_MASK, BLK_CLASS_MATCH,
				 hal_driver_port);
	if (kr != KERN_SUCCESS) {
		printf("blk: hal_register_driver failed (kr=%d)\n", kr);
		return -1;
	}

	printf("blk: registered with HAL (mask=0x%08x match=0x%08x)\n",
	       BLK_CLASS_MASK, BLK_CLASS_MATCH);
	return 0;
}

/*
 * Drain the initial hal_device_added() burst queued on hal_driver_port
 * by the HAL replay.  Uses MACH_RCV_TIMEOUT; when the queue has been
 * empty for DRAIN_TIMEOUT_MS we consider the initial scan complete.
 *
 * The port is then handed over to port_set so future hotplug events
 * flow into the main message loop together with device I/O and IRQs.
 */
#define DRAIN_TIMEOUT_MS	300
#define DRAIN_BUF_SIZE		512

static void
hal_drain_replay(void)
{
	union {
		mach_msg_header_t	hdr;
		unsigned char		raw[DRAIN_BUF_SIZE];
	} in_msg, out_msg;
	kern_return_t kr;
	int n_events = 0;

	for (;;) {
		in_msg.hdr.msgh_local_port = hal_driver_port;
		in_msg.hdr.msgh_size = sizeof(in_msg);

		kr = mach_msg(&in_msg.hdr,
			      MACH_RCV_MSG | MACH_RCV_TIMEOUT,
			      0, sizeof(in_msg),
			      hal_driver_port,
			      DRAIN_TIMEOUT_MS,
			      MACH_PORT_NULL);
		if (kr == MACH_RCV_TIMED_OUT)
			break;
		if (kr != KERN_SUCCESS) {
			printf("blk: drain mach_msg failed (kr=%d)\n", kr);
			break;
		}

		if (!hal_notify_server(&in_msg.hdr, &out_msg.hdr))
			printf("blk: unexpected msgh_id=%d on drain\n",
			       in_msg.hdr.msgh_id);
		else
			n_events++;
	}

	printf("blk: HAL replay drained — %d event(s), "
	       "%d controller(s), %d partition(s)\n",
	       n_events, n_controllers, n_partitions);

	/* Park the subscription port on the main port set for hotplug. */
	mach_port_move_member(mach_task_self(), hal_driver_port, port_set);
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
 * Combined demux: device RPC + batch RPC + HAL notify + IRQ
 * ================================================================ */

/*
 * Forward decl: handler in block_device.c that reaps a per-client
 * struct blk_handle when its port runs out of senders.
 */
extern boolean_t blk_handle_no_senders(mach_msg_header_t *in,
				       mach_msg_header_t *out);

/*
 * Forward decl: handler in block_device.c that flips the revoked bit
 * on every blk_handle whose cap_id matches.  Returns the number of
 * handles affected (only used for diagnostics).
 */
extern int blk_handles_revoke_by_cap_id(uint64_t cap_id);

/*
 * MIG server-side handler for cap_revoke_notify (Issue #183).  This
 * is invoked by the cap_revoke_server() demux when cap_server fires a
 * notification on our subscription port.
 */
kern_return_t
cap_revoke_notify(mach_port_t notify_port, uint64_t cap_id)
{
	(void)notify_port;
	int n = blk_handles_revoke_by_cap_id(cap_id);
	if (n == 0)
		printf("blk: cap_revoke_notify cap=%llu — no live handle\n",
		       (unsigned long long)cap_id);
	return KERN_SUCCESS;
}

static boolean_t
blk_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
	/*
	 * No-senders notifications are SEND_ONCE messages with a
	 * recognisable msgh_id; check before MIG dispatchers since
	 * none of them know about the notification subsystem id space.
	 */
	if (blk_handle_no_senders(in, out))
		return TRUE;

	if (device_server(in, out))
		return TRUE;

	if (ahci_batch_server(in, out))
		return TRUE;

	if (hal_notify_server(in, out))
		return TRUE;

	if (cap_revoke_server(in, out))
		return TRUE;

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

	printf("\n=== Block device server (HAL-driven) ===\n");

	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_PORT_SET, &port_set);
	if (kr != KERN_SUCCESS) {
		printf("blk: port set alloc failed\n");
		return 1;
	}

	modload_init("blk");
	n_modules = modload_load_class("block", "_module_ops",
				       (void **)modules, MAX_MODULES);
	modules[n_modules] = NULL;

	if (n_modules == 0) {
		printf("blk: no driver modules loaded, exiting\n");
		return 1;
	}

	if (hal_subscribe() < 0)
		return 1;

	hal_drain_replay();

	if (n_partitions == 0) {
		printf("blk: no partitions found, exiting\n");
		return 1;
	}

	blk_register_partitions();

	/*
	 * Issue #183: subscribe to cap_server's revocation back-channel
	 * so we can drop authenticated handles synchronously when a cap
	 * is revoked.  We share the main port_set: cap_revoke_notify
	 * messages flow into blk_demux right alongside device I/O and
	 * IRQs, no separate thread needed.
	 */
	{
		mach_port_t notify_port = MACH_PORT_NULL;
		kern_return_t skr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &notify_port);
		if (skr == KERN_SUCCESS) {
			(void)mach_port_move_member(mach_task_self(),
						    notify_port, port_set);
			mach_port_t send = MACH_PORT_NULL;
			(void)mach_port_insert_right(mach_task_self(),
				notify_port, notify_port,
				MACH_MSG_TYPE_MAKE_SEND);
			send = notify_port;
			skr = cap_subscribe_revoke(send);
			if (skr == KERN_SUCCESS)
				printf("blk: subscribed to cap_revoke_notify "
				       "(port=0x%x)\n", (unsigned)notify_port);
			else
				printf("blk: cap_subscribe_revoke failed "
				       "(kr=%d)\n", (int)skr);
		} else {
			printf("blk: notify port alloc failed (kr=%d)\n",
			       (int)skr);
		}
	}

	printf("blk: init complete, %d partition(s), "
	       "entering message loop\n", n_partitions);

	mach_msg_server(blk_demux, 8192, port_set,
			MACH_MSG_OPTION_NONE);

	printf("blk: mach_msg_server exited unexpectedly\n");
	return 1;
}
