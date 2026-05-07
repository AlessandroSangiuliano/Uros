/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * bootstrap/log_forwarder.c — thin wrapper around libgpu_console.
 *
 * The original #198 implementation had its own cap_request +
 * netname_look_up + gpu_text_puts plumbing.  The real logic now
 * lives in libgpu_console (#199 prep) so every server task can
 * mirror its printf to the screen with a single function call;
 * this file remains as a stable bootstrap-internal API in case the
 * future kernel-log-ring drain (#200) wants a bootstrap-private
 * place to hang state that does not belong in libgpu_console.
 *
 * After bootstrap finishes launching servers it invokes
 * log_forwarder_init() → gpu_console_init("bootstrap"), which
 * installs the libsa_mach printf mirror.  From that point bootstrap's
 * own printf reaches both the kernel serial console and the VGA
 * framebuffer via gpu_server.
 */

#include <string.h>
#include <stdio.h>

#include "log_forwarder.h"
#include "gpu_console.h"

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
	printf("Uros 0.1.0 — userspace text path online.\n");
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
