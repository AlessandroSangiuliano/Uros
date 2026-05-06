/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * gpu_server/main.c — Task entry point and MIG dispatch loop.
 *
 * Boot sequence:
 *   1. bootstrap_ports  — fetch host_port, device_port, ledgers, security_port
 *   2. printf_init      — open the kernel console for our own diagnostics
 *      (this is the *bootstrap* console; once gpu_server is up the kernel
 *      VGA console will be retired in #199)
 *   3. allocate the service port + a SEND right
 *   4. publish "gpu" in name_server so clients can find us
 *   5. core init + module discovery (static-list in 0.1.0; libdl in #162)
 *   6. enter mach_msg_server with the MIG demux
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
#include "gpu_server.h"
#include "modload.h"
#include "libcap.h"
#include "cap_revoke_server.h"

/* ================================================================
 * Global ports
 * ================================================================ */

mach_port_t	gpu_host_port;
mach_port_t	gpu_device_port;
mach_port_t	gpu_security_port;
mach_port_t	gpu_root_ledger_wired;
mach_port_t	gpu_root_ledger_paged;
mach_port_t	gpu_service_port;
static mach_port_t gpu_port_set;
static mach_port_t gpu_cap_revoke_port;

/* ================================================================
 * cap_revoke_notify — server-side handler invoked when cap_server
 * fans out a revocation event we subscribed to (#183).
 *
 * In 0.1.0 gpu_server holds no per-client state we need to tear
 * down: gpu_device_open is a no-op handshake and bo_alloc /
 * context_create still return KERN_FAILURE.  We log the event so
 * the wiring is observable; once those ops become real (post-0.2)
 * this is where bo handles / context rings get released by cap_id.
 * ================================================================ */

kern_return_t
cap_revoke_notify(mach_port_t notify_port, uint64_t cap_id)
{
	(void)notify_port;
	printf("gpu_server: cap_revoke_notify cap=%llu (no live state to "
	       "release in 0.1.0)\n", (unsigned long long)cap_id);
	return KERN_SUCCESS;
}

/* ================================================================
 * MIG demux
 *
 * Plain pass-through to the generated dispatcher.  When #197 wires
 * up cap_subscribe_revoke, the dead-name notification handler from
 * hal_server gets ported here too.
 * ================================================================ */

static boolean_t
gpu_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
	if (gpu_server_server(in, out))
		return TRUE;

	if (cap_revoke_server(in, out))
		return TRUE;

	return FALSE;
}

/* ================================================================
 * Service-port bring-up
 * ================================================================ */

static int
publish_service_port(void)
{
	kern_return_t kr;

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_PORT_SET, &gpu_port_set);
	if (kr != KERN_SUCCESS) {
		printf("gpu_server: port set alloc failed (kr=%d)\n", kr);
		return -1;
	}

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &gpu_service_port);
	if (kr != KERN_SUCCESS) {
		printf("gpu_server: service port alloc failed (kr=%d)\n", kr);
		return -1;
	}
	(void)mach_port_move_member(mach_task_self(),
				    gpu_service_port, gpu_port_set);

	kr = mach_port_insert_right(mach_task_self(),
		gpu_service_port, gpu_service_port,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("gpu_server: service port send-right failed (kr=%d)\n", kr);
		return -1;
	}

	kr = netname_check_in(name_server_port, "gpu",
			      mach_task_self(), gpu_service_port);
	if (kr != KERN_SUCCESS) {
		printf("gpu_server: netname_check_in(\"gpu\") failed (kr=%d)\n",
		       kr);
		/* Non-fatal: stand-alone tests still want the dispatch loop. */
	} else {
		printf("gpu_server: registered service \"gpu\" in name_server\n");
	}
	return 0;
}

/* ================================================================
 * cap_server back-channel subscription (#183 / #197)
 *
 * Allocate a notify port, drop it in the main port set so
 * cap_revoke_notify messages flow through gpu_demux alongside
 * gpu_server.defs RPCs, then ask cap_server to fire on it.
 * ================================================================ */

