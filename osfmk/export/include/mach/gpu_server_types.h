/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * mach/gpu_server_types.h — Narrow ctype typedefs imported by the
 * gpu_server.defs MIG file.
 *
 * MIG ctype names appear verbatim in the generated user/server stubs;
 * this header maps them to the underlying fixed-width types so the
 * generated code compiles cleanly against <stdint.h>.  Public payload
 * types (gpu_mode_t, gpu_device_info, GPU_CAP_*) live in
 * gpu/gpu_types.h.
 */

#ifndef _MACH_GPU_SERVER_TYPES_H_
#define _MACH_GPU_SERVER_TYPES_H_

#include <stdint.h>

typedef uint32_t	gpu_u32_t;
typedef uint64_t	gpu_u64_t;

/* Wire-format upper bounds — keep in sync with gpu_server.defs.
 * The MIG-generated stubs reference these typedefs by name and
 * size them against the bounds declared in the .defs `type`
 * directives. */
#define GPU_TOKEN_MAX	256
#define GPU_BUF_MAX	4096

typedef char	gpu_token_t[GPU_TOKEN_MAX];
typedef char	gpu_buf_t[GPU_BUF_MAX];

#endif /* _MACH_GPU_SERVER_TYPES_H_ */
