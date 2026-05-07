/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * bootstrap/log_forwarder.c — first and only client of gpu_text_puts
 * in 0.1.0 (#198).
 *
 * Lifecycle:
 *   1. log_forwarder_init() runs once after bootstrap has launched
 *      every server in bootstrap.conf.  By that point cap_server is
 *      up (so we can cap_request) and gpu_server has registered
 *      "gpu" in netname (so we can look it up).  Both conditions are
 *      enforced by bootstrap.conf order — see make-bundle.sh /
 *      make-disk-image.sh.
 *   2. We cap_request a token authorising GPU_CAP_DISPLAY_SCANOUT on
 *      resource_id 0 (the system text surface) and stash both the
 *      token and the gpu_server send right.
 *   3. log_forwarder_puts() sends gpu_text_puts as a MIG
 *      simpleroutine — no reply, no timeout block, no allocation.
 *      Producers (bootstrap itself today; future panic / ring-drain
 *      paths) push through here.
 *
 * Drain thread:
 *   The design doc §8.2 (and issue #198) call for a pthread that
 *   drains a kernel log ring buffer.  That ring does not exist in
 *   0.1.0 — kernel printf still goes straight to serial + (until
 *   #199) the in-kernel VGA driver.  When the ring lands, this file
 *   gains a worker that runs `read_kring_chunk(); puts(...)` in a
 *   loop; the puts() API stays unchanged, so call sites elsewhere
 *   in bootstrap don't need updating.
 */

#include <mach.h>
#include <mach/mach_port.h>
#include <stdio.h>
#include <string.h>

#include <servers/netname.h>
#include <gpu/gpu_types.h>
#include <mach/cap_types.h>

#include "libcap.h"
#include "log_forwarder.h"

/* MIG-generated user stub — name follows `userprefix gpu_` from
 * gpu_server.defs.  The header defining it is gpu_server.h emitted
 * into ${CMAKE_CURRENT_BINARY_DIR}, included via the bootstrap
 * include path. */
#include "gpu_server.h"

/* ============================================================
 * State.  The cap struct is wire-format — we cap_request(...) once
 * and pass the same blob to every gpu_text_puts.  No locking: the
 * struct is written exactly once during init() before any other
 * thread observes it.
 * ============================================================ */

static int		log_fwd_ready;
static mach_port_t	log_fwd_gpu_port = MACH_PORT_NULL;
static struct uros_cap	log_fwd_cap;

/* ============================================================
 * Init
 * ============================================================ */

int
log_forwarder_init(void)
{
	kern_return_t kr;

	if (log_fwd_ready)
		return 0;

	/* Resolve gpu_server's service port. */
	kr = netname_look_up(name_server_port, "", "gpu", &log_fwd_gpu_port);
	if (kr != KERN_SUCCESS) {
		printf("log_forwarder: netname_look_up(\"gpu\") failed "
		       "(kr=%d) — on-screen logging disabled\n", (int)kr);
		return -1;
	}

	/* Acquire a cap that lets us write to the system text surface.
	 * resource_id=0 is the convention gpu_server uses for "the lone
	 * display in 0.1.0".  Lifetime 0 = never expires. */
	kr = cap_request(GPU_CAP_RESOURCE_TYPE,
			 /*resource_id*/ 0,
			 GPU_CAP_DISPLAY_SCANOUT,
			 /*lifetime_ms*/ 0,
			 &log_fwd_cap);
	if (kr != KERN_SUCCESS) {
		printf("log_forwarder: cap_request failed (kr=%d) — "
		       "on-screen logging disabled\n", (int)kr);
		(void)mach_port_deallocate(mach_task_self(),
					   log_fwd_gpu_port);
		log_fwd_gpu_port = MACH_PORT_NULL;
		return -1;
	}

	log_fwd_ready = 1;
	printf("log_forwarder: ready (gpu_port=0x%x cap_id=%llu)\n",
	       (unsigned)log_fwd_gpu_port,
	       (unsigned long long)log_fwd_cap.cap_id);

	/* First on-screen line: announce that the userspace text path
	 * is live.  Doubles as a sanity check that the whole chain
	 * (cap_request → gpu_text_puts → text_render → vga.so → 0xB8000)
	 * is wired. */
	log_forwarder_str("Uros 0.1.0 — userspace text path online.\n");
	return 0;
}

/* ============================================================
 * puts / str
 * ============================================================ */

void
log_forwarder_puts(const char *buf, size_t len)
{
	if (!log_fwd_ready || buf == NULL || len == 0)
		return;
	if (len > GPU_BUF_MAX)
		len = GPU_BUF_MAX;

	/* simpleroutine: fire-and-forget, no reply expected.  If the
	 * gpu_server queue is full it drops silently on its side; if
	 * the Mach send fails we ignore that too — best-effort log
	 * semantics per §11.3 rule 2. */
	(void)gpu_text_puts(log_fwd_gpu_port,
			    (char *)&log_fwd_cap, sizeof(log_fwd_cap),
			    (char *)buf, (mach_msg_type_number_t)len);
}

void
log_forwarder_str(const char *s)
{
	if (s == NULL)
		return;
	log_forwarder_puts(s, strlen(s));
}
