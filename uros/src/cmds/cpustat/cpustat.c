/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * cpustat — per-CPU load monitor (#375).
 *
 * The first real Uros tool linked *dynamically* against the umbrella
 * libc.so (rather than statically, like ush / ipc_bench / gpustat): a
 * shell-launched musl binary that reports per-processor CPU utilisation,
 * the "top per-core" view.
 *
 * It reads every processor's cpu_ticks in one call,
 * host_processor_info(PROCESSOR_CPU_LOAD_INFO), by the host's name port, at
 * two instants -- not host_processors(), which hands out the processors'
 * control ports and asks for the privileged host port, a shell tool's to have
 * neither (#786).  The share spent in each state is that state's tick delta
 * over the total delta, so no wall-clock is needed — the sample interval
 * only sets how long a window each line averages over.  The host feeds
 * these counters from machine_slot[cpu].cpu_ticks[], bumped per-CPU by the
 * timer interrupt (kern/mach_clock.c), so each row is that core's own time.
 *
 *   cpustat [interval]
 *     interval   seconds between refreshes; when given, cpustat loops like
 *                top.  With no argument it prints a single sample taken
 *                over a one-second window.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mach.h>
#include <mach/processor_info.h>
#include <mach/machine.h>		/* CPU_STATE_* */
#include <mach/mach_host.h>		/* the host_processor_info stub */

/* mach_host_self and host_processor_info are imported from the umbrella
 * libc.so, which #375 exports from its folded-in Mach backend so this tool
 * runs the umbrella's single, initialised Mach runtime instead of a static
 * duplicate.  The trap has no prototype in the musl include world. */
extern mach_port_t mach_host_self(void);

typedef struct processor_cpu_load_info	cpu_load_t;

static void
nap(unsigned int secs)
{
	struct timespec ts;

	ts.tv_sec = secs;
	ts.tv_nsec = 0;
	nanosleep(&ts, NULL);
}

/*
 * Read every processor's cpu_ticks snapshot into load[], all in one call:
 * host_processor_info() answers one PROCESSOR_CPU_LOAD_INFO record per
 * processor, back to back.  Returns how many processors answered, or 0 when
 * the call failed or the records are not the size load[] holds.  load may
 * be null, to count them.
 */
static unsigned int
sample(mach_port_t host, cpu_load_t *load, unsigned int room)
{
	integer_t buf[PROCESSOR_INFO_ARRAY_MAX];
	mach_msg_type_number_t count = PROCESSOR_INFO_ARRAY_MAX;
	natural_t n = 0;
	kern_return_t kr;

	kr = host_processor_info(host, PROCESSOR_CPU_LOAD_INFO, &n, buf,
				 &count);
	if (kr != KERN_SUCCESS || n == 0 ||
	    count != n * PROCESSOR_CPU_LOAD_INFO_COUNT) {
		printf("cpustat: host_processor_info failed (kr=%d, %u "
		       "processors, %u ints)\n", (int)kr, (unsigned)n,
		       (unsigned)count);
		return 0;
	}
	if (load != NULL) {
		if (n != room) {
			printf("cpustat: %u processors answered, %u expected\n",
			       (unsigned)n, room);
			return 0;
		}
		memcpy(load, buf, n * sizeof(*load));
	}
	return n;
}

/* Print the per-core percentages from the two snapshots' tick deltas. */
static void
report(const cpu_load_t *a, const cpu_load_t *b, mach_msg_type_number_t nprocs)
{
	mach_msg_type_number_t i;

	printf("CPU   user   nice    sys   idle\n");
	for (i = 0; i < nprocs; i++) {
		unsigned long d[CPU_STATE_MAX], total = 0;
		int s;

		for (s = 0; s < CPU_STATE_MAX; s++) {
			d[s] = (unsigned long)b[i].cpu_ticks[s] -
			       (unsigned long)a[i].cpu_ticks[s];
			total += d[s];
		}
		if (total == 0)
			total = 1;	/* offline / idle-frozen core: avoid /0 */

		printf("%3u  %4lu%%  %4lu%%  %4lu%%  %4lu%%\n",
		       (unsigned)i,
		       d[CPU_STATE_USER]   * 100 / total,
		       d[CPU_STATE_NICE]   * 100 / total,
		       d[CPU_STATE_SYSTEM] * 100 / total,
		       d[CPU_STATE_IDLE]   * 100 / total);
	}
}

int
main(int argc, char **argv)
{
	mach_port_t host;
	unsigned int nprocs;
	unsigned int interval = 0;
	cpu_load_t *a, *b;

	setvbuf(stdout, NULL, _IONBF, 0);	/* unbuffered — a live monitor */

	host = mach_host_self();

	if (argc > 1) {
		int v = atoi(argv[1]);
		if (v > 0)
			interval = (unsigned int)v;
	}

	nprocs = sample(host, NULL, 0);
	if (nprocs == 0)
		return 1;

	a = calloc(nprocs, sizeof(*a));
	b = calloc(nprocs, sizeof(*b));
	if (a == 0 || b == 0) {
		printf("cpustat: out of memory for %u cpus\n", (unsigned)nprocs);
		return 1;
	}

	printf("cpustat: %u processor%s\n",
	       (unsigned)nprocs, nprocs == 1 ? "" : "s");

	if (sample(host, b, nprocs) == 0)
		return 1;

	if (interval == 0) {
		/*
		 * One-shot ("top -n1"): per-core utilisation since boot.  'a'
		 * came zeroed from calloc, so report(a, b) prints b's cumulative
		 * ticks as a percentage — a single sample, hence no sleep.  Pass
		 * an interval to get the live delta refresh below instead.
		 */
		report(a, b, nprocs);
		return 0;
	}

	for (;;) {
		memcpy(a, b, nprocs * sizeof(*a));
		nap(interval);
		if (sample(host, b, nprocs) == 0)
			return 1;
		report(a, b, nprocs);
	}
}
