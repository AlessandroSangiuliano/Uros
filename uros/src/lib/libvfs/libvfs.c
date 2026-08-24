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
 * Threading: protected by a single global mutex.  Hot-path sharding
 * (per-fd or rwlock) is deferred — fd table operations are well below
 * filesystem RPC cost so contention is negligible.
 *
 * v0.2.0 (#230): dead-name resilience.  When an fs_server dies its
 * cached send right becomes a dead name; the next fs RPC fails with
 * MACH_SEND_INVALID_DEST / MIG_SERVER_DIED.  libvfs catches that,
 * evicts the stale mount cache entry and marks every fd routed to that
 * server broken (subsequent ops return EIO-class -1 until closed).
 * vfs_open() / vfs_stat() retry once through a fresh name_server resolve
 * so a respawned server is picked up transparently.
 */

#include "libvfs.h"
#include <mach.h>
#include <mach/message.h>
#include <mach/mig_errors.h>      /* MIG_SERVER_DIED */
#include <servers/netname.h>
#include <servers/netname_defs.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

/* MIG-generated user stubs (vfs.defs).  vfs.h is emitted in the build
 * directory; consumers of libvfs see only the public API in
 * vfs_types.h, so the include path stays an internal detail. */
#include "vfs.h"

/* fd-table internals + the FLIPC fast-path seam (#234).  The FLIPC code
 * itself lives in vfs_flipc.c (static path) / vfs_flipc_stub.c (umbrella
 * libc.so); libvfs.c only ever calls the seam. */
#include "libvfs_internal.h"

/* ------------------------------------------------------------------ */
/*  Internal state                                                     */
/* ------------------------------------------------------------------ */

/* VFS_MAX_FDS and struct vfs_fd_entry live in libvfs_internal.h (shared
 * with vfs_flipc.c). */
#define VFS_MAX_CACHED_MOUNTS    16

struct vfs_mount_cache_entry {
    int             in_use;
    /* ⚠️ Was `prefix[80]' with a comment saying it matched netname_name_t.
     * A comment is the one kind of check that cannot fail, and it was
     * wrong: the wire carried 128.  Now it is the same macro. */
    char            prefix[NETNAME_NAME_MAX];
    mach_port_t     fs_port;
};

/* fd table is shared with vfs_flipc.c → not static (see libvfs_internal.h). */
struct vfs_fd_entry                 vfs_fds[VFS_MAX_FDS];
pthread_mutex_t                     vfs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct vfs_mount_cache_entry vfs_mounts[VFS_MAX_CACHED_MOUNTS];
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

/* Look up the mount that owns 'path'.  We always query name_server
 * because the in-process cache cannot know whether a more-specific
 * mount has appeared since the last lookup — e.g. the cached "/" entry
 * would beat a later-registered "/proc" if we trusted the cache without
 * re-checking.  This always-query design is also what gives cross-task
 * mount coherence for free (a mount/umount in one task is visible to
 * every other within one RPC), so v0.2.0 (#230) did not need a
 * name_server change-notification channel.  Cost is one ~1.4 µs RPC per
 * vfs_open / vfs_stat — acceptable, since those are rare.  The cache is
 * maintained for diagnostics and as the unit dead-name eviction targets
 * (vfs_evict_port_locked). */
static mach_port_t
vfs_resolve_mount(const char *path)
{
    int i;
    netname_name_t matched;
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr;

    matched[0] = '\0';
    kr = netname_look_up_mount(name_server_port, (char *)path,
                               &port, matched);
    if (kr != NETNAME_SUCCESS || port == MACH_PORT_NULL)
        return MACH_PORT_NULL;

    /* Refresh-or-insert cache entry for matched prefix.
     *
     * netname_look_up_mount() hands us a fresh send-right reference on
     * every call.  When we already cache this mount, that new reference
     * is a duplicate of the one the cache holds (same port name, bumped
     * uref) — drop it, or every vfs_open() of an already-mounted path
     * leaks a send right and the IPC space grows without bound until the
     * task faults.  Found via the fd-lifecycle stress (#272): ~13 back-
     * to-back opens of one file were enough to corrupt the task. */
    for (i = 0; i < VFS_MAX_CACHED_MOUNTS; i++) {
        if (vfs_mounts[i].in_use
            && strcmp(vfs_mounts[i].prefix, matched) == 0) {
            if (port != vfs_mounts[i].fs_port)
                (void)mach_port_deallocate(mach_task_self(),
                                           vfs_mounts[i].fs_port);
            else
                (void)mach_port_deallocate(mach_task_self(), port);
            vfs_mounts[i].fs_port = port;
            return port;
        }
    }

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
/*  Dead-name handling (#230 v0.2.0)                                    */
/* ------------------------------------------------------------------ */

/*
 * True when a MIG fs RPC failed because the fs_server is gone: either
 * the send right is already a dead name (MACH_SEND_INVALID_DEST), or
 * the server died after we sent and the kernel turned our reply into a
 * send-once dead notification (MIG_SERVER_DIED).  In both cases the
 * mount this port came from is no longer serviceable.
 */
static int
vfs_send_died(kern_return_t kr)
{
    return kr == MACH_SEND_INVALID_DEST || kr == MIG_SERVER_DIED;
}

/*
 * The fs_server behind 'port' has died.  Drop every cache entry that
 * routed to it so the next vfs_open()/vfs_stat() re-queries name_server
 * (which, once the server respawns and re-checks-in its mount, hands
 * back the fresh send right).  We deliberately do NOT mach_port_deallocate
 * the dead name here: the same name integer may still sit in live fd
 * entries, and the netname move-send refcounting means one over-deallocate
 * would invalidate it for an unrelated holder.  A dead name is a bounded
 * leak (one per crashed server) — correctness beats reclaiming it.
 * Caller must hold vfs_lock.
 */
static void
vfs_evict_port_locked(mach_port_t port)
{
    int i;
    if (port == MACH_PORT_NULL)
        return;
    for (i = 0; i < VFS_MAX_CACHED_MOUNTS; i++)
        if (vfs_mounts[i].in_use && vfs_mounts[i].fs_port == port)
            vfs_mounts[i].in_use = 0;
}

/*
 * Mark every open fd bound to the dead 'port' broken and evict its mount
 * cache entries.  Subsequent ops on those fds short-circuit to EIO until
 * the client closes them; a fresh vfs_open() then routes through a
 * respawned server.  Caller must hold vfs_lock.
 */
static void
vfs_mark_port_dead_locked(mach_port_t port)
{
    int i;
    for (i = 0; i < VFS_MAX_FDS; i++)
        if (vfs_fds[i].in_use && vfs_fds[i].fs_port == port)
            vfs_fds[i].dead = 1;
    vfs_evict_port_locked(port);
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

struct vfs_fd_entry *
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

    /*
     * Resolve + open, retrying once if the chosen fs_server turns out to
     * be dead.  vfs_resolve_mount always queries name_server, so the
     * retry picks up a respawned server's fresh send right after we evict
     * the stale cache entry.  One retry is enough: a second death means
     * the mount is genuinely gone.
     */
    int attempt;
    for (attempt = 0; attempt < 2; attempt++) {
        pthread_mutex_lock(&vfs_lock);
        fs_port = vfs_resolve_mount(path);
        if (fs_port == MACH_PORT_NULL) {
            pthread_mutex_unlock(&vfs_lock);
            return VFS_FD_INVALID;
        }
        /* Drop the lock across the RPC: the fs_server can be slow and
         * holding the libvfs mutex through a kernel transition would
         * serialise every client thread.  fs_port is a Mach send right
         * with its own refcount, safe to use without the lock. */
        pthread_mutex_unlock(&vfs_lock);

        /* #385: hand the fs_server a send right to our task so it can
         * reclaim this open's server-side fid if we die without close()
         * (e.g. SIGKILL) — the right turns into a dead name on our death. */
        kr = fs_open(fs_port, mach_task_self(), (char *)path, flags, mode,
                     &handle, &type);
        if (!vfs_send_died(kr))
            break;
        /* Server died — evict the stale route and try a fresh resolve. */
        pthread_mutex_lock(&vfs_lock);
        vfs_mark_port_dead_locked(fs_port);
        pthread_mutex_unlock(&vfs_lock);
    }
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
    vfs_fds[fd].dead    = 0;
    vfs_fds[fd].fs_port = fs_port;
    vfs_fds[fd].handle  = handle;
    vfs_fds[fd].offset  = 0;
    vfs_fds[fd].flags   = flags;
    vfs_fds[fd].type    = (uint8_t)type;
    vfs_fds[fd].pf_buf  = 0;
    vfs_fds[fd].pf_start = 0;
    vfs_fds[fd].pf_len  = 0;
    vfs_fds[fd].wb_buf  = 0;
    vfs_fds[fd].wb_start = 0;
    vfs_fds[fd].wb_len  = 0;
    pthread_mutex_unlock(&vfs_lock);
    return fd;
}

int
vfs_close(vfs_fd_t fd)
{
    struct vfs_fd_entry *e;
    mach_port_t fs_port;
    vfs_u64_t handle;
    int dead;

    /* Flush any buffered writes before tearing the fd down (#232). */
    (void)vfs_flipc_wb_flush(fd);

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (!e) {
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    fs_port = e->fs_port;
    handle  = e->handle;
    dead    = e->dead;
    e->in_use = 0;
    e->dead   = 0;
    {
        /* Release the prefetch + write-behind windows (#232). */
        vm_offset_t pf = e->pf_buf;
        vm_offset_t wb = e->wb_buf;
        e->pf_buf = 0;
        e->pf_len = 0;
        e->wb_buf = 0;
        e->wb_len = 0;
        if (pf)
            (void)vm_deallocate(mach_task_self(), pf, VFS_PREFETCH_SIZE);
        if (wb)
            (void)vm_deallocate(mach_task_self(), wb, VFS_PREFETCH_SIZE);
    }
    pthread_mutex_unlock(&vfs_lock);

    /* A dead fd has no live server to tell; freeing the slot is the whole
     * job.  close() always succeeds from the client's point of view. */
    if (!dead)
        (void)fs_close(fs_port, handle);
    return 0;
}

/*
 * Flush every fd's write-behind buffer.  libposix-uros calls this before
 * execve hands the fd table to a new image (the buffer lives in this
 * task's heap and would otherwise be lost across exec).
 */
void
vfs_flush_all(void)
{
    int i;
    for (i = 0; i < VFS_MAX_FDS; i++) {
        int pending;
        pthread_mutex_lock(&vfs_lock);
        pending = vfs_fds[i].in_use && vfs_fds[i].wb_len > 0;
        pthread_mutex_unlock(&vfs_lock);
        if (pending)
            (void)vfs_flipc_wb_flush((vfs_fd_t)i);
    }
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
    if (!e || e->dead) {
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    fs_port = e->fs_port;
    handle  = e->handle;
    offset  = e->offset;
    pthread_mutex_unlock(&vfs_lock);

    /* #232: a read must see prior buffered writes — flush write-behind. */
    (void)vfs_flipc_wb_flush(fd);

    /* #232/#234: serve reads from the FLIPC pipelined read-ahead window
     * when the fast-path is present (vfs_flipc.c) and the server offers
     * it.  Returns >= 0 (and owns the offset update) on success; -1 means
     * fall through to the Mach fs_read path below.  In the umbrella
     * libc.so the no-op stub always returns -1 → Mach path. */
    {
        ssize_t fr = vfs_flipc_read_fastpath(fd, fs_port, handle, buf, count);
        if (fr >= 0)
            return fr;
    }

    kr = fs_read(fs_port, handle, offset, (vfs_u32_t)count,
                 &data, &data_count);
    if (vfs_send_died(kr)) {
        pthread_mutex_lock(&vfs_lock);
        vfs_mark_port_dead_locked(fs_port);
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
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
    if (!e || e->dead) {
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    fs_port = e->fs_port;
    handle  = e->handle;
    offset  = e->offset;
    pthread_mutex_unlock(&vfs_lock);

    /* #232/#234: coalesce into the FLIPC write-behind buffer and emit as
     * few large channel writes.  Returns >= 0 (owns the offset update) or
     * -1 to fall through to the Mach fs_write below.  Umbrella libc.so
     * stub always returns -1 → Mach path. */
    {
        ssize_t wr = vfs_flipc_write_fastpath(fd, fs_port, handle, buf, count);
        if (wr >= 0)
            return wr;
    }

    /* MIG OOL transfers expect to own the buffer, but our caller's
     * 'buf' may live on the stack or be reused.  Pass a copy by value
     * via the inline pointer_t form: MIG copies it for us. */
    kr = fs_write(fs_port, handle, offset, (pointer_t)buf,
                  (mach_msg_type_number_t)count, &written);
    if (vfs_send_died(kr)) {
        pthread_mutex_lock(&vfs_lock);
        vfs_mark_port_dead_locked(fs_port);
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    if (kr != KERN_SUCCESS)
        return -1;

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (e) {
        e->offset += written;
        e->pf_len  = 0;        /* #232: content changed — drop prefetch */
    }
    pthread_mutex_unlock(&vfs_lock);
    return (ssize_t)written;
}

off_t
vfs_lseek(vfs_fd_t fd, off_t offset, int whence)
{
    struct vfs_fd_entry *e;
    off_t new_off = -1;
    vfs_stat_t st;

    /* #232: flush buffered writes so the seek base / future ops are
     * coherent (a jump also ends the contiguous write-behind run). */
    (void)vfs_flipc_wb_flush(fd);

    /* SEEK_END requires a stat for the file size; do it before grabbing
     * the lock to avoid an RPC under the mutex. */
    if (whence == VFS_SEEK_END) {
        if (vfs_fstat(fd, &st) != 0)
            return -1;
    }

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (!e || e->dead) {
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

    /* Resolve + stat, retrying once through a fresh resolve if the
     * fs_server proved dead (see vfs_open for the rationale). */
    int attempt;
    for (attempt = 0; attempt < 2; attempt++) {
        pthread_mutex_lock(&vfs_lock);
        fs_port = vfs_resolve_mount(path);
        pthread_mutex_unlock(&vfs_lock);
        if (fs_port == MACH_PORT_NULL)
            return -1;

        kr = fs_stat(fs_port, (char *)path, out);
        if (!vfs_send_died(kr))
            break;
        pthread_mutex_lock(&vfs_lock);
        vfs_mark_port_dead_locked(fs_port);
        pthread_mutex_unlock(&vfs_lock);
    }
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

    /* #232: flush buffered writes so st_size reflects them. */
    (void)vfs_flipc_wb_flush(fd);

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (!e || e->dead) {
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    fs_port = e->fs_port;
    handle  = e->handle;
    pthread_mutex_unlock(&vfs_lock);

    kr = fs_fstat(fs_port, handle, out);
    if (vfs_send_died(kr)) {
        pthread_mutex_lock(&vfs_lock);
        vfs_mark_port_dead_locked(fs_port);
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    return (kr == KERN_SUCCESS) ? 0 : -1;
}

int
vfs_sync(vfs_fd_t fd)
{
    struct vfs_fd_entry *e;
    mach_port_t fs_port;
    kern_return_t kr;

    /* #232: push buffered writes to the server before syncing. */
    (void)vfs_flipc_wb_flush(fd);

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (!e || e->dead) {
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    fs_port = e->fs_port;
    pthread_mutex_unlock(&vfs_lock);

    kr = fs_sync(fs_port);
    if (vfs_send_died(kr)) {
        pthread_mutex_lock(&vfs_lock);
        vfs_mark_port_dead_locked(fs_port);
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    return (kr == KERN_SUCCESS) ? 0 : -1;
}

/*
 * vfs_mmap — request a Mach memory_object backing this fd's file
 * from the fs_server.  Caller hands the returned send right to
 * vm_map; the kernel will then route faults to the fs server via
 * libfspager (#276 Phase B).
 *
 * The fs_port + handle are read under vfs_lock just like every other
 * op.  Flush the write-behind window first so the memory object
 * starts from a coherent on-server state (matters once we wire
 * MAP_SHARED in Phase D; harmless before).
 */
int
vfs_mmap(vfs_fd_t fd, int prot, int flags, mach_port_t *out_port)
{
    struct vfs_fd_entry *e;
    mach_port_t fs_port;
    vfs_u64_t handle;
    mach_port_t mem_obj = MACH_PORT_NULL;
    kern_return_t kr;

    if (!out_port)
        return -1;

    (void)vfs_flipc_wb_flush(fd);

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (!e || e->dead) {
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    fs_port = e->fs_port;
    handle  = e->handle;
    pthread_mutex_unlock(&vfs_lock);

    kr = fs_mmap(fs_port, handle,
                 (vfs_u32_t)prot, (vfs_u32_t)flags, &mem_obj);
    if (vfs_send_died(kr)) {
        pthread_mutex_lock(&vfs_lock);
        vfs_mark_port_dead_locked(fs_port);
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    if (kr != KERN_SUCCESS || mem_obj == MACH_PORT_NULL)
        return -1;

    *out_port = mem_obj;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  fork inheritance support (#262 step 2)                              */
/* ------------------------------------------------------------------ */

/*
 * Collect the distinct fs_server send-right names behind every live
 * (in-use, non-dead) open fd into 'ports'.  Returns the count written,
 * capped at 'max'.  posix_fork.c uses this to re-insert each right into
 * the forked child's IPC space: the child's CoW'd vfs fd table holds
 * these port NAMES but they resolve to nothing until the rights are
 * inserted under the same names.  Distinct because several fds can share
 * one mount's fs_port and inserting a name twice is wasted work.
 */
int
vfs_enum_open_ports(mach_port_t *ports, int max)
{
    int n = 0;
    int i, j;

    if (!ports || max <= 0)
        return 0;

    pthread_mutex_lock(&vfs_lock);
    for (i = 0; i < VFS_MAX_FDS && n < max; i++) {
        mach_port_t p;
        int dup = 0;
        if (!vfs_fds[i].in_use || vfs_fds[i].dead)
            continue;
        p = vfs_fds[i].fs_port;
        if (p == MACH_PORT_NULL)
            continue;
        for (j = 0; j < n; j++)
            if (ports[j] == p) { dup = 1; break; }
        if (!dup)
            ports[n++] = p;
    }
    pthread_mutex_unlock(&vfs_lock);
    return n;
}

/*
 * Read out the persistent state of one open fd (#262 step 3, execve
 * handoff).  Returns 0 on success, -1 if the fd is invalid/dead.
 */
int
vfs_export_fd(vfs_fd_t fd, mach_port_t *port, vfs_u64_t *handle,
              vfs_u64_t *offset, int *flags, uint8_t *type)
{
    struct vfs_fd_entry *e;

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (!e || e->dead) {
        pthread_mutex_unlock(&vfs_lock);
        return -1;
    }
    if (port)   *port   = e->fs_port;
    if (handle) *handle = e->handle;
    if (offset) *offset = e->offset;
    if (flags)  *flags  = e->flags;
    if (type)   *type   = e->type;
    pthread_mutex_unlock(&vfs_lock);
    return 0;
}

/*
 * Recreate an fd entry from previously-exported state (#262 step 3).
 * 'port' must already be a valid send right in this task's IPC space
 * (the execve handoff inserts it before calling this).  Returns the new
 * vfs_fd_t, or VFS_FD_INVALID if the table is full.
 */
vfs_fd_t
vfs_import_fd(mach_port_t port, vfs_u64_t handle, vfs_u64_t offset,
              int flags, uint8_t type)
{
    vfs_fd_t fd;

    vfs_init();
    pthread_mutex_lock(&vfs_lock);
    fd = vfs_fd_alloc();
    if (fd == VFS_FD_INVALID) {
        pthread_mutex_unlock(&vfs_lock);
        return VFS_FD_INVALID;
    }
    vfs_fds[fd].in_use  = 1;
    vfs_fds[fd].dead    = 0;
    vfs_fds[fd].fs_port = port;
    vfs_fds[fd].handle  = handle;
    vfs_fds[fd].offset  = offset;
    vfs_fds[fd].flags   = flags;
    vfs_fds[fd].type    = type;
    pthread_mutex_unlock(&vfs_lock);
    return fd;
}

/* ------------------------------------------------------------------ */
/*  Namespace operations (v0.3.0, #231)                                */
/* ------------------------------------------------------------------ */

#define VFS_COPY_CHUNK  8192

int
vfs_unlink(const char *path)
{
    mach_port_t fs_port;
    kern_return_t kr = KERN_FAILURE;
    int attempt;

    if (!path || path[0] != '/')
        return -1;

    vfs_init();

    for (attempt = 0; attempt < 2; attempt++) {
        pthread_mutex_lock(&vfs_lock);
        fs_port = vfs_resolve_mount(path);
        pthread_mutex_unlock(&vfs_lock);
        if (fs_port == MACH_PORT_NULL)
            return -1;

        kr = fs_unlink(fs_port, (char *)path);
        if (!vfs_send_died(kr))
            break;
        pthread_mutex_lock(&vfs_lock);
        vfs_mark_port_dead_locked(fs_port);
        pthread_mutex_unlock(&vfs_lock);
    }
    return (kr == KERN_SUCCESS) ? 0 : -1;
}

int
vfs_copy(const char *src, const char *dst)
{
    vfs_fd_t s, d;
    char buf[VFS_COPY_CHUNK];
    ssize_t n;
    int rc = 0;

    if (!src || !dst || src[0] != '/' || dst[0] != '/')
        return -1;

    s = vfs_open(src, VFS_O_RDONLY, 0);
    if (s == VFS_FD_INVALID)
        return -1;
    d = vfs_open(dst, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, 0644);
    if (d == VFS_FD_INVALID) {
        vfs_close(s);
        return -1;
    }

    for (;;) {
        n = vfs_read(s, buf, sizeof buf);
        if (n < 0) { rc = -1; break; }
        if (n == 0) break;                  /* EOF */
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = vfs_write(d, buf + off, (size_t)(n - off));
            if (w <= 0) { rc = -1; break; }
            off += w;
        }
        if (rc != 0) break;
    }

    vfs_close(s);
    vfs_close(d);
    return rc;
}

int
vfs_rename(const char *oldpath, const char *newpath)
{
    mach_port_t po, pn;
    kern_return_t kr = KERN_FAILURE;
    int attempt;

    if (!oldpath || !newpath || oldpath[0] != '/' || newpath[0] != '/')
        return -1;

    vfs_init();

    for (attempt = 0; attempt < 2; attempt++) {
        pthread_mutex_lock(&vfs_lock);
        po = vfs_resolve_mount(oldpath);
        pn = vfs_resolve_mount(newpath);
        pthread_mutex_unlock(&vfs_lock);
        if (po == MACH_PORT_NULL || pn == MACH_PORT_NULL)
            return -1;

        /* Different mounts can't share an atomic rename — POSIX returns
         * EXDEV; we synthesize it as copy + unlink of the source. */
        if (po != pn) {
            if (vfs_copy(oldpath, newpath) != 0)
                return -1;
            return vfs_unlink(oldpath);
        }

        kr = fs_rename(po, (char *)oldpath, (char *)newpath);
        if (!vfs_send_died(kr))
            break;
        pthread_mutex_lock(&vfs_lock);
        vfs_mark_port_dead_locked(po);
        pthread_mutex_unlock(&vfs_lock);
    }
    return (kr == KERN_SUCCESS) ? 0 : -1;
}
