/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * bootstrap/log_forwarder.c — kernel printf → gpu_server bridge.
 *
 * Two paths:
 *
 *   1. log_forwarder_puts(buf, len) / log_forwarder_str(s) —
 *      synchronous mirror of bootstrap's own printf (libsa_mach hook
 *      installed by gpu_console_init).  This is what was here before
 *      #200; nothing changed.
 *
 *   2. drain pthread (#200) — once gpu_console is up, spawn a
 *      worker that pulls bytes out of the kernel ring buffer (see
 *      kern/klog.c) via host_get_log and feeds them to gpu_console.
 *      Without this, the only kernel printf that ever reached the
 *      VGA console after #199 was anything bootstrap re-printed
 *      itself; panic backtraces and pre-userspace boot output went
 *      to serial only.
 *
 * Cadence: 50 ms poll (design doc §11.3 rule 3 — bounded latency
 * from kernel printf to on-screen pixel without burning CPU on an
 * idle ring).  If the drainer ever falls more than 64 KiB behind,
 * the kernel snaps the cursor forward and we lose the gap silently
 * (drop-on-loss — never block printf for a slow viewer).
 */

#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_klog.h>
#include <mach/host_info.h>
#include <mach/thread_switch.h>

#include "log_forwarder.h"
#include "gpu_console.h"

#define KLOG_DRAIN_INTERVAL_MS	50	/* 50 ms — design doc §11.3 rule 3 */

extern kern_return_t thread_switch(mach_port_t thread,
				   int option,
				   mach_msg_timeout_t option_time);

static pthread_t klog_drain_tid;
static int       klog_drain_started;

static void *
klog_drain_thread(void *arg)
{
	natural_t cursor = 0;
	mach_port_t host = mach_host_self();
	klog_data_t buf;

	(void)arg;

	for (;;) {
		mach_msg_type_number_t cnt = sizeof(buf);
		natural_t next;
		kern_return_t kr;

		kr = host_get_log(host, cursor, buf, &cnt, &next);
		if (kr == KERN_SUCCESS && cnt > 0) {
			gpu_console_puts(buf, (size_t)cnt);
			cursor = next;
		}
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT,
			      KLOG_DRAIN_INTERVAL_MS);
	}
	return NULL;
}

int
log_forwarder_init(void)
{
	int rc = gpu_console_init("bootstrap");
	if (rc < 0) {
		printf("log_forwarder: gpu_console_init failed — "
		       "on-screen logging disabled (serial still works)\n");
		return rc;
	}
	/* Sanity-check banner: the next libsa_mach printf is mirrored
	 * to gpu_server, so this line should land on the VGA console
	 * via vga.so → 0xB8000. */
	printf("Uros 0.2.1 — userspace text path online.\n");

	/* #200: spawn the kernel ring drain.  Failure is non-fatal —
	 * bootstrap's own printf still mirrors via libsa_mach. */
	if (!klog_drain_started) {
		int err = pthread_create(&klog_drain_tid, NULL,
					 klog_drain_thread, NULL);
		if (err == 0) {
			klog_drain_started = 1;
			printf("log_forwarder: kernel klog drain started\n");
		} else {
			printf("log_forwarder: pthread_create failed (%d) — "
			       "kernel printf will not reach VGA\n", err);
		}
	}
	return 0;
}

void
log_forwarder_puts(const char *buf, size_t len)
{
	gpu_console_puts(buf, len);
}

void
log_forwarder_str(const char *s)
{
	if (s == NULL)
		return;
	gpu_console_puts(s, strlen(s));
}