static void
subscribe_to_cap_revoke(void)
{
	kern_return_t kr;
	mach_port_t   send;

	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &gpu_cap_revoke_port);
	if (kr != KERN_SUCCESS) {
		printf("gpu_server: cap_revoke port alloc failed (kr=%d)\n",
		       kr);
		return;
	}
	(void)mach_port_move_member(mach_task_self(),
				    gpu_cap_revoke_port, gpu_port_set);

	kr = mach_port_insert_right(mach_task_self(),
		gpu_cap_revoke_port, gpu_cap_revoke_port,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("gpu_server: cap_revoke send-right failed (kr=%d)\n",
		       kr);
		return;
	}
	send = gpu_cap_revoke_port;

	kr = cap_subscribe_revoke(send);
	if (kr != KERN_SUCCESS) {
		printf("gpu_server: cap_subscribe_revoke failed (kr=%d) — "
		       "revocation back-channel disabled\n", (int)kr);
		return;
	}
	printf("gpu_server: subscribed to cap_revoke_notify (port=0x%x)\n",
	       (unsigned)gpu_cap_revoke_port);
}

/* ================================================================
 * Optional HAL handle for discovery
 *
 * In 0.1.0 the only display is legacy VGA which the vga module
 * (#195) probes without HAL.  Looking up "hal" here is best-effort:
 * if HAL is up, future modules (bochs_dispi, intel_gen, ...) will
 * need it.
 * ================================================================ */

static mach_port_t
lookup_hal_optional(void)
{
	mach_port_t hal_port = MACH_PORT_NULL;
	kern_return_t kr;

	kr = netname_look_up(name_server_port, "", "hal", &hal_port);
	if (kr != KERN_SUCCESS)
		return MACH_PORT_NULL;
	return hal_port;
}

/* ================================================================
 * Main entry point
 * ================================================================ */

int
main(int argc, char **argv)
{
	kern_return_t kr;
	mach_port_t   hal_port;

	(void)argc;
	(void)argv;

	kr = bootstrap_ports(bootstrap_port,
			     &gpu_host_port, &gpu_device_port,
			     &gpu_root_ledger_wired, &gpu_root_ledger_paged,
			     &gpu_security_port);
	if (kr != KERN_SUCCESS)
		_exit(1);

	printf_init(gpu_device_port);
	panic_init(gpu_host_port);

	printf("\n=== gpu_server (skeleton, #194) ===\n");

	if (publish_service_port() < 0)
		return 1;

	if (gpu_core_init() < 0) {
		printf("gpu_server: core init failed\n");
		return 1;
	}

	/* Load back-end modules from /mach_servers/modules/gpu/ via
	 * libmodload.  Same machinery block_device_server (#161) and
	 * hal_server use: each .so exports `<basename>_module_ops` of
	 * type `gpu_module_ops_t` and is dlopen'd into our address
	 * space, with host-side symbols (`gpu_device_port`,
	 * `device_mmio_map`, `printf`, ...) resolved via the
	 * `--export-dynamic` symtab. */
	{
		const gpu_module_ops_t *modules[GPU_MAX_MODULES + 1];
		int n_modules;

		modload_init("gpu");
		n_modules = modload_load_class("gpu", "_module_ops",
					       (void **)modules,
					       GPU_MAX_MODULES);
		modules[n_modules] = NULL;

		hal_port = lookup_hal_optional();
		if (hal_port == MACH_PORT_NULL)
			printf("gpu_server: HAL not available "
			       "(legacy-VGA-only mode)\n");
		else
			printf("gpu_server: HAL port=0x%x\n",
			       (unsigned int)hal_port);

		gpu_core_run_discovery(modules, (unsigned int)n_modules,
				       hal_port);
	}

	/* Start the text rendering worker.  Must come AFTER discovery
	 * so the worker sees an attached module on its first dequeue;
	 * before mach_msg_server so any startup chatter from
	 * bootstrap_completed routes through the queue. */
	if (gpu_text_render_init() < 0)
		printf("gpu_server: text_render init failed — text path "
		       "will run inline on the dispatch thread\n");

	subscribe_to_cap_revoke();

	bootstrap_completed(bootstrap_port, mach_task_self());
	printf("gpu_server: init complete, entering message loop\n");

	mach_msg_server(gpu_demux, 8192, gpu_port_set,
			MACH_MSG_OPTION_NONE);

	printf("gpu_server: mach_msg_server exited unexpectedly\n");
	return 1;
}
