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
 * File:    mach/cap_types.h
 * Purpose: Uros capability token layout, resource types, op bitmasks,
 *          and error codes.  Shared between userspace (cap_server,
 *          libcap, client tasks) and the UrMach kernel fast-path
 *          (urmach_cap_verify / urmach_cap_use / urmach_cap_revoke /
 *          urmach_cap_register).
 *
 * Design: /home/slex/Documenti/uros_docs/Uros_tecnico_v6.pdf §2
 */

#ifndef _MACH_CAP_TYPES_H_
#define _MACH_CAP_TYPES_H_

#include <mach/mach_types.h>
#include <mach/kern_return.h>
#include <stdint.h>

/*
 * Size constants.
 */
#define CAP_HMAC_SIZE        32      /* HMAC-SHA256 output */
#define CAP_OWNER_NAME_LEN   64      /* stable task name */
#define CAP_MAX_DEPTH        16      /* max delegation tree depth */
#define CAP_TOKEN_MAX        256     /* MIG wire upper bound for cap_token_t */

/*
 * MIG/C interop types.  cap_server.defs declares cap_u64_t, cap_u32_t and
 * cap_token_t at the MIG level with matching C ctype names; the MIG stub
 * generator emits parameter declarations using those names, so the
 * generated .c files need typedefs in scope.  Keep in sync with
 * cap_server.defs.
 */
typedef uint64_t cap_u64_t;
typedef uint32_t cap_u32_t;
typedef char     cap_token_t[CAP_TOKEN_MAX];

/*
 * Resource types (resource_type field).  v1 only emits RESOURCE_BLK_DEVICE;
 * the remaining values are reserved so the enum is stable when later
 * servers begin issuing them.
 */
typedef enum {
    RESOURCE_NONE          = 0,
    RESOURCE_FILE          = 1,
    RESOURCE_DIRECTORY     = 2,
    RESOURCE_MOUNT_POINT   = 3,
    RESOURCE_BLK_DEVICE    = 4,
    RESOURCE_PCI_DEVICE    = 5,
    RESOURCE_PRINTER       = 6,
    RESOURCE_SERIAL        = 7,
    RESOURCE_FRAMEBUFFER   = 8,
    RESOURCE_BUDGET        = 9,
    RESOURCE_TYPE_MAX
} cap_resource_type_t;

/*
 * Operation bitmasks (allowed_ops field).  Per-resource-type namespace:
 * the same bit value means different things under different resource
 * types.  v1 only defines the block-device ops.
 */

/* RESOURCE_BLK_DEVICE */
#define CAP_OP_BLK_READ      0x0000000000000001ULL
#define CAP_OP_BLK_WRITE     0x0000000000000002ULL
#define CAP_OP_BLK_IOCTL     0x0000000000000004ULL

/* RESOURCE_FILE (reserved for v2) */
#define CAP_OP_FILE_READ     0x0000000000000001ULL
#define CAP_OP_FILE_WRITE    0x0000000000000002ULL
#define CAP_OP_FILE_EXEC     0x0000000000000004ULL
#define CAP_OP_FILE_STAT     0x0000000000000008ULL

/*
 * Capability token.  Autoverifying: HMAC-SHA256 over every preceding
 * field binds the payload to the kernel's cap_hmac_key, so any server
 * can validate a token with a single syscall (urmach_cap_verify) without
 * RPC back to cap_server.
 *
 * Layout constraints:
 *   - hmac[] MUST be the last field (the HMAC input is the preceding
 *     bytes verbatim).
 *   - Field order and sizes are wire-format for the opaque blob
 *     transport used by MIG (cap_token_t).
 */
struct uros_cap {
    /* identity */
    uint64_t  cap_id;
    uint32_t  resource_type;        /* cap_resource_type_t */
    uint32_t  _pad0;
    uint64_t  resource_id;

    /* rights */
    uint64_t  allowed_ops;

    /* ownership & delegation */
    mach_port_t owner;
    char      owner_name[CAP_OWNER_NAME_LEN];
    uint64_t  parent_cap_id;
    uint8_t   depth;
    uint8_t   delegable;
    uint8_t   _pad1[6];

    /* lifetime */
    uint64_t  expires;              /* 0 = never */
    uint32_t  max_uses;              /* 0 = unlimited, 1 = use-once */
    uint32_t  current_uses;          /* atomic fetch_add */
    uint8_t   revoked;
    uint8_t   is_generic;
    uint8_t   _pad2[6];

    /* left-child right-sibling delegation tree */
    uint64_t  first_child_cap_id;
    uint64_t  next_sibling_cap_id;

    /* verification (MUST be last — HMAC input is every preceding byte) */
    uint8_t   hmac[CAP_HMAC_SIZE];
};

/*
 * MIG blob transport.  Clients and servers send/receive an opaque byte
 * array of this size; the layout above is private to libcap, cap_server,
 * and the UrMach kernel, which lets the struct evolve without churning
 * every .defs consumer.
 */
#define CAP_TOKEN_SIZE       (sizeof(struct uros_cap))

/*
 * Error codes.  These live in the KERN_RETURN space (positive ints).
 * Keep in sync with libcap error translation and cap_server reply paths.
 */
#define CAP_ERR_NONE              0
#define CAP_ERR_UNAUTHORIZED      2401
#define CAP_ERR_POLICY_DENIED     2402
#define CAP_ERR_INVALID_TOKEN     2403
#define CAP_ERR_EXPIRED           2404
#define CAP_ERR_REVOKED           2405
#define CAP_ERR_EXHAUSTED         2406   /* current_uses >= max_uses */
#define CAP_ERR_NOT_DELEGABLE     2407
#define CAP_ERR_DEPTH_EXCEEDED    2408
#define CAP_ERR_OP_NOT_ALLOWED    2409
#define CAP_ERR_RESOURCE_MISMATCH 2410
#define CAP_ERR_NO_MEMORY         2411
#define CAP_ERR_INTERNAL          2412

#endif /* _MACH_CAP_TYPES_H_ */
