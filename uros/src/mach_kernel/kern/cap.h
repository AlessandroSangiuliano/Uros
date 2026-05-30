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
 * File:    kern/cap.h
 * Purpose: UrMach capability fast-path trap handlers.  These replace the
 *          RPC-to-cap_server round trip for the hot verify/use paths and
 *          implement the kernel side of the Uros capability system.
 *
 *          The kernel holds a single HMAC-SHA256 key, installed at boot
 *          by cap_server through a setup token (cap_id == 0).  The first
 *          task that registers a key becomes the trusted cap_server
 *          identity — all later register calls from other tasks are
 *          rejected.  The kernel also keeps a small revocation/consumed
 *          set keyed by cap_id so that revoke and use-once semantics
 *          survive the self-verifying token format.
 *
 * Trap slot numbers: 37, 38, 39, 40 in mach_trap_table.
 *
 * Design: /home/slex/Documenti/uros_docs/Uros_tecnico_v6.pdf §4, §7.
 */

#ifndef _KERN_CAP_H_
#define _KERN_CAP_H_

#include <mach/boolean.h>
#include <mach/kern_return.h>
#include <mach/cap_types.h>

/*
 * One-time subsystem init — called from startup.c after the basic
 * task / thread / IPC plumbing is up.  Initializes locks and zeroes the
 * HMAC key and revocation table; does NOT install a key (cap_server
 * does that once it launches).
 */
extern void cap_init(void);

/*
 * Fast-path: validate `user_token` against the cap_hmac_key, ensure it
 * authorizes `op` on `resource_id`, and that it has not been revoked or
 * expired.  Pure check — no side effects on the token or on kernel state.
 */
extern kern_return_t urmach_cap_verify(struct uros_cap *user_token,
                                       uint32_t op,
                                       uint64_t resource_id);

/*
 * Same as urmach_cap_verify, plus atomic bookkeeping for bounded-use
 * tokens: if max_uses != 0 the kernel consumes one use from its own
 * table (cap_id keyed) and returns CAP_ERR_EXHAUSTED once the budget
 * is gone.
 */
extern kern_return_t urmach_cap_use(struct uros_cap *user_token,
                                    uint32_t op,
                                    uint64_t resource_id);

/*
 * Add `cap_id` to the revocation set.  Only the cap_server task (the
 * one captured at setup-token time) may call this; other callers get
 * CAP_ERR_UNAUTHORIZED.
 */
extern kern_return_t urmach_cap_revoke(uint64_t cap_id);

/*
 * Setup / bookkeeping entry point:
 *   - If cap_id == 0 the token is a "setup token": the first 32 bytes
 *     of its hmac[] field are installed as the HMAC key and the caller
 *     becomes the trusted cap_server task (trust-on-first-use).  Later
 *     setup-token calls from the same task refresh the key; calls from
 *     other tasks return CAP_ERR_UNAUTHORIZED.
 *   - If cap_id != 0 the call is reserved for future cap bookkeeping
 *     (e.g. populating kernel-side metadata); v1 accepts and ignores it.
 */
extern kern_return_t urmach_cap_register(struct uros_cap *user_token);

#endif /* _KERN_CAP_H_ */
