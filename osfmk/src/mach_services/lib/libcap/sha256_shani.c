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
 * Issue #180: SHA-256 compress using Intel SHA Extensions (SHA-NI).
 *
 * Uses compiler intrinsics gated on a per-function target attribute so
 * the file links unconditionally; the dispatcher in sha256.c only calls
 * sha256_compress_shani() after CPUID confirms the extensions exist.
 *
 * Algorithm follows the standard SHA-NI block layout (Intel SHA
 * Extensions whitepaper / ISA reference):
 *   STATE0 holds {C, D, G, H}
 *   STATE1 holds {A, B, E, F}
 * with one SHA256RNDS2 instruction performing two rounds and producing
 * the next two state words.  MSG0..MSG3 hold the 16-word working
 * message schedule; SHA256MSG1/MSG2 produce the next quartet of
 * schedule words once the original block has been consumed.
 */

#include "sha256.h"

/*
 * immintrin.h declares the SHA intrinsics under a per-function target;
 * the definitions are valid even when -march does not enable SHA, as
 * long as every function that uses them carries the attribute below.
 */
/* See kern/sha256_shani.c — stub mm_malloc.h before immintrin.h. */
#define _MM_MALLOC_H_INCLUDED
#include <immintrin.h>

#define SHA_NI_TARGET __attribute__((target("sha,ssse3,sse4.1")))

