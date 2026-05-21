/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#ifndef _VFS_TYPES_H_
#define _VFS_TYPES_H_

/*
 * vfs_types.h — wire-stable types for the VFS MIG subsystem (#220).
 *
 * Imported by vfs.defs and by every fs_server / libvfs translation unit.
 * Do not put libc-specific types here: this header is consumed by the
 * MIG-generated stubs that link into nostdlib environments (kernel-side
 * helpers, bootstrap, etc.).
 */

#include <stdint.h>
#include <stddef.h>     /* size_t */

/*
 * MIG ctype mappings — vfs.defs declares vfs_u32_t/vfs_u64_t to keep the
 * generated user/server stubs free of stdint.h dependencies; the typedefs
 * below make them resolve to the obvious widths in C consumers.
 */
typedef uint32_t        vfs_u32_t;
typedef uint64_t        vfs_u64_t;

/*
 * vfs_path_t — fixed-width C string buffer matching the vfs.defs
 * declaration `c_string[*:1024]`.  Servers receive paths in a buffer
 * of this exact size.
 */
typedef char            vfs_path_t[1024];

/*
 * vfs_buf_t / vfs_dirent_array_t — out-of-line byte buffers (Mach
 * pointer_t on the wire).  Typedef so the MIG-generated stubs that
 * mention vfs_buf_t / vfs_dirent_array_t in their prototypes compile.
 */
#include <mach/vm_types.h>
typedef pointer_t       vfs_buf_t;
typedef pointer_t       vfs_dirent_array_t;

/* The libvfs public C API lives at the bottom of this file, after the
 * wire structures (vfs_stat_t / vfs_dirent_t) it references. */

/* ------------------------------------------------------------------ */
/*  Library version (BSD-style major.minor.patch)                      */
/* ------------------------------------------------------------------ */

#define LIBVFS_VERSION_MAJOR    0
#define LIBVFS_VERSION_MINOR    4
#define LIBVFS_VERSION_PATCH    0
#define LIBVFS_VERSION_STRING   "0.4.0"

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define VFS_PATH_MAX            1024    /* matches vfs_path_t in vfs.defs  */
#define VFS_NAME_MAX            255     /* dirent name, NUL-terminated     */
#define VFS_DIRENT_MAX          64      /* per readdir RPC                 */

/*
 * vfs_handle_t — opaque, server-assigned cookie returned by fs_open.
 * The fs_server is free to encode (inode << 32 | gen) or anything else
 * that lets it find the open file in O(1).  libvfs treats it as opaque.
 * Value 0 is reserved (never returned on success).
 */
typedef uint64_t vfs_handle_t;

/*
 * File type tag — returned by fs_open and fs_stat so libvfs can decide
 * the right syscall surface (regular vs dir vs special).  Mirrors POSIX
 * S_IFMT bits but uses a small numeric tag to stay arch-independent.
 */
#define VFS_FT_UNKNOWN          0
#define VFS_FT_REG              1       /* regular file                */
#define VFS_FT_DIR              2       /* directory                   */
#define VFS_FT_SYMLINK          3       /* symbolic link               */
#define VFS_FT_CHAR             4       /* character device            */
#define VFS_FT_BLOCK            5       /* block device                */
#define VFS_FT_FIFO             6       /* named pipe                  */
#define VFS_FT_SOCK             7       /* unix socket                 */

/*
 * fs_open flags (subset of POSIX O_*).  Stable wire constants — do not
 * reuse libc O_* numbers, those may differ per arch / libc.
 */
#define VFS_O_RDONLY            0x0001
#define VFS_O_WRONLY            0x0002
#define VFS_O_RDWR              0x0003
#define VFS_O_ACCMODE           0x0003

#define VFS_O_CREAT             0x0010
#define VFS_O_EXCL              0x0020
#define VFS_O_TRUNC             0x0040
#define VFS_O_APPEND            0x0080
#define VFS_O_DIRECTORY         0x0100  /* require target to be a dir   */
#define VFS_O_NOFOLLOW          0x0200  /* don't follow trailing symlink */

