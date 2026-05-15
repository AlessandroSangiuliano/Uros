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

/*
 * libvfs.c — client-side VFS dispatcher (#220 v0.1).
 *
 * libvfs runs in every task that does file I/O.  It maintains:
 *   - a per-task fd table (handle / offset / type / fs_port)
 *   - an in-process mount cache (path-prefix -> fs_server send right)
 *
 * Path resolution: vfs_open() picks the best mount via
 * netname_look_up_mount() (longest segment-aligned prefix), caches the
 * (prefix, port) pair, then delegates the actual open to the chosen
 * fs_server through the MIG fs_open RPC.
 *
 * Threading: protected by a single global mutex for v0.1.  Hot-path
 * sharding (per-fd or rwlock) is a v0.2 concern — fd table operations
 * are well below filesystem RPC cost so contention is negligible.
 */

#include "libvfs.h"
#include <mach.h>
#include <mach/message.h>
#include <servers/netname.h>
#include <servers/netname_defs.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

/* MIG-generated user stubs (vfs.defs).  vfs.h is emitted in the build
 * directory; consumers of libvfs see only the public API in
 * vfs_types.h, so the include path stays an internal detail. */
#include "vfs.h"

/* ------------------------------------------------------------------ */
/*  Internal state                                                     */
/* ------------------------------------------------------------------ */

#define VFS_MAX_FDS              64
#define VFS_MAX_CACHED_MOUNTS    16

struct vfs_fd_entry {
    int             in_use;
    mach_port_t     fs_port;
    vfs_u64_t       handle;
    vfs_u64_t       offset;
    int             flags;
    uint8_t         type;       /* VFS_FT_* */
};

struct vfs_mount_cache_entry {
    int             in_use;
    char            prefix[80]; /* matches netname_name_t */
    mach_port_t     fs_port;
};

static struct vfs_fd_entry          vfs_fds[VFS_MAX_FDS];
static struct vfs_mount_cache_entry vfs_mounts[VFS_MAX_CACHED_MOUNTS];
static pthread_mutex_t              vfs_lock = PTHREAD_MUTEX_INITIALIZER;
static int                          vfs_initialised;

/* ------------------------------------------------------------------ */
/*  Mount resolution                                                   */
/* ------------------------------------------------------------------ */

/* Same logic as the server-side mount_prefix_match: prefix must end at
 * a '/' boundary in path (or path's end), so /mnt/d does not match
 * /mnt/disk1.  Returns matched length or 0. */
static size_t
vfs_match_prefix(const char *path, const char *prefix)
{
    size_t pl = strlen(prefix);
    size_t patl = strlen(path);

    if (pl > patl)
        return 0;
    if (strncmp(path, prefix, pl) != 0)
        return 0;
    if (pl == 1 && prefix[0] == '/')
        return 1;
    if (path[pl] == '\0' || path[pl] == '/')
        return pl;
    return 0;
}

/* Look up the mount that owns 'path'.  Tries the in-process cache
 * first; on miss, asks name_server and inserts the result.  Returns
 * MACH_PORT_NULL if no mount matches (caller surfaces VFS_FD_INVALID). */
static mach_port_t
vfs_resolve_mount(const char *path)
{
    int i;
    size_t best_len = 0;
    mach_port_t best_port = MACH_PORT_NULL;
    netname_name_t matched;
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr;

    /* Cache lookup (longest prefix wins). */
    for (i = 0; i < VFS_MAX_CACHED_MOUNTS; i++) {
        if (!vfs_mounts[i].in_use)
            continue;
        size_t mlen = vfs_match_prefix(path, vfs_mounts[i].prefix);
        if (mlen > best_len) {
            best_len  = mlen;
            best_port = vfs_mounts[i].fs_port;
        }
    }
    if (best_port != MACH_PORT_NULL)
        return best_port;

    /* Cache miss: ask name_server. */
    matched[0] = '\0';
    kr = netname_look_up_mount(name_server_port, (char *)path,
                               &port, matched);
    if (kr != NETNAME_SUCCESS || port == MACH_PORT_NULL)
        return MACH_PORT_NULL;

    /* Insert into the first free slot.  v0.1: no eviction; if we run
     * out of slots, future lookups bypass the cache and pay the
     * name_server RPC every time — correct, just slower. */
    for (i = 0; i < VFS_MAX_CACHED_MOUNTS; i++) {
        if (!vfs_mounts[i].in_use) {
            vfs_mounts[i].in_use = 1;
            vfs_mounts[i].fs_port = port;
            (void)strncpy(vfs_mounts[i].prefix, matched,
                          sizeof(vfs_mounts[i].prefix) - 1);
            vfs_mounts[i].prefix[sizeof(vfs_mounts[i].prefix) - 1] = '\0';
            break;
        }
    }
    return port;
}

