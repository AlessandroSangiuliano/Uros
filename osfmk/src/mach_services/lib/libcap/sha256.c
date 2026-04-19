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
 * SHA-256 per FIPS PUB 180-4.  Straightforward reference implementation:
 * big-endian block layout, standard message schedule, 64 rounds.
 * No table tricks, no SIMD — small and easy to audit.  The UrMach
 * fast-path target is ~200 ns for a single 128-byte block, which this
 * implementation achieves comfortably on i686 without AES-NI; AES-NI
 * and SHA-NI acceleration are tracked for a later issue.
 */

#include "sha256.h"

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_compress(struct sha256_ctx *ctx, const uint8_t block[64]) {
    uint32_t W[64];
    for (unsigned i = 0; i < 16; i++) {
        W[i] = ((uint32_t)block[4*i]   << 24)
             | ((uint32_t)block[4*i+1] << 16)
             | ((uint32_t)block[4*i+2] <<  8)
             | ((uint32_t)block[4*i+3]);
    }
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = rotr(W[i-15], 7)  ^ rotr(W[i-15], 18) ^ (W[i-15] >> 3);
        uint32_t s1 = rotr(W[i-2], 17)  ^ rotr(W[i-2], 19)  ^ (W[i-2] >> 10);
        W[i] = W[i-16] + s0 + W[i-7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1];
    uint32_t c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5];
    uint32_t g = ctx->state[6], h = ctx->state[7];

    for (unsigned i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + W[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(struct sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bitcount = 0;
    ctx->buflen = 0;
}

void sha256_update(struct sha256_ctx *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->bitcount += (uint64_t)len * 8;

    if (ctx->buflen) {
        size_t n = SHA256_BLOCK_SIZE - ctx->buflen;
        if (n > len) n = len;
        for (size_t i = 0; i < n; i++) ctx->buf[ctx->buflen + i] = p[i];
        ctx->buflen += n;
        p += n; len -= n;
        if (ctx->buflen == SHA256_BLOCK_SIZE) {
            sha256_compress(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
    while (len >= SHA256_BLOCK_SIZE) {
        sha256_compress(ctx, p);
        p += SHA256_BLOCK_SIZE;
        len -= SHA256_BLOCK_SIZE;
    }
    for (size_t i = 0; i < len; i++) ctx->buf[i] = p[i];
    ctx->buflen = len;
}

void sha256_final(struct sha256_ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE]) {
    uint64_t bits = ctx->bitcount;

    ctx->buf[ctx->buflen++] = 0x80;
    if (ctx->buflen > 56) {
        while (ctx->buflen < SHA256_BLOCK_SIZE) ctx->buf[ctx->buflen++] = 0;
        sha256_compress(ctx, ctx->buf);
        ctx->buflen = 0;
    }
    while (ctx->buflen < 56) ctx->buf[ctx->buflen++] = 0;

    ctx->buf[56] = (uint8_t)(bits >> 56);
    ctx->buf[57] = (uint8_t)(bits >> 48);
    ctx->buf[58] = (uint8_t)(bits >> 40);
    ctx->buf[59] = (uint8_t)(bits >> 32);
    ctx->buf[60] = (uint8_t)(bits >> 24);
    ctx->buf[61] = (uint8_t)(bits >> 16);
    ctx->buf[62] = (uint8_t)(bits >>  8);
    ctx->buf[63] = (uint8_t)(bits      );
    sha256_compress(ctx, ctx->buf);

    for (unsigned i = 0; i < 8; i++) {
        out[4*i]   = (uint8_t)(ctx->state[i] >> 24);
        out[4*i+1] = (uint8_t)(ctx->state[i] >> 16);
        out[4*i+2] = (uint8_t)(ctx->state[i] >>  8);
        out[4*i+3] = (uint8_t)(ctx->state[i]      );
    }
}

void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]) {
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}
