/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * libgpu_console — generic "mirror printf to gpu_server" client.
 *
 * Once the in-kernel VGA driver leaves (#199) the kernel "console"
 * device is serial-only.  Servers that want their printf output to
 * also appear on screen (i.e. all of them, except early-boot kernel
 * code which has no choice but serial) link this library and call
 * gpu_console_init() once after they have a bootstrap_port and the
 * name_server is reachable.
 *
 * After init, every libsa_mach printf is mirrored to gpu_text_puts
 * via the printf hook installed in libsa_mach (see
 * `printf_set_mirror`).  Failures are silent: gpu_server unreachable,
 * cap_request denied, send error — all degrade to "serial only", the
 * caller's printf path keeps working.
 *
 * This is a generalisation of the bootstrap-private log_forwarder
 * landed in #198: same idea, but reusable from every userspace task.
 */

#ifndef _LIBGPU_CONSOLE_H_
#define _LIBGPU_CONSOLE_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up the gpu mirror.  Returns 0 on success, < 0 on failure
 * (caller treats failure as non-fatal — kernel serial console keeps
 * working).  Idempotent: subsequent calls are no-ops.
 *
 * The optional `tag` is a short label (≤ 16 chars) prepended to every
 * mirrored line so the screen makes it clear which task is printing.
 * Pass NULL to disable.
 */
int  gpu_console_init(const char *tag);

/*
 * Push a chunk of text directly through the gpu mirror, bypassing
 * any libsa_mach hook.  Useful for tasks that don't use the libsa_mach
 * printf path (e.g. raw dumps from a custom logger).  No-op if init
 * failed or wasn't called.
 */
void gpu_console_puts(const char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* _LIBGPU_CONSOLE_H_ */
