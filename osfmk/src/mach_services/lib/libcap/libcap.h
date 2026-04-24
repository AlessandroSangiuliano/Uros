/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

/*
 * File:    libcap.h
 * Purpose: Client API for the Uros capability system.  Wraps the MIG
 *          RPC interface (cap_server.defs) in a small, stable surface
 *          that hides token serialization and port lookup from clients.
 *
 * Migration plan:
 *   Issue A — all calls are RPC to cap_server.
 *   Issue B — cap_verify() switches to the UrMach fast-path trap
 *             (urmach_cap_verify); cap_request/derive/revoke stay RPC.
 */

#ifndef _LIBCAP_LIBCAP_H_
#define _LIBCAP_LIBCAP_H_

#include <mach.h>
#include <mach/cap_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CAP_TOKEN_MAX — on-wire upper bound for the opaque token blob.  Must
 * match cap_server.defs (cap_token_t = array[*:256]).  sizeof(struct
 * uros_cap) must stay below this number.
 */
#define CAP_TOKEN_MAX    256

/*
 * cap_server_port() — look up the cap_server via netname.  Cached
 * after first call.  Returns MACH_PORT_NULL on failure.
 */
mach_port_t cap_server_port(void);

/*
 * cap_request() — ask cap_server to issue a fresh capability for a
 * resource.  On success, *out is filled with a signed token.
 *
 * lifetime_ms: 0 = never expires.
 */
kern_return_t cap_request(uint32_t resource_type,
                          uint64_t resource_id,
                          uint64_t ops,
                          uint32_t lifetime_ms,
                          struct uros_cap *out);

/*
 * cap_derive() — delegate a reduced-ops child token from parent.
 * reduced_ops must be a subset of parent->allowed_ops.
 */
kern_return_t cap_derive(const struct uros_cap *parent,
                         uint64_t reduced_ops,
                         uint32_t lifetime_ms,
                         struct uros_cap *out);

/*
 * cap_revoke() — revoke a capability and its whole subtree.
 */
kern_return_t cap_revoke(uint64_t cap_id);

/*
 * cap_verify() — check that the caller is allowed to perform `op`
 * on `resource_id` using `token`.  In Issue A this is an RPC; once
 * Issue B lands it routes to the UrMach fast-path trap.
 */
kern_return_t cap_verify(const struct uros_cap *token,
                         uint64_t op,
                         uint64_t resource_id);

/*
 * cap_verify_local() — Issue B fast path: go straight to the UrMach
 * urmach_cap_verify trap (slot 37) without an RPC to cap_server.  Use
 * this in hot paths where the caller already holds a token; use
 * cap_verify() when cap_server-specific policy is needed.
 */
kern_return_t cap_verify_local(const struct uros_cap *token,
                               uint32_t op,
                               uint64_t resource_id);

/*
 * cap_use_local() — fast path for consume-one-use tokens.  Equivalent
 * to cap_verify_local() plus atomic bookkeeping: when the token was
 * issued with max_uses != 0, the kernel decrements a per-cap_id use
 * counter and returns CAP_ERR_EXHAUSTED once it hits zero.
 */
kern_return_t cap_use_local(const struct uros_cap *token,
                            uint32_t op,
                            uint64_t resource_id);

#ifdef __cplusplus
}
#endif

#endif /* _LIBCAP_LIBCAP_H_ */
