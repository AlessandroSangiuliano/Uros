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
 * File:    kern/sha256.h
 * Purpose: Kernel-side SHA-256 (FIPS 180-4).  Shares its wire algorithm
 *          with the userspace libcap/sha256.c — the two implementations
 *          are kept source-identical on purpose so cap_server and
 *          urmach_cap_verify agree bit-for-bit on token HMACs.
 */

#ifndef _KERN_SHA256_H_
#define _KERN_SHA256_H_

#include <stddef.h>
#include <stdint.h>

#define SHA256_BLOCK_SIZE   64
#define SHA256_DIGEST_SIZE  32

struct sha256_ctx {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t  buf[SHA256_BLOCK_SIZE];
    size_t   buflen;
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(struct sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE]);
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

/*
 * Issue #180: runtime selection of the per-block compress primitive.
 * sha256_dispatch_init() probes CPUID for SHA Extensions and, if
 * present, swaps the internal compress pointer to the SHA-NI
 * implementation.  Safe to call multiple times; idempotent.  Must be
 * called after FPU/SSE init (the SHA-NI path uses XMM registers).
 */
void sha256_dispatch_init(void);

/*
 * Returns 1 if the SHA-NI fast path is currently selected, 0 otherwise.
 * Useful for boot-time logging and self-test gating.
 */
int  sha256_using_sha_ni(void);

#endif /* _KERN_SHA256_H_ */
