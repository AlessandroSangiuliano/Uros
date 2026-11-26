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
 *          (urmach_cap_verify / urmach_cap_revoke / urmach_cap_register).
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

    /*
     * A buffer the kernel allocated for DMA (#432).
     *
     * 🔑 THE RESOURCE IS THE BUFFER AND THE ID IS ITS REGION ID, which is a
     * number the kernel mints and never repeats.  It is the one resource type
     * here whose instances the POLICY cannot know in advance -- a manifest can
     * name a device class or a mount point, and it cannot name a buffer that
     * will exist for four milliseconds -- so the manifest says whether a task
     * may hold buffer capabilities at all, and the kernel says whether this
     * particular one is that task's to give away.
     */
    RESOURCE_DMA_BUFFER    = 10,

    /*
     * Revocation notices (#744): whether a task may ask cap_server to tell it
     * of every capability revoked.  A PERMISSION, which a manifest grants and
     * cap_subscribe_revoke() checks, and not a capability: no token of this
     * type is issued, and cap_acquire() refuses one.  It was anyone's, into
     * one of eight slots, and a resource server that found them taken heard
     * of no revocation.
     */
    RESOURCE_REVOKE_NOTICES = 11,

    /*
     * One PCI function (#982): the id is its bus address, bus<<8 | slot<<3 |
     * func, as the device master's routines take it, and the operations are
     * RESOURCE_PCI_DEVICE's.  No manifest names an address -- see below why a
     * policy file cannot -- so a manifest writes this type with `any' only, in
     * the line of a task that starts drivers, and the address is written by
     * that task, at run time, into the manifest of the driver it starts.
     */
    RESOURCE_PCI_FUNCTION  = 12,

    /*
     * The drivers the HAL starts (#964): whether a task may stop, start or
     * update the driver of a device, through the HAL's stop_driver(),
     * start_driver() and update_driver().  The id is the device's class, as
     * RESOURCE_PCI_DEVICE's is, so that a manifest can say which kinds of
     * device a task administers -- the network cards and not the disks --
     * which a bus address could not say in a file written before the machine
     * exists.  The HAL checks it against the class of the device a request
     * names.
     *
     * ⚠️ Below RESOURCE_TYPE_MAX, where it must stay: cap_server asks a
     * manifest only of the types below it, and issues the others to anyone.
     */
    RESOURCE_DRIVER_ADMIN  = 13,

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

/*
 * RESOURCE_PCI_DEVICE (#432).
 *
 * 🔴 THE RESOURCE ID IS THE DEVICE CLASS, NOT THE BUS ADDRESS, and that is the
 * decision this namespace exists to record.  A bus/device/function is a
 * property of the BOARD -- this tree's AHCI is 00:04.0 with one command line
 * and 00:1f.2 with another -- so a policy naming one would have to be
 * rewritten per machine, which is a policy file that is wrong somewhere by
 * construction.  A class code is a property of the device KIND: one manifest,
 * every machine.
 *
 * 🔑 Which makes the manifest half of a two-part check, deliberately.  It says
 * what KIND of device a driver may drive; it cannot say which instance,
 * because it does not know what is plugged in.  The other half is the device
 * master's claim -- a device has one driver, first come -- and neither alone
 * is a policy.
 *
 * 🔑 #982: and the instance is named at run time, by whoever knows what is
 * plugged in and starts the driver -- the HAL.  It derives, for the driver it
 * starts, a RESOURCE_PCI_FUNCTION capability for that one device, out of its
 * own `delegatable' line for the type, written with `any'.  So a manifest still
 * names kinds and never an instance, and a driver started that way can claim
 * its own device and no other, where a class capability lets it claim any
 * unclaimed device of the class.  The operations below are that type's too.
 *
 * The class is the upper 24 bits of the PCI class-code register: base class,
 * sub-class and programming interface, as configuration space reports them.
 *
 *	0x010601	SATA, AHCI 1.0		0x010000	SCSI
 *	0x020000	Ethernet		0x0c0330	USB xHCI
 */
#define CAP_OP_PCI_DMA_MAP   0x0000000000000001ULL  /* map memory for it   */
#define CAP_OP_PCI_MMIO_MAP  0x0000000000000002ULL  /* map its registers   */
#define CAP_OP_PCI_IRQ       0x0000000000000004ULL  /* claim its interrupt */

/*
 * RESOURCE_DMA_BUFFER (#432).  What the HOLDER of the capability may ask a
 * device to do with the buffer -- not what the owner may do with it, which is
 * ordinary memory and needs no capability at all.
 */
#define CAP_OP_DMA_DEVICE_READ   0x0000000000000001ULL  /* device reads it  */
#define CAP_OP_DMA_DEVICE_WRITE  0x0000000000000002ULL  /* device writes it */

/* RESOURCE_REVOKE_NOTICES (#744) */
#define CAP_OP_REVOKE_SUBSCRIBE  0x0000000000000001ULL  /* be told of them */

/* RESOURCE_DRIVER_ADMIN (#964): what may be asked of the HAL for a driver. */
#define CAP_OP_DRIVER_STOP       0x0000000000000001ULL  /* stop it            */
#define CAP_OP_DRIVER_START      0x0000000000000002ULL  /* start it again     */
#define CAP_OP_DRIVER_UPDATE     0x0000000000000004ULL  /* replace its module */

