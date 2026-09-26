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

#ifndef _LIBVFS_H_
#define _LIBVFS_H_

/*
 * libvfs.h — public client API for the libvfs dispatcher (#220 v0.1).
 *
 * Include from any task that needs filesystem I/O routed through the
 * VFS mount table.  fs_servers MUST NOT include this header: their
 * impl symbols (vfs_open, vfs_read, ...) collide with the client
 * wrappers declared here.
 */

#include "vfs_types.h"          /* wire types: vfs_stat_t, VFS_O_*, ... */
#include <mach.h>               /* mach_port_t for vfs_mmap (#276) */

/*
 * vfs_fd_t — libvfs file descriptor.  An int index into the per-task
 * fd table; NOT a POSIX fd (libposix-uros owns the POSIX numbering and
 * its own translation, in a future layer).  -1 on error.
 */
typedef int             vfs_fd_t;

#define VFS_FD_INVALID  ((vfs_fd_t)-1)

/* lseek whence */
#define VFS_SEEK_SET    0
#define VFS_SEEK_CUR    1
#define VFS_SEEK_END    2

/*
 * size_t, ssize_t and off_t come from <sa_mach/types.h> or from musl,
 * whichever the including target has -- they agree now (#480).  This
 * header used to declare ssize_t and off_t itself, behind a guard
 * (_OFF_T_DEFINED) that neither of the other two had heard of, so it
 * protected against nothing; the widths it picked then had to be worked
 * around by keeping every wire-side offset in vfs_u64_t.  The wire types
 * stay as they are: they are 64-bit by design and not by workaround.
 */
#include <sa_mach/types.h>

/*
 * Initialise libvfs once per task.  Idempotent and cheap; safe to
 * call from library constructors.  Today: zeroes the fd table; the
 * mount cache is populated lazily on the first vfs_open().
 */
int             vfs_init(void);

/*
 * vfs_open — resolve path to fs_server, ask it to open, allocate an fd.
 *   flags — VFS_O_* (vfs_types.h)
 *   mode  — POSIX mode bits, used only when VFS_O_CREAT is set
 * Returns a valid vfs_fd_t on success, VFS_FD_INVALID on failure.
 */
vfs_fd_t        vfs_open(const char *path, int flags, int mode);

/*
 * #599: vfs_open, answering why when it fails.  Returns KERN_SUCCESS and the
 * fd in *fd_out, or the reason with *fd_out VFS_FD_INVALID: a VFS_ERR_* code
 * from the filesystem (VFS_ERR_NOENT for a name that is not there, VFS_ERR_IO
 * for a damaged directory or a device that failed), a KERN_* code, or a Mach
 * send error when the server is gone.  vfs_open is this without the reason.
 */
kern_return_t   vfs_open_rc(const char *path, int flags, int mode,
                            vfs_fd_t *fd_out);

int             vfs_close(vfs_fd_t fd);

ssize_t         vfs_read(vfs_fd_t fd, void *buf, size_t count);
ssize_t         vfs_write(vfs_fd_t fd, const void *buf, size_t count);

off_t           vfs_lseek(vfs_fd_t fd, off_t offset, int whence);

/*
 * vfs_stat, vfs_fstat, vfs_unlink and vfs_rename return 0 on success and
 * otherwise the reason, as vfs_open_rc does -- never a bare -1 (#599), so
 * that a name that is absent and a directory that is damaged are two answers.
 */
int             vfs_stat(const char *path, vfs_stat_t *out);
int             vfs_fstat(vfs_fd_t fd, vfs_stat_t *out);

int             vfs_sync(vfs_fd_t fd);

/*
 * vfs_mmap - ask the fs_server for a Mach memory_object backing the
 * file open under `fd`.  The returned send right is what libposix-uros
 * passes to vm_map (h_mmap2 for the POSIX mmap path, #276 Phase B).
 *
 *   prot/flags - POSIX PROT_xxx and MAP_xxx bits, forwarded as hints;
 *                actual protection is enforced per-mapping by the
 *                kernel at vm_map time.
 *   out_port   - set to the memory_object port on success.
 * Returns 0 on success, -1 on failure (out_port left untouched).
 */
int             vfs_mmap(vfs_fd_t fd, int prot, int flags,
                         mach_port_t *out_port);

/*
 * Namespace operations (v0.3.0, #231).  Paths are absolute.
 *   vfs_unlink — remove a name.
 *   vfs_rename — move a name; same-mount uses the server's atomic
 *                rename, cross-mount is synthesized as copy + unlink
 *                (POSIX EXDEV semantics).
 *   vfs_copy   — byte copy src -> dst, creating/truncating dst; works
 *                within or across mounts.  Returns 0 / -1.
 */
int             vfs_unlink(const char *path);
int             vfs_rename(const char *oldpath, const char *newpath);
int             vfs_copy(const char *src, const char *dst);

/*
 * FLIPC v2 fast-path control (#232).  Bulk vfs_read above an internal
 * size threshold routes its data through a shared-memory channel when
 * the fs_server offers one; disabling forces the Mach data path (used
 * by benchmarks to A/B the two).  Enabled by default.
 */
void            vfs_flipc_set_enabled(int on);

/*
 * Flush every fd's pending write-behind buffer to its fs_server (#232).
 * libposix-uros calls this before execve hands the fd table to a new
 * image, since the buffers live in this task's heap and would otherwise
 * be lost across exec.
 */
void            vfs_flush_all(void);

#endif /* _LIBVFS_H_ */
