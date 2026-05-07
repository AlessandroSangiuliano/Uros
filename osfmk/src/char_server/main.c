/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * char_server/main.c — task entry point + MIG dispatch loop.
 *
 * Mirror gpu_server/main.c.  Boot sequence:
 *   1. bootstrap_ports + printf_init
 *   2. publish service port "char" via netname
 *   3. mirror printf to gpu_server (libgpu_console — char_server boots
 *      after gpu_server in bootstrap.conf so sync init is fine)
 *   4. modload_load_class("char", "_module_ops", ...)
 *   5. discovery → modules' attach()
 *   6. cap_subscribe_revoke
 *   7. mach_msg_server on the port set
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
#include "char_server.h"
#include "modload.h"
#include "libcap.h"
#include "gpu_console.h"
#include "cap_revoke_server.h"

mach_port_t	char_host_port;
mach_port_t	char_device_port;
mach_port_t	char_security_port;
mach_port_t	char_root_ledger_wired;
mach_port_t	char_root_ledger_paged;
mach_port_t	char_service_port;
static mach_port_t char_port_set;
static mach_port_t char_cap_revoke_port;

/* ============================================================
 * cap_revoke_notify handler.
 *
 * #205 holds no per-client state to release: kbd subscriptions
 * accumulate inside ps2.so (which only learns about revoke via the
 * notify port going dead, handled by no-senders notif in #206).
 * This logger is the placeholder until the equivalent of #202 lands
 * for char_server.
 * ============================================================ */

kern_return_t
cap_revoke_notify(mach_port_t notify_port, uint64_t cap_id)
{
	(void)notify_port;
	printf("char_server: cap_revoke_notify cap=%llu (no live state to "
	       "release in #205)\n", (unsigned long long)cap_id);
	return KERN_SUCCESS;
}

/* ============================================================
 * MIG demux — both subsystems on the same port set.
 * ============================================================ */

static boolean_t
char_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
	if (char_server_server(in, out))
		return TRUE;
	if (cap_revoke_server(in, out))
		return TRUE;
	return FALSE;
}

/* ============================================================
 * Service-port + cap-revoke-port bring-up.
 * ============================================================ */

static int
publish_service_port(void)
{
	kern_return_t kr;

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_PORT_SET, &char_port_set);
	if (kr != KERN_SUCCESS) {
		printf("char_server: port set alloc failed (kr=%d)\n", kr);
		return -1;
	}

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &char_service_port);
	if (kr != KERN_SUCCESS) {
		printf("char_server: service port alloc failed (kr=%d)\n", kr);
		return -1;
	}
	(void)mach_port_move_member(mach_task_self(),
				    char_service_port, char_port_set);

	kr = mach_port_insert_right(mach_task_self(),
		char_service_port, char_service_port,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("char_server: service port send-right failed (kr=%d)\n",
		       kr);
		return -1;
	}

	kr = netname_check_in(name_server_port, "char",
			      mach_task_self(), char_service_port);
	if (kr != KERN_SUCCESS)
		printf("char_server: netname_check_in(\"char\") failed "
		       "(kr=%d)\n", kr);
	else
		printf("char_server: registered service \"char\" in "
		       "name_server\n");
	return 0;
}

static void
subscribe_to_cap_revoke(void)
{
	kern_return_t kr;
	mach_port_t   send;

	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &char_cap_revoke_port);
	if (kr != KERN_SUCCESS) {
		printf("char_server: cap_revoke port alloc failed (kr=%d)\n",
		       kr);
		return;
	}
	(void)mach_port_move_member(mach_task_self(),
				    char_cap_revoke_port, char_port_set);
	kr = mach_port_insert_right(mach_task_self(),
		char_cap_revoke_port, char_cap_revoke_port,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("char_server: cap_revoke send-right failed (kr=%d)\n",
		       kr);
		return;
	}
	send = char_cap_revoke_port;

	kr = cap_subscribe_revoke(send);
	if (kr != KERN_SUCCESS) {
		printf("char_server: cap_subscribe_revoke failed (kr=%d) — "
		       "back-channel disabled\n", (int)kr);
		return;
	}
	printf("char_server: subscribed to cap_revoke_notify (port=0x%x)\n",
	       (unsigned)char_cap_revoke_port);
}

static mach_port_t
lookup_hal_optional(void)
{
	mach_port_t hal_port = MACH_PORT_NULL;
	(void)netname_look_up(name_server_port, "", "hal", &hal_port);
	return hal_port;
}

/* ============================================================
 * Main
 * ============================================================ */

int
main(int argc, char **argv)
{
	kern_return_t kr;
	mach_port_t   hal_port;

	(void)argc;
	(void)argv;

	kr = bootstrap_ports(bootstrap_port,
			     &char_host_port, &char_device_port,
			     &char_root_ledger_wired, &char_root_ledger_paged,
			     &char_security_port);
	if (kr != KERN_SUCCESS)
		_exit(1);

	printf_init(char_device_port);
	panic_init(char_host_port);

	printf("\n=== char_server (skeleton, #205) ===\n");

	if (publish_service_port() < 0)
		return 1;

	/* char_server boots after gpu_server in bootstrap.conf, so the
	 * sync mirror init succeeds.  Tag every printf with [char] so
	 * keyboard/tty diagnostics are easy to spot on screen. */
	(void)gpu_console_init("char");

	if (char_core_init() < 0) {
		printf("char_server: core init failed\n");
		return 1;
	}

	{
		const char_module_ops_t *modules[CHAR_MAX_MODULES + 1];
		int n_modules;

		modload_init("char");
		n_modules = modload_load_class("char", "_module_ops",
					       (void **)modules,
					       CHAR_MAX_MODULES);
		modules[n_modules] = NULL;

		hal_port = lookup_hal_optional();
		if (hal_port == MACH_PORT_NULL)
			printf("char_server: HAL not available "
			       "(legacy-only mode)\n");

		char_core_run_discovery(modules, (unsigned int)n_modules,
					hal_port);
	}

	subscribe_to_cap_revoke();

	bootstrap_completed(bootstrap_port, mach_task_self());
	printf("char_server: init complete, entering message loop\n");

	mach_msg_server(char_demux, 8192, char_port_set,
			MACH_MSG_OPTION_NONE);

	printf("char_server: mach_msg_server exited unexpectedly\n");
	return 1;
}