/*
 * Error codes returned in MIG kern_return_t.  Reuse Mach KERN_* where
 * the meaning matches; introduce a separate VFS_ERR_* range otherwise.
 * libvfs maps these to POSIX errno on the way up.
 *
 * The base 0x10000 is chosen above any KERN_* defined today; if the
 * Mach kernel ever extends KERN_* we can shift, but the .defs RetCode
 * helpers below always use the symbolic form.
 */
#define VFS_ERR_BASE            0x10000
#define VFS_ERR_NOENT           (VFS_ERR_BASE +  1)     /* no such file    */
#define VFS_ERR_EXIST           (VFS_ERR_BASE +  2)     /* already exists  */
#define VFS_ERR_NOTDIR          (VFS_ERR_BASE +  3)     /* path component  */
#define VFS_ERR_ISDIR           (VFS_ERR_BASE +  4)     /* op needs a file */
#define VFS_ERR_INVAL           (VFS_ERR_BASE +  5)     /* bad arg         */
#define VFS_ERR_NOSPC           (VFS_ERR_BASE +  6)     /* fs full         */
#define VFS_ERR_NAMETOOLONG     (VFS_ERR_BASE +  7)     /* > VFS_NAME_MAX  */
#define VFS_ERR_PERM            (VFS_ERR_BASE +  8)     /* permission      */
#define VFS_ERR_BADHANDLE       (VFS_ERR_BASE +  9)     /* unknown handle  */
#define VFS_ERR_NOTEMPTY        (VFS_ERR_BASE + 10)     /* rmdir non-empty */
#define VFS_ERR_XDEV            (VFS_ERR_BASE + 11)     /* cross-fs op     */
#define VFS_ERR_IO              (VFS_ERR_BASE + 12)     /* underlying I/O  */

/* ------------------------------------------------------------------ */
/*  Wire structures                                                    */
/* ------------------------------------------------------------------ */

/*
 * vfs_stat_t — wire-stable stat record.  Keep field order and widths
 * fixed; never insert in the middle, only append.  Times are seconds
 * since the Unix epoch (nsec resolution reserved for v0.2).
 */
typedef struct vfs_stat {
    uint64_t    st_ino;             /* inode / object id               */
    uint64_t    st_size;            /* size in bytes                   */
    uint64_t    st_blocks;          /* allocated 512-byte blocks       */
    uint64_t    st_atime_sec;       /* last access                     */
    uint64_t    st_mtime_sec;       /* last data modification          */
    uint64_t    st_ctime_sec;       /* last metadata change            */
    uint32_t    st_mode;            /* POSIX mode bits (perms only)    */
    uint32_t    st_nlink;           /* hard link count                 */
    uint32_t    st_uid;
    uint32_t    st_gid;
    uint32_t    st_blksize;         /* preferred I/O block size        */
    uint8_t     st_type;            /* VFS_FT_*                        */
    uint8_t     _pad[11];           /* reserved, brings sizeof to 80   */
} vfs_stat_t;
/* sizeof(vfs_stat_t) == 80 bytes == 20 * uint32_t (matches vfs.defs). */

/*
 * vfs_dirent_t — single readdir entry.  Fixed-size for a fixed-size
 * MIG array; the trailing name is NUL-terminated and at most
 * VFS_NAME_MAX + 1 bytes including the NUL.  cookie is opaque — the
 * caller passes it back to fs_readdir to resume.
 */
typedef struct vfs_dirent {
    uint64_t    d_ino;
    uint64_t    d_cookie;
    uint8_t     d_type;             /* VFS_FT_*                         */
    uint8_t     d_namelen;          /* strlen(d_name), excluding NUL    */
    uint8_t     _pad[6];
    char        d_name[VFS_NAME_MAX + 1];
} vfs_dirent_t;

/*
 * The libvfs public client API (vfs_open / vfs_read / ...) lives in a
 * separate libvfs.h header.  vfs_types.h is the wire / kernel-side
 * contract — it's safe to include from both client tasks and from
 * fs_servers, and intentionally does NOT pull in the client wrappers
 * which would collide with the MIG-generated server impl symbols
 * (vfs_open vs vfs_open client wrapper).
 */

#endif /* _VFS_TYPES_H_ */
