/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * libgpu_console — implementation.  See gpu_console.h.
 *
 * Lifecycle:
 *   1. gpu_console_init(tag) resolves "gpu" via netname, cap_request's
 *      a long-lived GPU_CAP_DISPLAY_SCANOUT token, stashes both, and
 *      installs a printf mirror hook in libsa_mach.
 *   2. Every libsa_mach printf flush runs through `mirror_hook`,
 *      which calls gpu_text_puts as a MIG simpleroutine — fire-and-
 *      forget, never blocks the printf flush path.
 *   3. Failures (gpu_server not registered, cap_request denied, send
 *      error) are silent: printf still reaches the kernel console
 *      (serial after #199) so log output is never lost.
 *
 * No threads, no queue: the gpu_server side already has its own
 * text_render queue (#196) that absorbs paint-side latency.  Adding a
 * second queue here would just double the drop budget for no gain.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <pthread.h>
#include <sa_mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <servers/netname.h>
#include <gpu/gpu_types.h>
#include <mach/cap_types.h>

#include "libcap.h"
#include "gpu_console.h"

/* MIG-generated user stub for gpu_server.defs.  The header `gpu_server.h`
 * lives alongside the MIG output for libgpu_console (see CMakeLists). */
#include "gpu_server.h"

/* libsa_mach hook — see printf.c (#199 prep): if non-NULL, every
 * console flush also calls this with the buffered string.  We declare
 * it here as an extern so libgpu_console can install it without
 * pulling in a libsa_mach header. */
extern void printf_set_mirror(void (*hook)(const char *buf, size_t len));

/* ============================================================
 * State.  Written once from gpu_console_init(); the mirror hook
 * reads but never writes after that, so no locking is needed.
 * ============================================================ */

static int		gc_ready;
static mach_port_t	gc_gpu_port = MACH_PORT_NULL;
static struct uros_cap	gc_cap;

#define GC_TAG_MAX	16
static char		gc_tag[GC_TAG_MAX + 2];	/* "[tag] " + NUL */
static size_t		gc_tag_len;

/* ============================================================
 * Internal: send a chunk to gpu_server via the simpleroutine.
 * ============================================================ */

static void
gc_send(const char *buf, size_t len)
{
	if (!gc_ready || buf == NULL || len == 0)
		return;
	if (len > GPU_BUF_MAX)
		len = GPU_BUF_MAX;

	(void)gpu_text_puts(gc_gpu_port,
			    (char *)&gc_cap, sizeof(gc_cap),
			    (char *)buf, (mach_msg_type_number_t)len);
}

/* ============================================================
 * libsa_mach printf hook.  Adds the optional tag prefix at every
 * line break — without it, the screen would mix output from N
 * servers into an unreadable stream.
 *
 * The flush buffer libsa_mach hands us is at most PRINTF_BUFMAX (128
 * bytes) and is already \r\n-cooked.  We treat each call as one
 * "burst": tag once at the start, send the rest as-is.  Newlines in
 * the middle don't get re-tagged — keeps overhead low and matches
 * the way a serial console looks anyway.
 * ============================================================ */

static void
mirror_hook(const char *buf, size_t len)
{
	if (gc_tag_len > 0) {
		gc_send(gc_tag, gc_tag_len);
	}
	gc_send(buf, len);
}

/* ============================================================
 * Init
 * ============================================================ */

int
gpu_console_init(const char *tag)
{
	kern_return_t kr;

	if (gc_ready)
		return 0;

	kr = netname_look_up(name_server_port, "", "gpu", &gc_gpu_port);
	if (kr != KERN_SUCCESS)
		return -1;

	kr = cap_request(GPU_CAP_RESOURCE_TYPE,
			 /*resource_id*/ 0,
			 GPU_CAP_DISPLAY_SCANOUT,
			 /*lifetime_ms*/ 0,
			 &gc_cap);
	if (kr != KERN_SUCCESS) {
		(void)mach_port_deallocate(mach_task_self(), gc_gpu_port);
		gc_gpu_port = MACH_PORT_NULL;
		return -1;
	}

	if (tag != NULL && tag[0] != '\0') {
		size_t n = strlen(tag);
		if (n > GC_TAG_MAX) n = GC_TAG_MAX;
		gc_tag[0] = '[';
		memcpy(&gc_tag[1], tag, n);
		gc_tag[n + 1] = ']';
		gc_tag[n + 2] = ' ';
		gc_tag_len = n + 3;
	} else {
		gc_tag_len = 0;
	}

	gc_ready = 1;
	printf_set_mirror(mirror_hook);
	return 0;
}

void
gpu_console_puts(const char *buf, size_t len)
{
	gc_send(buf, len);
}

/* ============================================================
 * Async / retrying init (#209)
 *
 * Servers booted before gpu_server (today only cap_server) use
 * this so they don't have to think about bootstrap.conf order.
 * The worker thread is detached: nobody joins it, it exits as
 * soon as gpu_console_init() succeeds or the retry budget runs
 * out.
 * ============================================================ */

struct gpu_console_async_arg {
	char		tag[GC_TAG_MAX + 1];
	unsigned int	retry_us;
	unsigned int	max_tries;
};

static void *
gpu_console_async_worker(void *raw)
{
	struct gpu_console_async_arg *a = raw;
	unsigned int n;

	for (n = 0; n < a->max_tries; n++) {
		if (gc_ready)
			break;	/* somebody else won */
		if (gpu_console_init(a->tag) == 0)
			break;
		usleep(a->retry_us);
	}

	free(a);
	return NULL;
}

int
gpu_console_init_async(const char *tag,
		       unsigned int retry_ms,
		       unsigned int max_tries)
{
	pthread_t       tid;
	pthread_attr_t  attr;
	struct gpu_console_async_arg *a;
	int             rc;

	if (gc_ready)
		return 0;

	a = malloc(sizeof(*a));
	if (a == NULL)
		return -1;

	if (tag != NULL) {
		size_t n = strlen(tag);
		if (n > GC_TAG_MAX) n = GC_TAG_MAX;
		memcpy(a->tag, tag, n);
		a->tag[n] = '\0';
	} else {
		a->tag[0] = '\0';
	}
	a->retry_us  = (retry_ms ? retry_ms : 100u) * 1000u;
	a->max_tries = (max_tries ? max_tries : 50u);

	rc = pthread_attr_init(&attr);
	if (rc == 0)
		(void)pthread_attr_setdetachstate(&attr,
				PTHREAD_CREATE_DETACHED);

	rc = pthread_create(&tid, (rc == 0) ? &attr : NULL,
			    gpu_console_async_worker, a);
	(void)pthread_attr_destroy(&attr);

	if (rc != 0) {
		free(a);
		return -1;
	}
	return 0;
}