/*
 * RESOURCE_MOUNT_POINT (#716).  The resource is a path PREFIX in the name
 * server's mount registry, and its id is cap_name_hash() of the prefix
 * exactly as it is checked in -- "/", "/mnt/disk2", "/proc".  The name server
 * asks for it in netname_check_in_mount(), and nowhere else: the capability
 * decides who may put a mount under a prefix, the signature the mount was
 * checked in with decides who may take it out.
 */
#define CAP_OP_MOUNT_REGISTER    0x0000000000000001ULL  /* check a mount in */

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

    /*
     * The task the token was issued to, by the identity the kernel gives a
     * task when it creates it (#747).  It was a port name, MACH_PORT_NULL in
     * every token, and a 64-byte name, empty in every token: a field nobody
     * wrote, beside a hole the compiler dug in a different place on each
     * target (#415).
     */
    uint64_t  owner;

    /* delegation */
    uint64_t  parent_cap_id;
    uint8_t   depth;
    uint8_t   delegable;
    uint8_t   _pad1[6];

    /* lifetime */
    uint64_t  expires;              /* 0 = never */
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
 * And the claim above, made checkable (#415).
 *
 * "Field order and sizes are wire-format" is a promise the compiler is under
 * no obligation to keep, and it did not keep it: an alignment rule the two
 * targets disagree about moved half the structure on one of them, silently.
 * These two lines are what makes the promise hold by construction rather than
 * by anybody remembering to look -- the offset of the hmac is the one that
 * matters most, because the signature covers every byte before it, so a field
 * that moves invalidates every token ever minted.
 *
 * ⚠️ If either of these ever fails to compile, the format has changed.  That
 * is not a reason to update the number: it is a reason to decide whether the
 * change was intended and, if it was, to give the format a version.
 */
/*
 * 🔑 THE FORMAT'S VERSIONS, which these two lines hold:
 *
 *	1  192 bytes, the hmac at 160 (#415): a port name and a 64-byte name for
 *	   the owner, neither ever written, and a use count nothing read.
 *	2  120 bytes, the hmac at 88 (#747): the owner an identity of 64 bits,
 *	   and no use count -- counting operations is the quota's, not the
 *	   permission's.
 *
 * ⚠️ The version is said here and not carried in the token: within a boot one
 * cap_server mints every token with that boot's key, so a field naming the
 * format would only ever be compared with itself.  What these lines guard is
 * the build -- every program built from one tree agreeing on where each field
 * is.
 */
_Static_assert(sizeof(struct uros_cap) == 120,
	       "the capability token is a wire format and its size has moved");
_Static_assert(__builtin_offsetof(struct uros_cap, hmac) == 88,
	       "the signature covers every byte before the hmac, so the hmac's "
	       "offset is part of the format");

/*
 * MIG blob transport.  Clients and servers send/receive an opaque byte
 * array of this size; the layout above is private to libcap, cap_server,
 * and the UrMach kernel, which lets the struct evolve without churning
 * every .defs consumer.
 */
#define CAP_TOKEN_SIZE       (sizeof(struct uros_cap))

/*
 * cap_name_hash(), the resource id of a resource named by a string, is in a
 * header of its own so that the host's mkmanifest can include it (#716).
 */
#include <mach/cap_name_hash.h>

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
/* 2406 was CAP_ERR_EXHAUSTED, the use count's, which #747 took out: not reused. */
#define CAP_ERR_NOT_DELEGABLE     2407
#define CAP_ERR_DEPTH_EXCEEDED    2408
#define CAP_ERR_OP_NOT_ALLOWED    2409
#define CAP_ERR_RESOURCE_MISMATCH 2410
#define CAP_ERR_NO_MEMORY         2411
#define CAP_ERR_INTERNAL          2412
#define CAP_ERR_NOT_IN_MANIFEST   2413   /* v2.1 / #216: type+ops not declared */
#define CAP_ERR_MANIFEST_INVALID  2414   /* malformed .cmf blob */
#define CAP_ERR_NO_MANIFEST_SLOT  2415   /* cap_server manifest table full */
#define CAP_ERR_NOT_PROVISIONER   2416   /* #743: provisioning asked off the provisioning port */
#define CAP_ERR_NOTHING_TO_DERIVE 2417   /* #937: a derivation that keeps nothing takes no slot */
#define CAP_ERR_DERIVED_QUOTA     2418   /* #937: derived per-task ports hold their share of the table */
#define CAP_ERR_LINEAGE_FULL      2419   /* #937: the caller's family holds as many derived ports as it may */
#define CAP_ERR_TASK_NOT_IDENTITY 2420   /* #938: a task port handed over carries more than the task's identity */
#define CAP_ERR_NOT_OWNER         2421   /* #747: presented by a task the capability was not issued to */
#define CAP_ERR_NO_DELEGATE       2422   /* #747: cap_derive_for named a port no task receives on */

/*
 * Manifest binary format (v2.1 / #216) lives in its own header so
 * it stays usable from the host-side mkmanifest tool, which can't
 * pull in the arch-specific Mach chain.
 */
#include <mach/cap_manifest.h>

#endif /* _MACH_CAP_TYPES_H_ */