/* ------------------------------------------------------------------ */
/*  fd table helpers                                                   */
/* ------------------------------------------------------------------ */

static vfs_fd_t
vfs_fd_alloc(void)
{
    int i;
    for (i = 0; i < VFS_MAX_FDS; i++)
        if (!vfs_fds[i].in_use)
            return (vfs_fd_t)i;
    return VFS_FD_INVALID;
}

static struct vfs_fd_entry *
vfs_fd_get(vfs_fd_t fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS)
        return (struct vfs_fd_entry *)0;
    if (!vfs_fds[fd].in_use)
        return (struct vfs_fd_entry *)0;
    return &vfs_fds[fd];
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int
vfs_init(void)
{
    pthread_mutex_lock(&vfs_lock);
    if (!vfs_initialised) {
        memset(vfs_fds, 0, sizeof(vfs_fds));
        memset(vfs_mounts, 0, sizeof(vfs_mounts));
        vfs_initialised = 1;
    }
    pthread_mutex_unlock(&vfs_lock);
    return 0;
}

vfs_fd_t
vfs_open(const char *path, int flags, int mode)
{
    mach_port_t fs_port;
    vfs_u64_t   handle = 0;
    vfs_u32_t   type   = VFS_FT_UNKNOWN;
    vfs_fd_t    fd;
    kern_return_t kr;

    if (!path || path[0] != '/')
        return VFS_FD_INVALID;

    vfs_init();

    pthread_mutex_lock(&vfs_lock);
    fs_port = vfs_resolve_mount(path);
    if (fs_port == MACH_PORT_NULL) {
        pthread_mutex_unlock(&vfs_lock);
        return VFS_FD_INVALID;
    }

    /* Drop the lock across the RPC: the fs_server can be slow and
     * holding the libvfs mutex through a kernel transition would
     * serialise every client thread.  Re-acquire only to mutate the
     * fd table.  fs_port is a Mach send right with refcount, safe to
     * use without the lock; the cache entry can't be removed today. */
    pthread_mutex_unlock(&vfs_lock);
    kr = fs_open(fs_port, (char *)path, flags, mode, &handle, &type);
    if (kr != KERN_SUCCESS || handle == 0)
        return VFS_FD_INVALID;

    pthread_mutex_lock(&vfs_lock);
    fd = vfs_fd_alloc();
    if (fd == VFS_FD_INVALID) {
        pthread_mutex_unlock(&vfs_lock);
        /* Out of fd slots — close the just-opened handle so the
         * fs_server doesn't leak its per-open state. */
        (void)fs_close(fs_port, handle);
        return VFS_FD_INVALID;
    }
    vfs_fds[fd].in_use  = 1;
    vfs_fds[fd].fs_port = fs_port;
    vfs_fds[fd].handle  = handle;
    vfs_fds[fd].offset  = 0;
    vfs_fds[fd].flags   = flags;
    vfs_fds[fd].type    = (uint8_t)type;
    pthread_mutex_unlock(&vfs_lock);
    return fd;
}

