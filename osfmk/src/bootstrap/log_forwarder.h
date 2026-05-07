/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * log_forwarder.h — bootstrap → gpu_server text path (#198).
 *
 * After every `bootstrap.conf` server has come up, bootstrap calls
 * log_forwarder_init() to acquire a GPU_CAP_DISPLAY_SCANOUT cap from
 * cap_server, look up the "gpu" service port via netname, and start
 * the (currently parked) drain thread that will pull from the kernel
 * log ring once it exists.
 *
 * Until that ring lands, log_forwarder_puts() is the active path: a
 * MIG simpleroutine to gpu_text_puts that fires-and-forgets so the
 * caller never blocks on VGA paint.  Per design doc §11.3 rule 2.
 */

#ifndef _BOOTSTRAP_LOG_FORWARDER_H_
#define _BOOTSTRAP_LOG_FORWARDER_H_

#include <stddef.h>

/*
 * Bring up the forwarder.  Returns 0 on success, negative on failure;
 * callers should treat failure as non-fatal (the kernel console is
 * still working and bootstrap can finish without on-screen logs).
 */
int  log_forwarder_init(void);

/*
 * Push a chunk of text to gpu_server.  No-op if init failed.  Drops
 * silently on send error.  Up to 4096 bytes per call (chunks are
 * truncated, not split).
 */
void log_forwarder_puts(const char *buf, size_t len);

/*
 * Convenience: log_forwarder_puts(s, strlen(s)).
 */
void log_forwarder_str(const char *s);

#endif /* _BOOTSTRAP_LOG_FORWARDER_H_ */
