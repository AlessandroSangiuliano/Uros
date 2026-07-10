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
 * It enumerates the host's processors with host_processors() and reads
 * each one's cpu_ticks through processor_info(PROCESSOR_CPU_LOAD_INFO) at
 * two instants.  The share spent in each state is that state's tick delta
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
#include <mach/mach_host.h>		/* host_processors / processor_info stubs */

/* mach_host_self / host_processors / processor_info are imported from the
 * umbrella libc.so, which #375 exports from its folded-in Mach backend so
 * this tool runs the umbrella's single, initialised Mach runtime instead of
 * a static duplicate.  The trap has no prototype in the musl include world. */
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

/* Read every processor's cpu_ticks snapshot into load[].  0 on success. */
static int
sample(processor_port_array_t procs, mach_msg_type_number_t nprocs,
       cpu_load_t *load)
{
	mach_msg_type_number_t i;

	for (i = 0; i < nprocs; i++) {
		mach_port_t phost;
		mach_msg_type_number_t count = PROCESSOR_CPU_LOAD_INFO_COUNT;
		kern_return_t kr = processor_info(procs[i],
						  PROCESSOR_CPU_LOAD_INFO, &phost,
						  (processor_info_t)&load[i],
						  &count);
		if (kr != KERN_SUCCESS) {
			printf("cpustat: processor_info(cpu %u) failed (kr=%d)\n",
			       (unsigned)i, (int)kr);
			return -1;
		}
	}
	return 0;
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
	processor_port_array_t procs = 0;
	mach_msg_type_number_t nprocs = 0;
	unsigned int interval = 0;
	kern_return_t kr;
	cpu_load_t *a, *b;

	setvbuf(stdout, NULL, _IONBF, 0);	/* unbuffered — a live monitor */

	host = mach_host_self();

	if (argc > 1) {
		int v = atoi(argv[1]);
		if (v > 0)
			interval = (unsigned int)v;
	}

	kr = host_processors(host, &procs, &nprocs);
	if (kr != KERN_SUCCESS || nprocs == 0) {
		printf("cpustat: host_processors failed (kr=%d)\n", (int)kr);
		return 1;
	}

	a = calloc(nprocs, sizeof(*a));
	b = calloc(nprocs, sizeof(*b));
	if (a == 0 || b == 0) {
		printf("cpustat: out of memory for %u cpus\n", (unsigned)nprocs);
		return 1;
	}

	printf("cpustat: %u processor%s\n",
	       (unsigned)nprocs, nprocs == 1 ? "" : "s");

	if (sample(procs, nprocs, b) != 0)
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
		if (sample(procs, nprocs, b) != 0)
			return 1;
		report(a, b, nprocs);
	}
}
