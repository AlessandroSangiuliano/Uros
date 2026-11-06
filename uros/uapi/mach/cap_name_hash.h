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
 * File:    mach/cap_name_hash.h
 * Purpose: the resource id of a resource named by a string.
 *
 * 🔑 A HEADER OF ITS OWN, WITH NOTHING IN IT THAT NEEDS THE TARGET (#716).
 * The id is computed on two sides that must agree to the bit: by whoever
 * names the resource -- a manifest compiled on the host by mkmanifest, a
 * client asking cap_server for a token -- and by the server that verifies,
 * from its own copy of the name.  <mach/cap_types.h> needs the target's
 * headers, so the host tool could not include it, and a second copy of the
 * hash there would be two halves free to disagree.  This is the one copy:
 * cap_types.h includes it for everyone who has always called it there.
 */

#ifndef _MACH_CAP_NAME_HASH_H_
#define _MACH_CAP_NAME_HASH_H_

#include <stdint.h>

/*
 * FNV-1a 64-bit hash of a device/resource name.  Used to derive a
 * stable resource_id for capabilities issued on named resources like
 * block-device partitions ("ahci0a", "virtio_blk0a", ...) and mount
 * points ("/", "/proc", #716).  Clients ask cap_server to sign a token
 * with id = cap_name_hash(name), and servers recompute the same hash on
 * their local copy of the name when verifying — so the agreement is on
 * the string, not on a database of numeric ids.
 */
static inline uint64_t
cap_name_hash(const char *name)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*name) {
        h ^= (uint8_t)(*name++);
        h *= 0x100000001b3ULL;
    }
    return h;
}

#endif /* _MACH_CAP_NAME_HASH_H_ */