int
vfs_close(vfs_fd_t fd)
{
    struct vfs_fd_entry *e;
    mach_port_t fs_port;
    vfs_u64_t handle;

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (!e) {
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    fs_port = e->fs_port;
    handle  = e->handle;
    e->in_use = 0;
    pthread_mutex_unlock(&vfs_lock);

    (void)fs_close(fs_port, handle);
    return 0;
}

ssize_t
vfs_read(vfs_fd_t fd, void *buf, size_t count)
{
    struct vfs_fd_entry *e;
    mach_port_t fs_port;
    vfs_u64_t handle;
    vfs_u64_t offset;
    pointer_t data = 0;
    mach_msg_type_number_t data_count = 0;
    kern_return_t kr;

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (!e) {
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    fs_port = e->fs_port;
    handle  = e->handle;
    offset  = e->offset;
    pthread_mutex_unlock(&vfs_lock);

    kr = fs_read(fs_port, handle, offset, (vfs_u32_t)count,
                 &data, &data_count);
    if (kr != KERN_SUCCESS)
        return -1;
    if (data_count > 0 && data) {
        memcpy(buf, (void *)data, data_count);
        vm_deallocate(mach_task_self(), (vm_address_t)data,
                      (vm_size_t)data_count);
    }

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (e)
        e->offset += data_count;
    pthread_mutex_unlock(&vfs_lock);
    return (ssize_t)data_count;
}

ssize_t
vfs_write(vfs_fd_t fd, const void *buf, size_t count)
{
    struct vfs_fd_entry *e;
    mach_port_t fs_port;
    vfs_u64_t handle;
    vfs_u64_t offset;
    vfs_u32_t written = 0;
    kern_return_t kr;

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (!e) {
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    fs_port = e->fs_port;
    handle  = e->handle;
    offset  = e->offset;
    pthread_mutex_unlock(&vfs_lock);

    /* MIG OOL transfers expect to own the buffer, but our caller's
     * 'buf' may live on the stack or be reused.  Pass a copy by value
     * via the inline pointer_t form: MIG copies it for us. */
    kr = fs_write(fs_port, handle, offset, (pointer_t)buf,
                  (mach_msg_type_number_t)count, &written);
    if (kr != KERN_SUCCESS)
        return -1;

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (e)
        e->offset += written;
    pthread_mutex_unlock(&vfs_lock);
    return (ssize_t)written;
}

off_t
vfs_lseek(vfs_fd_t fd, off_t offset, int whence)
{
    struct vfs_fd_entry *e;
    off_t new_off = -1;
    vfs_stat_t st;

    /* SEEK_END requires a stat for the file size; do it before grabbing
     * the lock to avoid an RPC under the mutex. */
    if (whence == VFS_SEEK_END) {
        if (vfs_fstat(fd, &st) != 0)
            return -1;
    }

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (!e) {
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    switch (whence) {
    case VFS_SEEK_SET:  new_off = offset;                      break;
    case VFS_SEEK_CUR:  new_off = (off_t)e->offset + offset;   break;
    case VFS_SEEK_END:  new_off = (off_t)st.st_size + offset;  break;
    default:
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    if (new_off < 0) {
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    e->offset = (vfs_u64_t)new_off;
    pthread_mutex_unlock(&vfs_lock);
    return new_off;
}

int
vfs_stat(const char *path, vfs_stat_t *out)
{
    mach_port_t fs_port;
    kern_return_t kr;

    if (!path || !out || path[0] != '/')
        return -1;

    vfs_init();

    pthread_mutex_lock(&vfs_lock);
    fs_port = vfs_resolve_mount(path);
    pthread_mutex_unlock(&vfs_lock);
    if (fs_port == MACH_PORT_NULL)
        return -1;

    kr = fs_stat(fs_port, (char *)path, out);
    return (kr == KERN_SUCCESS) ? 0 : -1;
}

int
vfs_fstat(vfs_fd_t fd, vfs_stat_t *out)
{
    struct vfs_fd_entry *e;
    mach_port_t fs_port;
    vfs_u64_t handle;
    kern_return_t kr;

    if (!out)
        return -1;

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (!e) {
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    fs_port = e->fs_port;
    handle  = e->handle;
    pthread_mutex_unlock(&vfs_lock);

    kr = fs_fstat(fs_port, handle, out);
    return (kr == KERN_SUCCESS) ? 0 : -1;
}

int
vfs_sync(vfs_fd_t fd)
{
    struct vfs_fd_entry *e;
    mach_port_t fs_port;
    kern_return_t kr;

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (!e) {
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    fs_port = e->fs_port;
    pthread_mutex_unlock(&vfs_lock);

    kr = fs_sync(fs_port);
    return (kr == KERN_SUCCESS) ? 0 : -1;
}