SHA_NI_TARGET
void sha256_compress_shani(struct sha256_ctx *ctx, const uint8_t block[64])
{
    __m128i STATE0, STATE1;
    __m128i MSG, TMP;
    __m128i MSG0, MSG1, MSG2, MSG3;
    __m128i ABEF_SAVE, CDGH_SAVE;

    /*
     * Byte-swap mask: SHA-256 input words are big-endian, but XMM lanes
     * are little-endian on x86, so each 32-bit word loaded from memory
     * needs its bytes reversed before being treated as a message word.
     * Mask layout (low to high byte index): 03 02 01 00 | 07 06 05 04 |
     * 0b 0a 09 08 | 0f 0e 0d 0c.
     */
    const __m128i BSWAP_MASK = _mm_set_epi64x(
        (long long)0x0c0d0e0f08090a0bULL,
        (long long)0x0405060700010203ULL);

    /*
     * Load state[0..3] = ABCD and state[4..7] = EFGH (host order).
     * Re-pack into the SHA-NI native arrangement:
     *   STATE0 = [C, D, G, H]  (low to high lane: H, G, D, C)
     *   STATE1 = [A, B, E, F]  (low to high lane: F, E, B, A)
     */
    TMP    = _mm_loadu_si128((const __m128i *)&ctx->state[0]);  /* [A,B,C,D] */
    STATE1 = _mm_loadu_si128((const __m128i *)&ctx->state[4]);  /* [E,F,G,H] */

    TMP    = _mm_shuffle_epi32(TMP, 0xB1);      /* [B,A,D,C] */
    STATE1 = _mm_shuffle_epi32(STATE1, 0x1B);   /* [H,G,F,E] */
    STATE0 = _mm_alignr_epi8(TMP, STATE1, 8);   /* [F,E,B,A] = ABEF */
    STATE1 = _mm_blend_epi16(STATE1, TMP, 0xF0);/* [H,G,D,C] = CDGH */

    ABEF_SAVE = STATE0;
    CDGH_SAVE = STATE1;

    /* ---------- Rounds 0-3 ---------- */
    MSG  = _mm_loadu_si128((const __m128i *)(block +  0));
    MSG0 = _mm_shuffle_epi8(MSG, BSWAP_MASK);
    MSG  = _mm_add_epi32(MSG0, _mm_set_epi64x(
        (long long)0xE9B5DBA5B5C0FBCFULL,
        (long long)0x71374491428A2F98ULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG    = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

    /* ---------- Rounds 4-7 ---------- */
    MSG  = _mm_loadu_si128((const __m128i *)(block + 16));
    MSG1 = _mm_shuffle_epi8(MSG, BSWAP_MASK);
    MSG  = _mm_add_epi32(MSG1, _mm_set_epi64x(
        (long long)0xAB1C5ED5923F82A4ULL,
        (long long)0x59F111F13956C25BULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG    = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG0   = _mm_sha256msg1_epu32(MSG0, MSG1);

    /* ---------- Rounds 8-11 ---------- */
    MSG  = _mm_loadu_si128((const __m128i *)(block + 32));
    MSG2 = _mm_shuffle_epi8(MSG, BSWAP_MASK);
    MSG  = _mm_add_epi32(MSG2, _mm_set_epi64x(
        (long long)0x550C7DC3243185BEULL,
        (long long)0x12835B01D807AA98ULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG    = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG1   = _mm_sha256msg1_epu32(MSG1, MSG2);

    /* ---------- Rounds 12-15 ---------- */
    MSG  = _mm_loadu_si128((const __m128i *)(block + 48));
    MSG3 = _mm_shuffle_epi8(MSG, BSWAP_MASK);
    MSG  = _mm_add_epi32(MSG3, _mm_set_epi64x(
        (long long)0xC19BF1749BDC06A7ULL,
        (long long)0x80DEB1FE72BE5D74ULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    TMP    = _mm_alignr_epi8(MSG3, MSG2, 4);
    MSG0   = _mm_add_epi32(MSG0, TMP);
    MSG0   = _mm_sha256msg2_epu32(MSG0, MSG3);
    MSG    = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
    MSG2   = _mm_sha256msg1_epu32(MSG2, MSG3);

    /*
     * Generic round-quartet: rotates message slots Mn / Mn-1 / Mn-2 /
     * Mn-3 and produces both the schedule words and the round state
     * updates for four SHA-256 rounds.
     */
#define ROUND_4(Ma, Mb, Mc, Md, K_HI, K_LO) do { \
    MSG    = _mm_add_epi32((Ma), _mm_set_epi64x((long long)(K_HI), \
                                                (long long)(K_LO))); \
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG); \
    TMP    = _mm_alignr_epi8((Ma), (Md), 4); \
    (Mb)   = _mm_add_epi32((Mb), TMP); \
    (Mb)   = _mm_sha256msg2_epu32((Mb), (Ma)); \
    MSG    = _mm_shuffle_epi32(MSG, 0x0E); \
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG); \
    (Md)   = _mm_sha256msg1_epu32((Md), (Ma)); \
} while (0)

    /* Rounds 16-19 */ ROUND_4(MSG0, MSG1, MSG2, MSG3,
        0x240CA1CC0FC19DC6ULL, 0xEFBE4786E49B69C1ULL);
    /* Rounds 20-23 */ ROUND_4(MSG1, MSG2, MSG3, MSG0,
        0x76F988DA5CB0A9DCULL, 0x4A7484AA2DE92C6FULL);
    /* Rounds 24-27 */ ROUND_4(MSG2, MSG3, MSG0, MSG1,
        0xBF597FC7B00327C8ULL, 0xA831C66D983E5152ULL);
    /* Rounds 28-31 */ ROUND_4(MSG3, MSG0, MSG1, MSG2,
        0x1429296706CA6351ULL, 0xD5A79147C6E00BF3ULL);
    /* Rounds 32-35 */ ROUND_4(MSG0, MSG1, MSG2, MSG3,
        0x53380D134D2C6DFCULL, 0x2E1B213827B70A85ULL);
    /* Rounds 36-39 */ ROUND_4(MSG1, MSG2, MSG3, MSG0,
        0x92722C8581C2C92EULL, 0x766A0ABB650A7354ULL);
    /* Rounds 40-43 */ ROUND_4(MSG2, MSG3, MSG0, MSG1,
        0xC76C51A3C24B8B70ULL, 0xA81A664BA2BFE8A1ULL);
    /* Rounds 44-47 */ ROUND_4(MSG3, MSG0, MSG1, MSG2,
        0x106AA070F40E3585ULL, 0xD6990624D192E819ULL);

    /*
     * Rounds 48-59: drop the SHA256MSG1 step in the last three quartets
     * (no further schedule needed) but keep SHA256MSG2 to finish
     * producing W[60..63].
     */
#define ROUND_4_NO_MSG1(Ma, Mb, Md, K_HI, K_LO) do { \
    MSG    = _mm_add_epi32((Ma), _mm_set_epi64x((long long)(K_HI), \
                                                (long long)(K_LO))); \
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG); \
    TMP    = _mm_alignr_epi8((Ma), (Md), 4); \
    (Mb)   = _mm_add_epi32((Mb), TMP); \
    (Mb)   = _mm_sha256msg2_epu32((Mb), (Ma)); \
    MSG    = _mm_shuffle_epi32(MSG, 0x0E); \
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG); \
} while (0)

    /* Rounds 48-51 */ ROUND_4_NO_MSG1(MSG0, MSG1, MSG3,
        0x34B0BCB52748774CULL, 0x1E376C0819A4C116ULL);
    /* Rounds 52-55 */ ROUND_4_NO_MSG1(MSG1, MSG2, MSG0,
        0x682E6FF35B9CCA4FULL, 0x4ED8AA4A391C0CB3ULL);
    /* Rounds 56-59 */ ROUND_4_NO_MSG1(MSG2, MSG3, MSG1,
        0x8CC7020884C87814ULL, 0x78A5636F748F82EEULL);

    /* ---------- Rounds 60-63 (no schedule needed) ---------- */
    MSG    = _mm_add_epi32(MSG3, _mm_set_epi64x(
        (long long)0xC67178F2BEF9A3F7ULL,
        (long long)0xA4506CEB90BEFFFAULL));
    STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
    MSG    = _mm_shuffle_epi32(MSG, 0x0E);
    STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

    /* Add the working state back into the saved chaining values. */
    STATE0 = _mm_add_epi32(STATE0, ABEF_SAVE);
    STATE1 = _mm_add_epi32(STATE1, CDGH_SAVE);

    /*
     * Re-pack STATE0 = ABEF, STATE1 = CDGH back into host-order
     * state[0..3] = ABCD and state[4..7] = EFGH.
     */
    TMP    = _mm_shuffle_epi32(STATE0, 0x1B);   /* [A,B,E,F] (low→high) */
    STATE1 = _mm_shuffle_epi32(STATE1, 0xB1);   /* [G,H,C,D] (low→high) */
    STATE0 = _mm_blend_epi16(TMP, STATE1, 0xF0);   /* [A,B,C,D] = ABCD */
    STATE1 = _mm_alignr_epi8(STATE1, TMP, 8);      /* [E,F,G,H] = EFGH */

    _mm_storeu_si128((__m128i *)&ctx->state[0], STATE0);
    _mm_storeu_si128((__m128i *)&ctx->state[4], STATE1);

#undef ROUND_4
#undef ROUND_4_NO_MSG1
}
