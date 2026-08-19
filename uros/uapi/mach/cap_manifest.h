/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * mach/cap_manifest.h — Uros capability manifest binary format
 * (#216 v2.1).  Compiled at host build time by mkmanifest from a
 * plain-text source, consumed at boot by cap_server.
 *
 * Stand-alone on purpose: this header pulls in nothing except
 * <stdint.h> so the host-side mkmanifest tool can include it
 * without dragging in target-arch Mach headers.  The matching
 * runtime API (cap_manifest_validate, cap_manifest_allows, ...)
 * lives in cap_server's tree, not here.
 */

#ifndef _MACH_CAP_MANIFEST_H_
#define _MACH_CAP_MANIFEST_H_

#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Wire format                                                         */
/* ------------------------------------------------------------------ */

#define CAP_MANIFEST_MAGIC      0x464D4355u   /* 'UCMF' little-endian */
#define CAP_MANIFEST_VERSION    1u

/*
 * Maximum sane sizes — cap_server uses these to bound a single
 * blob without unbounded recursion or allocation.  16 KiB is
 * plenty for a TLV manifest; a server declaring hundreds of cap
 * types needs a redesign, not a bigger blob.
 */
#define CAP_MANIFEST_MAX_BYTES   (16 * 1024)
#define CAP_MANIFEST_MAX_NAME    64
#define CAP_MANIFEST_MAX_ENTRIES 64

/*
 * One declared capability requirement or delegation right.
 *
 *   type  — cap_resource_type_t enum value (see <mach/cap_types.h>)
 *   ops   — uint64 bitmask, per-type namespace
 *
 * sizeof == 16; 8-byte aligned so a blob written by mkmanifest on a
 * 64-bit host parses identically on the i386 target.
 */
typedef struct cap_manifest_entry {
    uint32_t  type;
    uint32_t  _pad;
    uint64_t  ops;
} cap_manifest_entry_t;

/*
 * Compiled manifest blob layout:
 *
 *   +--------------------------------+ offset 0
 *   | cap_manifest_header_t          |
 *   +--------------------------------+ required_offset
 *   | required[required_count]       |
 *   +--------------------------------+ delegatable_offset
 *   | delegatable[delegatable_count] |
 *   +--------------------------------+ name_offset
 *   | server name (NUL-terminated)   |
 *   +--------------------------------+ name_offset + name_length
 *   | zero pad up to total_size      |
 *   +--------------------------------+ total_size
 *
 * All offsets are byte offsets from the start of the blob.  Offsets
 * + counts are validated by cap_server against total_size before
 * any read; mismatch -> CAP_ERR_MANIFEST_INVALID.
 *
 * Growing the header: append new offset/count pairs and bump
 * header_size.  Old cap_server ignores the trailing fields; new
 * cap_server skips them when running against an old blob whose
 * header_size is smaller.  version bumps only on incompatible
 * format changes (e.g. entry width change).
 */
typedef struct cap_manifest_header {
    uint32_t  magic;            /* CAP_MANIFEST_MAGIC */
    uint16_t  version;          /* CAP_MANIFEST_VERSION */
    uint16_t  header_size;      /* sizeof(this struct) — forward compat */
    uint32_t  total_size;       /* total blob size, for bounds checking */
    uint32_t  flags;            /* reserved, must be 0 */

    uint32_t  name_offset;
    uint32_t  name_length;      /* including the trailing NUL */

    uint32_t  required_offset;
    uint32_t  required_count;

    uint32_t  delegatable_offset;
    uint32_t  delegatable_count;

    uint32_t  _reserved[2];     /* future: signature_offset, manifest_id */
} cap_manifest_header_t;

/*
 * The two sizes above, made checkable (#415).
 *
 * This format is written by mkmanifest ON A 64-BIT HOST and parsed by
 * cap_server on the target, which is the one case in this system where a
 * producer and a consumer really are different toolchains -- so it is the one
 * where "sizeof == 16" being true matters most, and it was said in a comment.
 * The entry is the sharper of the two: header_size travels in the blob and an
 * old server can skip a header it does not know, but the entry width is
 * implicit in every offset+count the header carries, and the format note says
 * so -- "version bumps only on incompatible format changes (e.g. entry width
 * change)".  A width change nobody noticed would not bump anything.
 */
_Static_assert(sizeof(cap_manifest_entry_t) == 16,
	       "a manifest entry's width is implicit in every offset and count "
	       "in the header; changing it is a format version bump");
_Static_assert(sizeof(cap_manifest_header_t) == 48,
	       "the manifest header is a wire format; growing it means "
	       "appending fields and bumping header_size, not moving them");

/*
 * MIG/C interop: cap_server.defs declares
 *   type cap_manifest_blob_t = array[*:16384] of char
 * which generates a `cap_manifest_blob_t buf` parameter in the user
 * and server stubs.  The C compiler resolves the name via this
 * typedef, same pattern as cap_token_t in <mach/cap_types.h>.
 */
typedef char cap_manifest_blob_t[CAP_MANIFEST_MAX_BYTES];

#endif /* _MACH_CAP_MANIFEST_H_ */
