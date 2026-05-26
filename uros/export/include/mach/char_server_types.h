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

/*
 * RPC result codes (#275.2).  Carried as `out result : int` on routines
 * that need to signal POSIX semantics beyond Mach's kern_return_t.
 *
 *   CHR_OK              — success
 *   CHR_TTY_BACKGROUND  — caller is in a background pgrp of this tty's
 *                         controlling session; SIGTTIN/SIGTTOU has been
 *                         sent to the calling pgrp.  libposix-uros's
 *                         wrapper maps this to errno = EINTR after the
 *                         handler runs (or EIO if the signal is blocked).
 *   CHR_ERR_INVAL       — bad device / class mismatch
 *   CHR_ERR_NO_MODULE   — module hook not wired
 *   CHR_ERR_KERNEL      — underlying Mach call failed
 */
#define CHR_OK                 0
#define CHR_TTY_BACKGROUND    -1
#define CHR_ERR_INVAL         -2
#define CHR_ERR_NO_MODULE     -3
#define CHR_ERR_KERNEL        -4

#endif /* _MACH_CHAR_SERVER_TYPES_H_ */
