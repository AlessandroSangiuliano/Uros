/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef _LIBVFS_INTERNAL_H_
#define _LIBVFS_INTERNAL_H_

/*
 * libvfs_internal.h — fd-table internals shared between libvfs.c (the
 * core Mach data path) and vfs_flipc.c (the optional FLIPC v2 fast-path,
 * #232/#234).  Not exported to libvfs consumers.
 *
 * The FLIPC fast-path lives in its own translation unit so it can be
 * left out of the umbrella libc.so (#234): the dynamic linker's reads go
 * straight to Mach, and untrusted dynamic apps get no shared-memory
 * channel into an fs_server (see memory uros-vision: fast paths are
 * capability-gated, not bundled into everyone's libc).  libvfs.c always
 * calls the seam below; the build decides whether the real vfs_flipc.c
 * or the no-op vfs_flipc_stub.c provides it.
 */

#include "libvfs.h"          /* vfs_fd_t, vfs_u64_t, vfs_u32_t, size_t, ssize_t */
#include "vfs_flipc.h"       /* VFS_FLIPC_MAX_READ (protocol #defines only) */
#include <mach.h>            /* mach_port_t, vm_offset_t */
#include <pthread.h>         /* pthread_mutex_t */

#define VFS_MAX_FDS              64

/* Read-ahead / write-behind window size.  Used by vfs_flipc.c to size the
 * buffers and by libvfs.c's close path to free them. */
#define VFS_PREFETCH_SIZE        VFS_FLIPC_MAX_READ

struct vfs_fd_entry {
    int             in_use;
    int             dead;        /* fs_server died — every op returns EIO */
    mach_port_t     fs_port;
    vfs_u64_t       handle;
    vfs_u64_t       offset;
    int             flags;
    uint8_t         type;       /* VFS_FT_* */
    /* FLIPC v2 read-ahead prefetch window (#232).  A large FLIPC fetch
     * fills pf_buf; subsequent sequential reads are served by memcpy
     * with no IPC.  pf_buf == 0 means no window allocated yet.  These
     * fields are touched only by vfs_flipc.c; they sit zero in builds
     * (umbrella) that drop the fast-path. */
    vm_offset_t     pf_buf;
    vfs_u64_t       pf_start;   /* file offset of pf_buf[0]            */
    vfs_u32_t       pf_len;     /* valid bytes in pf_buf (0 = empty)   */
    /* FLIPC v2 write-behind buffer (#232).  Contiguous sequential
     * writes coalesce here and flush as one large FLIPC write; flushed
     * before any read/stat/seek/sync/close so the file stays coherent. */
    vm_offset_t     wb_buf;
    vfs_u64_t       wb_start;   /* file offset of wb_buf[0]            */
    vfs_u32_t       wb_len;     /* buffered bytes (0 = empty)          */
};

/* fd table — defined in libvfs.c, shared with vfs_flipc.c. */
extern struct vfs_fd_entry vfs_fds[VFS_MAX_FDS];
extern pthread_mutex_t     vfs_lock;

/* Resolve a fd to its entry (NULL if out of range / not open).  Caller
 * must hold vfs_lock for any field access. */
struct vfs_fd_entry *vfs_fd_get(vfs_fd_t fd);

/* ------------------------------------------------------------------ */
/*  FLIPC fast-path seam (#234)                                         */
/*                                                                      */
/*  Provided by vfs_flipc.c (real) in the static path, or by            */
/*  vfs_flipc_stub.c (no-ops) in the umbrella libc.so build.  libvfs.c  */
/*  calls these unconditionally; a -1 / 0 return means "nothing done,   */
/*  use the Mach path".                                                 */
/* ------------------------------------------------------------------ */

/* Serve a read from the FLIPC prefetch window.  Returns bytes read
 * (>= 0, owns the fd offset update) or -1 to fall back to Mach. */
ssize_t vfs_flipc_read_fastpath(vfs_fd_t fd, mach_port_t fs_port,
                                vfs_u64_t handle, void *buf, size_t count);

/* Coalesce a write into the write-behind buffer.  Returns bytes accepted
 * (>= 0, owns the fd offset update) or -1 to fall back to Mach. */
ssize_t vfs_flipc_write_fastpath(vfs_fd_t fd, mach_port_t fs_port,
                                 vfs_u64_t handle, const void *buf,
                                 size_t count);

/* Flush a fd's pending write-behind buffer (no-op when empty / no FLIPC).
 * Called before any op that must see a coherent file. */
int vfs_flipc_wb_flush(vfs_fd_t fd);

#endif /* _LIBVFS_INTERNAL_H_ */
