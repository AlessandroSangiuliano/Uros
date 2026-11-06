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
 *
 * `gpustat bench N' (#744) times N more query_stats round trips after the
 * first, and says it in ipc_bench's "us/op" form.  Every call to gpu_server
 * carries its token and has the kernel verify it on arrival
 * (gpu_core_cap_check), so this is what one gpu RPC costs with that check in
 * it -- measured against a gpu_server that checks nothing, and one that reads
 * the revocation epoch instead, it says what share of an RPC the check is.
 */

#include <stdio.h>
#include <string.h>

#include <mach.h>
#include <mach/bootstrap.h>
#include <mach/mach_traps.h>
#include <mach/thread_switch.h>
#include <mach/cap_types.h>
#include <mach/gpu_stats.h>
#include <mach/clock.h>			/* #744: bench mode's timer */
#include <mach/clock_types.h>

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

/*
 * #744: N round trips of query_stats, timed with the REALTIME clock as
 * ipc_bench times its suites.  A call that fails ends it with WRONG and no
 * number: a timing of refusals would be a timing of something else.
 */
static void
bench_query_stats(mach_port_t host_port, mach_port_t gpu_port,
		  struct uros_cap *cap, int iters)
{
	char			blob[GPU_STATS_BLOB_MAX];
	mach_msg_type_number_t	blob_len;
	mach_port_t		clk = MACH_PORT_NULL;
	tvalspec_t		t0, t1;
	unsigned long long	ns, per;
	kern_return_t		kr;
	int			i;

	if (host_get_clock_service(host_port, REALTIME_CLOCK, &clk)
	    != KERN_SUCCESS) {
		printf("gpustat: bench NOT ASKED -- no clock to time it with\n");
		return;
	}
	for (i = 0; i < 100 + iters; i++) {
		if (i == 100)
			clock_get_time(clk, &t0);
		blob_len = (mach_msg_type_number_t)sizeof(blob);
		kr = gpu_query_stats(gpu_port,
				     (char *)cap, (mach_msg_type_number_t)sizeof(*cap),
				     blob, &blob_len);
		if (kr != KERN_SUCCESS) {
			printf("gpustat: bench WRONG -- call %d of %d failed "
			       "(kr=%d)\n", i, 100 + iters, (int)kr);
			return;
		}
	}
	clock_get_time(clk, &t1);
	ns = (unsigned long long)((long long)t1.tv_sec - (long long)t0.tv_sec)
	     * 1000000000ULL
	     + (unsigned long long)(long long)(t1.tv_nsec - t0.tv_nsec);
	per = ns / (unsigned long long)iters;
	printf("  %-34s %5llu.%02llu us/op  (%d iters, %llu us total)\n",
	       "gpu_query_stats round trip (#744)", per / 1000,
	       (per % 1000) / 10, iters, ns / 1000);
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

	int bench_iters = 0;

	/* `gpustat bench N' (#744); anything else is the one-shot read. */
	if (argc >= 3 && argv != 0 && strcmp(argv[1], "bench") == 0) {
		const char *p;

		for (p = argv[2]; *p >= '0' && *p <= '9'; p++)
			bench_iters = bench_iters * 10 + (*p - '0');
	}

	kr = bootstrap_ports(bootstrap_port,
			     &host_port, &device_port,
			     &root_ledger_wired, &root_ledger_paged,
			     &security_port);
	if (kr != KERN_SUCCESS)
		return 1;

	printf_init(device_port);
	/*
	 * 🔴 AND GIVEN BACK (#511).  Bus authority, wanted to open a console.
	 * See the note above printf_init() in libmach/printf.c.
	 */
	(void) mach_port_deallocate(mach_task_self(), device_port);

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

	if (bench_iters > 0)
		bench_query_stats(host_port, gpu_port, &cap, bench_iters);

	return 0;
}
