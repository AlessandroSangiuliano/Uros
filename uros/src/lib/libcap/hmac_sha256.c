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

#include "hmac_sha256.h"

void hmac_sha256(const void *key, size_t key_len,
                 const void *msg, size_t msg_len,
                 uint8_t out[HMAC_SHA256_SIZE])
{
    uint8_t k_block[SHA256_BLOCK_SIZE];
    uint8_t ipad[SHA256_BLOCK_SIZE];
    uint8_t opad[SHA256_BLOCK_SIZE];
    uint8_t inner[SHA256_DIGEST_SIZE];
    struct sha256_ctx ctx;

    for (unsigned i = 0; i < SHA256_BLOCK_SIZE; i++) k_block[i] = 0;
    if (key_len > SHA256_BLOCK_SIZE) {
        sha256(key, key_len, k_block);
    } else {
        const uint8_t *p = (const uint8_t *)key;
        for (size_t i = 0; i < key_len; i++) k_block[i] = p[i];
    }

    for (unsigned i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad[i] = k_block[i] ^ 0x36;
        opad[i] = k_block[i] ^ 0x5c;
    }

    sha256_init(&ctx);
    sha256_update(&ctx, ipad, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, msg, msg_len);
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, inner, SHA256_DIGEST_SIZE);
    sha256_final(&ctx, out);
}

int hmac_sha256_equal(const uint8_t a[HMAC_SHA256_SIZE],
                      const uint8_t b[HMAC_SHA256_SIZE])
{
    uint8_t diff = 0;
    for (unsigned i = 0; i < HMAC_SHA256_SIZE; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}
