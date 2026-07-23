/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * gpustat — userspace probe for gpu_query_stats (#203).
 *
 * Single-shot read of gpu_server's diagnostic counters and a one-line
 * summary on the kernel console.  Run from bootstrap.conf after
 * gpu_server / cap_server come up — useful for end-of-boot smoke tests
 * ("did anything print on VGA, and did we drop anything?") and for
 * future regression harnesses that grep the serial log for the
 * gpustat: line.
 *
 * A `--watch INTERVAL` mode (poll + print deltas) is the obvious next
 * step but is left out of v1 — without a real terminal there is no
 * good place to render a continuously-updating dashboard.
 */

#include <stdio.h>
#include <string.h>

#include <mach.h>
#include <mach/bootstrap.h>
#include <mach/mach_traps.h>
#include <mach/thread_switch.h>
#include <mach/cap_types.h>
#include <mach/gpu_stats.h>

#include <gpu/gpu_types.h>		/* GPU_CAP_RESOURCE_TYPE / GPU_CAP_DEV_ADMIN */
#include <servers/netname.h>
#include <libcap.h>

#include "gpu_server.h"		/* MIG user stub: gpu_query_stats */

extern kern_return_t bootstrap_ports(mach_port_t bootstrap,
				     mach_port_t *host_port,
				     mach_port_t *device_port,
				     mach_port_t *root_ledger_wired,
				     mach_port_t *root_ledger_paged,
				     mach_port_t *security_port);

extern void printf_init(mach_port_t device_port);

static mach_port_t
wait_for(const char *name)
{
	mach_port_t port = MACH_PORT_NULL;
	for (;;) {
		kern_return_t kr = netname_look_up(name_server_port, "",
						   (char *)name, &port);
		if (kr == KERN_SUCCESS)
			return port;
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 50);
	}
}

int
main(int argc, char **argv)
{
	mach_port_t host_port, device_port, security_port;
	mach_port_t root_ledger_wired, root_ledger_paged;
	mach_port_t gpu_port;
	struct uros_cap cap;
	char blob[GPU_STATS_BLOB_MAX];
	mach_msg_type_number_t blob_len = (mach_msg_type_number_t)sizeof(blob);
	struct gpu_stats stats;
	kern_return_t kr;

	(void)argc; (void)argv;

	kr = bootstrap_ports(bootstrap_port,
			     &host_port, &device_port,
			     &root_ledger_wired, &root_ledger_paged,
			     &security_port);
	if (kr != KERN_SUCCESS)
		return 1;

	printf_init(device_port);

	/* Wait until both cap_server and gpu_server are registered.
	 * bootstrap.conf launch ordering doesn't guarantee we run last,
	 * and the cap_request below would fail noisily otherwise. */
	(void)wait_for("cap_server");
	gpu_port = wait_for("gpu");

	kr = cap_request(GPU_CAP_RESOURCE_TYPE,
			 /*resource_id*/ 0,
			 GPU_CAP_DEV_ADMIN,
			 /*lifetime_ms*/ 0,
			 &cap);
	if (kr != KERN_SUCCESS) {
		printf("gpustat: cap_request(DEV_ADMIN) failed (kr=%d)\n",
		       (int)kr);
		return 1;
	}

	kr = gpu_query_stats(gpu_port,
			     (char *)&cap, (mach_msg_type_number_t)sizeof(cap),
			     blob, &blob_len);
	if (kr != KERN_SUCCESS) {
		printf("gpustat: gpu_query_stats failed (kr=%d)\n", (int)kr);
		return 1;
	}
	if (blob_len < sizeof(stats)) {
		printf("gpustat: short reply (%u < %u)\n",
		       (unsigned)blob_len, (unsigned)sizeof(stats));
		return 1;
	}

	memcpy(&stats, blob, sizeof(stats));

	printf("gpustat: v=%u devs=%u chunks=%llu drops=%llu scrolls=%llu\n",
	       (unsigned)stats.version,
	       (unsigned)stats.devices_attached,
	       (unsigned long long)stats.text_render_chunks_processed,
	       (unsigned long long)stats.text_render_drops,
	       (unsigned long long)stats.scroll_count);

	return 0;
}
