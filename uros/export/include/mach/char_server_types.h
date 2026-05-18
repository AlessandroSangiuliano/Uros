/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * mach/char_server_types.h — Narrow ctype typedefs imported by the
 * char_server.defs MIG file.  Mirror mach/gpu_server_types.h.
 */

#ifndef _MACH_CHAR_SERVER_TYPES_H_
#define _MACH_CHAR_SERVER_TYPES_H_

#include <stdint.h>

typedef uint32_t	chr_u32_t;
typedef uint64_t	chr_u64_t;

#define CHR_TOKEN_MAX	256
#define CHR_BUF_MAX	4096

typedef char		chr_token_t[CHR_TOKEN_MAX];
typedef char		chr_buf_t[CHR_BUF_MAX];

#endif /* _MACH_CHAR_SERVER_TYPES_H_ */
