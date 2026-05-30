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
 * File:    kern/hmac_sha256.h
 * Purpose: Kernel-side HMAC-SHA256 (RFC 2104).  Used by urmach_cap_verify
 *          and urmach_cap_use to validate capability tokens against the
 *          cap_hmac_key installed by cap_server at boot.
 */

#ifndef _KERN_HMAC_SHA256_H_
#define _KERN_HMAC_SHA256_H_

#include <kern/sha256.h>

#define HMAC_SHA256_SIZE    SHA256_DIGEST_SIZE

void hmac_sha256(const void *key, size_t key_len,
                 const void *msg, size_t msg_len,
                 uint8_t out[HMAC_SHA256_SIZE]);

int  hmac_sha256_equal(const uint8_t a[HMAC_SHA256_SIZE],
                       const uint8_t b[HMAC_SHA256_SIZE]);

#endif /* _KERN_HMAC_SHA256_H_ */
