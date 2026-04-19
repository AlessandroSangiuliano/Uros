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
 * cap_table.h — cap_server internal storage for issued capabilities.
 *
 * v1 layout: single-shard, mutex-protected, with three hash indexes
 * (by_id / by_owner / by_parent) over the same cap_entry pool.
 * 64-shard + 4-thread sharding is out-of-scope per plan.
 */

#ifndef _CAP_SERVER_CAP_TABLE_H_
#define _CAP_SERVER_CAP_TABLE_H_

#include <stdint.h>
#include <stddef.h>
#include <mach.h>
#include <mach/cap_types.h>

struct cap_entry {
    struct uros_cap  token;           /* signed + wire-format */
    mach_port_t      sender;          /* emission-time requester port */
    struct cap_entry *next_by_id;
    struct cap_entry *next_by_owner;
    struct cap_entry *next_by_parent;
};

void cap_table_init(void);

/*
 * Ownership: the table takes ownership of `e` on success and returns 0.
 * The caller allocates the entry; on error it remains owned by the caller.
 */
int  cap_table_insert(struct cap_entry *e);

struct cap_entry *cap_table_find_by_id(uint64_t cap_id);
struct cap_entry *cap_table_find_children(uint64_t parent_id);

/*
 * Mark a single entry revoked (token.revoked = 1).  Callers iterate
 * the delegation tree themselves via cap_table_find_children() to
 * implement cascade revocation.
 */
void cap_table_mark_revoked(struct cap_entry *e);

/*
 * Monotonic id generator used by cap_table_insert callers before they
 * sign the token.  Thread-safe (atomic fetch-add).
 */
uint64_t cap_table_alloc_id(void);

/*
 * HMAC helpers — sign or verify a token against the cap_server key.
 * sign: recomputes and writes token->hmac in place.
 * verify: returns 1 if HMAC matches, 0 otherwise.
 */
void cap_sign(struct uros_cap *token);
int  cap_verify_hmac(const struct uros_cap *token);

/*
 * Key setup.  Called once at cap_server startup; generates the HMAC
 * key from a mix of available entropy sources.  In Issue B the kernel
 * takes over key management via urmach_cap_register.
 */
void cap_key_init(void);

#endif /* _CAP_SERVER_CAP_TABLE_H_ */
