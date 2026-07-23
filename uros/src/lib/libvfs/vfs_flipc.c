/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * vfs_flipc.c — libvfs FLIPC v2 fast-path (#232), split out of libvfs.c
 * in #234.  Bulk reads/writes ride a shared-memory FLIPC v2 channel pair
 * instead of Mach RPC: read-ahead prefetch feeds many small reads from
 * one big fetch; write-behind coalesces sequential writes into few large
 * flushes.
 *
 * This whole translation unit is OPTIONAL.  The static system servers
 * link it and keep the fast-path.  The umbrella libc.so (#234) links the
 * no-op vfs_flipc_stub.c instead, so the dynamic linker reads via plain
 * Mach RPC and untrusted dynamic apps get no shared-memory channel into
 * an fs_server (capability-gated fast paths are post-0.1.0 design — see
 * memory uros-vision).  The seam (vfs_flipc_{read,write}_fastpath,
 * vfs_flipc_wb_flush) is declared in libvfs_internal.h; libvfs.c calls it
 * unconditionally.
 */

#include "libvfs.h"
#include "libvfs_internal.h"
#include <mach.h>
#include <string.h>

#include "vfs.h"             /* MIG stubs: fs_read/fs_write/fs_flipc_endpoint */

#include <flipc2.h>
/* vfs_flipc.h + VFS_PREFETCH_SIZE come in via libvfs_internal.h. */

struct vfs_flipc_conn {
    int                 in_use;
    int                 state;      /* 0 untried, 1 connected, 2 unavailable */
    mach_port_t         fs_port;
    flipc2_channel_t    fwd;        /* server -> client (we consume) */
    flipc2_channel_t    rev;        /* client -> server (we produce) */
    pthread_mutex_t     io_lock;    /* serialises one in-flight request */
    uint64_t            seq;
};

#define VFS_FLIPC_MAX_CONN  8
static struct vfs_flipc_conn vfs_flipc_conns[VFS_FLIPC_MAX_CONN];

/* Master switch — lets the benchmark A/B the FLIPC path against Mach on
 * the same vfs_read calls.  On by default. */
static int vfs_flipc_enabled = 1;

void
vfs_flipc_set_enabled(int on)
{
    vfs_flipc_enabled = on ? 1 : 0;
}

/*
 * Find (or lazily connect) the FLIPC channel for 'fs_port'.  Returns a
 * connected conn, or NULL when the server has no fast-path / connect
 * failed (caller stays on the Mach path).  Slot bookkeeping is under
 * vfs_lock; the connect handshake runs under the per-conn io_lock.
 */
static struct vfs_flipc_conn *
vfs_flipc_get(mach_port_t fs_port)
{
    int i, freeslot = -1;
    struct vfs_flipc_conn *c = NULL;

    pthread_mutex_lock(&vfs_lock);
    for (i = 0; i < VFS_FLIPC_MAX_CONN; i++) {
        if (vfs_flipc_conns[i].in_use &&
            vfs_flipc_conns[i].fs_port == fs_port) {
            c = &vfs_flipc_conns[i];
            break;
        }
        if (!vfs_flipc_conns[i].in_use && freeslot < 0)
            freeslot = i;
    }
    if (!c) {
        if (freeslot < 0) {
            pthread_mutex_unlock(&vfs_lock);
            return NULL;            /* table full — just use Mach */
        }
        c = &vfs_flipc_conns[freeslot];
        c->in_use  = 1;
        c->state   = 0;
        c->fs_port = fs_port;
        c->seq     = 0;
        pthread_mutex_init(&c->io_lock, NULL);
    }
    pthread_mutex_unlock(&vfs_lock);

    if (c->state == 1)
        return c;
    if (c->state == 2)
        return NULL;

    /* Untried: resolve the endpoint name and connect, once. */
    pthread_mutex_lock(&c->io_lock);
    if (c->state == 0) {
        vfs_path_t name;
        int res = -1;
        kern_return_t kr = fs_flipc_endpoint(fs_port, name, &res);
        if (kr == KERN_SUCCESS && res == 0 && name[0]) {
            flipc2_channel_t fwd = 0, rev = 0;
            if (flipc2_endpoint_connect(name, 0, 0, &fwd, &rev)
                == FLIPC2_SUCCESS) {
                c->fwd = fwd;
                c->rev = rev;
                c->state = 1;
            } else {
                c->state = 2;
            }
        } else {
            c->state = 2;
        }
    }
    pthread_mutex_unlock(&c->io_lock);
    return (c->state == 1) ? c : NULL;
}

/*
 * One FLIPC read round-trip.  Returns bytes read (>= 0, 0 = EOF) on
 * success, or -1 to tell the caller to fall back to the Mach path
 * (channel broke or the server reported an error).
 */
static ssize_t
vfs_flipc_read(struct vfs_flipc_conn *c, vfs_u64_t handle,
               vfs_u64_t offset, void *buf, size_t count)
{
    ssize_t ret = -1;
    struct flipc2_desc *req, *rep;
    uint64_t want = count;

    if (want > VFS_FLIPC_MAX_READ)
        want = VFS_FLIPC_MAX_READ;

    pthread_mutex_lock(&c->io_lock);

    req = flipc2_produce_wait(c->rev, FLIPC2_SPIN_DEFAULT);
    if (!req) { c->state = 2; goto out; }
    req->opcode      = VFS_FLIPC_OP_READ;
    req->flags       = 0;
    req->cookie      = ++c->seq;
    req->data_offset = 0;
    req->data_length = want;
    req->param[0]    = 0;
    req->param[1]    = handle;
    req->param[2]    = offset;
    flipc2_produce_commit(c->rev);

    rep = flipc2_consume_wait(c->fwd, FLIPC2_SPIN_DEFAULT);
    if (!rep) { c->state = 2; goto out; }
    if (rep->status == VFS_FLIPC_OK) {
        uint64_t n   = rep->data_length;
        uint64_t off = rep->data_offset;
        if (n > 0)
            memcpy(buf, flipc2_data_ptr(c->fwd, off), (size_t)n);
        ret = (ssize_t)n;
    }
    flipc2_consume_release(c->fwd);

out:
    pthread_mutex_unlock(&c->io_lock);
    return ret;
}

/*
 * One FLIPC write round-trip: copy the payload into the reverse
 * channel's data region, post a WRITE request, await the reply.
 * 'count' must be <= VFS_FLIPC_MAX_READ (the channel data region).
 * Returns bytes written (>= 0) or -1 to fall back to the Mach path.
 */
static ssize_t
vfs_flipc_write(struct vfs_flipc_conn *c, vfs_u64_t handle,
                vfs_u64_t offset, const void *buf, size_t count)
{
    ssize_t ret = -1;
    struct flipc2_desc *req, *rep;

    if (count > VFS_FLIPC_MAX_READ)
        count = VFS_FLIPC_MAX_READ;

    pthread_mutex_lock(&c->io_lock);

    req = flipc2_produce_wait(c->rev, FLIPC2_SPIN_DEFAULT);
    if (!req) { c->state = 2; goto out; }
    memcpy(flipc2_data_ptr(c->rev, 0), buf, count);
    req->opcode      = VFS_FLIPC_OP_WRITE;
    req->flags       = 0;
    req->cookie      = ++c->seq;
    req->data_offset = 0;
    req->data_length = count;
    req->param[0]    = 0;
    req->param[1]    = handle;
    req->param[2]    = offset;
    flipc2_produce_commit(c->rev);

    rep = flipc2_consume_wait(c->fwd, FLIPC2_SPIN_DEFAULT);
    if (!rep) { c->state = 2; goto out; }
    if (rep->status == VFS_FLIPC_OK)
        ret = (ssize_t)rep->data_length;
    flipc2_consume_release(c->fwd);

out:
    pthread_mutex_unlock(&c->io_lock);
    return ret;
}

/*
 * Emit one buffered run to the server: FLIPC write when a channel is
 * up, else (or on FLIPC failure) a single Mach fs_write of the whole
 * run.  Re-writing a FLIPC-written prefix via Mach is idempotent (same
 * bytes, same offset), so the fallback stays simple.
 */
static void
vfs_wb_emit(struct vfs_flipc_conn *c, mach_port_t fs_port, vfs_u64_t handle,
            vm_offset_t wb_buf, vfs_u64_t wb_start, vfs_u32_t wb_len)
{
    ssize_t w = -1;
    if (wb_len == 0)
        return;
    if (c)
        w = vfs_flipc_write(c, handle, wb_start, (void *)wb_buf, wb_len);
    if (w != (ssize_t)wb_len) {
        vfs_u32_t written = 0;
        (void)fs_write(fs_port, handle, wb_start, (pointer_t)wb_buf,
                       (mach_msg_type_number_t)wb_len, &written);
    }
}

/*
 * Flush a fd's pending write-behind buffer to the server.  Claims the
 * buffer under the lock (clears wb_len) so it can't double-flush, then
 * emits outside the lock.  Called before any op that must see a
 * coherent file: read, lseek, fstat, sync, close.
 */
int
vfs_flipc_wb_flush(vfs_fd_t fd)
{
    struct vfs_fd_entry *e;
    mach_port_t fs_port;
    vfs_u64_t handle, wb_start;
    vfs_u32_t wb_len;
    vm_offset_t wb_buf;
    struct vfs_flipc_conn *c;

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (!e || e->wb_len == 0) {
        pthread_mutex_unlock(&vfs_lock);
        return 0;
    }
    fs_port  = e->fs_port;
    handle   = e->handle;
    wb_buf   = e->wb_buf;
    wb_start = e->wb_start;
    wb_len   = e->wb_len;
    e->wb_len = 0;                  /* claim */
    pthread_mutex_unlock(&vfs_lock);

    c = vfs_flipc_get(fs_port);
    vfs_wb_emit(c, fs_port, handle, wb_buf, wb_start, wb_len);
    return 0;
}

/*
 * Write-behind path (#232): coalesce contiguous sequential writes into
 * a per-fd buffer and emit them as few large FLIPC writes.  A single
 * synchronous channel write loses to Mach's RPC (the round-trip wakeup),
 * so the win — like read prefetch — comes from batching.  Returns bytes
 * accepted (== count) or -1 to fall back to the Mach path.
 */
static ssize_t
vfs_writebehind(vfs_fd_t fd, struct vfs_flipc_conn *c, mach_port_t fs_port,
                vfs_u64_t handle, const void *ubuf, size_t count)
{
    struct vfs_fd_entry *e;
    vfs_u64_t offset, wb_start;
    vfs_u32_t wb_len;
    vm_offset_t wb_buf;
    size_t done = 0;
    const char *in = (const char *)ubuf;

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (!e || e->dead) { pthread_mutex_unlock(&vfs_lock); return -1; }
    offset   = e->offset;
    wb_buf   = e->wb_buf;
    wb_start = e->wb_start;
    wb_len   = e->wb_len;
    pthread_mutex_unlock(&vfs_lock);

    if (wb_buf == 0) {
        if (vm_allocate(mach_task_self(), &wb_buf,
                        VFS_PREFETCH_SIZE, TRUE) != KERN_SUCCESS)
            return -1;          /* fall back to Mach */
        wb_len = 0;
    }

    /* A non-contiguous write can't extend the current run — emit it. */
    if (wb_len > 0 && offset != wb_start + wb_len) {
        vfs_wb_emit(c, fs_port, handle, wb_buf, wb_start, wb_len);
        wb_len = 0;
    }
    if (wb_len == 0)
        wb_start = offset;

    while (done < count) {
        size_t space = VFS_PREFETCH_SIZE - wb_len;
        size_t take  = count - done;
        if (take > space)
            take = space;
        memcpy((void *)(wb_buf + wb_len), in + done, take);
        wb_len += (vfs_u32_t)take;
        done   += take;
        offset += take;
        if (wb_len == VFS_PREFETCH_SIZE) {
            vfs_wb_emit(c, fs_port, handle, wb_buf, wb_start, wb_len);
            wb_len   = 0;
            wb_start = offset;
        }
    }

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (e) {
        e->wb_buf   = wb_buf;
        e->wb_start = wb_start;
        e->wb_len   = wb_len;
        e->offset   = offset;
        e->pf_len   = 0;        /* content changed — drop read prefetch */
    }
    pthread_mutex_unlock(&vfs_lock);
    return (ssize_t)done;
}

/*
 * Pipelined read via the per-fd prefetch window (#232).  A single large
 * FLIPC fetch fills pf_buf; the requested bytes — and every subsequent
 * sequential read that lands in the window — are served by a local
 * memcpy with no further IPC.  This is what turns FLIPC's throughput
 * edge into a real win for small/medium reads, where a synchronous
 * per-read channel round-trip would otherwise lose to Mach's RPC.
 *
 * Returns bytes read (>= 0, 0 = EOF) on success, or -1 to fall back to
 * the Mach data path (channel error with nothing yet delivered).
 */
static ssize_t
vfs_prefetch_read(vfs_fd_t fd, struct vfs_flipc_conn *c,
                  vfs_u64_t handle, void *ubuf, size_t count)
{
    struct vfs_fd_entry *e;
    vfs_u64_t offset, pf_start;
    vfs_u32_t pf_len;
    vm_offset_t pf_buf;
    size_t done = 0;
    int err = 0;
    char *out = (char *)ubuf;

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (!e || e->dead) { pthread_mutex_unlock(&vfs_lock); return -1; }
    offset   = e->offset;
    pf_buf   = e->pf_buf;
    pf_start = e->pf_start;
    pf_len   = e->pf_len;
    pthread_mutex_unlock(&vfs_lock);

    if (pf_buf == 0) {
        if (vm_allocate(mach_task_self(), &pf_buf,
                        VFS_PREFETCH_SIZE, TRUE) != KERN_SUCCESS)
            return -1;          /* fall back to Mach */
        pf_len = 0;
    }

    while (done < count) {
        int covered = (pf_len > 0 && offset >= pf_start &&
                       offset < pf_start + pf_len);
        if (!covered) {
            ssize_t n = vfs_flipc_read(c, handle, offset,
                                       (void *)pf_buf, VFS_PREFETCH_SIZE);
            if (n < 0) { err = 1; break; }
            pf_start = offset;
            pf_len   = (vfs_u32_t)n;
            if (n == 0)
                break;          /* EOF */
        }
        {
            vfs_u64_t in_win = (pf_start + pf_len) - offset;
            size_t give = count - done;
            if (give > in_win)
                give = (size_t)in_win;
            memcpy(out + done,
                   (void *)(pf_buf + (size_t)(offset - pf_start)), give);
            done   += give;
            offset += give;
        }
    }

    pthread_mutex_lock(&vfs_lock);
    e = vfs_fd_get(fd);
    if (e) {
        e->pf_buf   = pf_buf;
        e->pf_start = pf_start;
        e->pf_len   = pf_len;
        if (done > 0)
            e->offset = offset;
    }
    pthread_mutex_unlock(&vfs_lock);

    if (done == 0 && err)
        return -1;              /* nothing delivered — try Mach */
    return (ssize_t)done;
}

/* ------------------------------------------------------------------ */
/*  Seam entry points (declared in libvfs_internal.h)                   */
/* ------------------------------------------------------------------ */

ssize_t
vfs_flipc_read_fastpath(vfs_fd_t fd, mach_port_t fs_port,
                        vfs_u64_t handle, void *buf, size_t count)
{
    struct vfs_flipc_conn *fc;

    if (!vfs_flipc_enabled)
        return -1;
    fc = vfs_flipc_get(fs_port);
    if (!fc)
        return -1;
    return vfs_prefetch_read(fd, fc, handle, buf, count);
}

ssize_t
vfs_flipc_write_fastpath(vfs_fd_t fd, mach_port_t fs_port,
                         vfs_u64_t handle, const void *buf, size_t count)
{
    struct vfs_flipc_conn *fc;

    if (!vfs_flipc_enabled || count == 0)
        return -1;
    fc = vfs_flipc_get(fs_port);
    if (!fc)
        return -1;
    return vfs_writebehind(fd, fc, fs_port, handle, buf, count);
}
