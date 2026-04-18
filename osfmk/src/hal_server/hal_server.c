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
 * hal_server.c — HAL server entry point + discovery orchestrator.
 *
 * Boot sequence:
 *   1. bootstrap_ports + printf/panic init
 *   2. Allocate the service port, publish it as "hal" in name_server
 *   3. modload_init + modload_load_class("hal") — fetch discovery plug-ins
 *      from the bootstrap module pool (e.g., pci_scan.so)
 *   4. Run each discovery module's scan() into the registry
 *   5. Enter the MIG message loop
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/bootstrap.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <mach/mig_errors.h>
#include <mach/notify.h>
#include <sa_mach.h>
#include <servers/netname.h>
#include <stdio.h>
#include <string.h>
#include "hal_server.h"
#include "hal_server_mig.h"
#include "modload.h"

/* ================================================================
 * Global state
 * ================================================================ */

mach_port_t	host_port;
mach_port_t	device_port;
mach_port_t	master_device;
mach_port_t	security_port;
mach_port_t	root_ledger_wired;
mach_port_t	root_ledger_paged;
mach_port_t	hal_service_port;

/* ================================================================
 * Discovery modules loaded from /mach_servers/modules/hal/
 * ================================================================ */

#define HAL_MAX_DISCOVERY_MODULES	4

static const struct hal_discovery_ops *discovery[HAL_MAX_DISCOVERY_MODULES + 1];
static int n_discovery;

/* ================================================================
 * Scratch buffer for per-module scan results
 * ================================================================ */

static struct hal_device_info scan_buf[HAL_MAX_DEVICES];

/* ================================================================
 * Discovery pass — run every loaded module, merge into the registry
 * ================================================================ */

static void
run_discovery(void)
{
	int m;

	for (m = 0; m < n_discovery; m++) {
		const struct hal_discovery_ops *ops = discovery[m];
		int n, i;

		if (ops->init != NULL && ops->init(master_device) < 0) {
			printf("hal: module %s init failed\n", ops->name);
			continue;
		}

		n = ops->scan(scan_buf, HAL_MAX_DEVICES);
		if (n < 0) {
			printf("hal: module %s scan failed\n", ops->name);
			continue;
		}

		printf("hal: module %s returned %d device(s)\n", ops->name, n);

		for (i = 0; i < n; i++) {
			if (hal_registry_add(&scan_buf[i]) < 0)
				continue;
			hal_driver_reg_notify_match(&scan_buf[i]);
		}
	}
}

/* ================================================================
 * Debug dump of the populated registry
 * ================================================================ */

static void
dump_registry(void)
{
	static struct hal_device_info snapshot[HAL_MAX_DEVICES];
	int got, i;

	got = hal_registry_copy_all(snapshot, HAL_MAX_DEVICES);
	printf("hal: registry has %d device(s):\n", got);

	for (i = 0; i < got; i++) {
		printf("hal:   %02u:%02u.%u  vendor=0x%08x  "
		       "class=0x%08x  irq=%u  status=%u\n",
		       snapshot[i].bus, snapshot[i].slot, snapshot[i].func,
		       snapshot[i].vendor_device, snapshot[i].class_rev,
		       snapshot[i].irq, snapshot[i].status);
	}
}

/* ================================================================
 * MIG demux
 * ================================================================ */

static boolean_t
hal_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
	if (in->msgh_id == MACH_NOTIFY_DEAD_NAME) {
		const mach_dead_name_notification_t *n =
			(const mach_dead_name_notification_t *)in;
		hal_driver_reg_handle_dead_name(n->not_port);
		((mig_reply_error_t *)out)->RetCode = MIG_NO_REPLY;
		((mig_reply_error_t *)out)->Head.msgh_size =
			sizeof(mig_reply_error_t);
		return TRUE;
	}

	if (hal_server(in, out))
		return TRUE;

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

	printf("\n=== HAL server ===\n");

	/* Allocate the service port. */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &hal_service_port);
	if (kr != KERN_SUCCESS) {
		printf("hal: service port alloc failed\n");
		return 1;
	}
	kr = mach_port_insert_right(mach_task_self(),
		hal_service_port, hal_service_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("hal: service port send-right failed\n");
		return 1;
	}

	/* Publish in name_server so driver servers can find us. */
	kr = netname_check_in(name_server_port, "hal",
			      mach_task_self(), hal_service_port);
	if (kr != KERN_SUCCESS)
		printf("hal: netname_check_in(\"hal\") failed (kr=%d)\n", kr);
	else
		printf("hal: registered service \"hal\" in name_server\n");

	/* Load discovery plug-ins from the bootstrap module pool. */
	modload_init("hal");
	n_discovery = modload_load_class("hal", "_discovery_ops",
					 (void **)discovery,
					 HAL_MAX_DISCOVERY_MODULES);
	discovery[n_discovery] = NULL;

	if (n_discovery == 0) {
		printf("hal: no discovery modules loaded — registry will "
		       "be empty\n");
	}

	run_discovery();
	dump_registry();

	printf("hal: init complete, entering message loop\n");

	mach_msg_server(hal_demux, 8192, hal_service_port,
			MACH_MSG_OPTION_NONE);

	printf("hal: mach_msg_server exited unexpectedly\n");
	return 1;
}
