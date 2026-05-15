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
 * size_t comes via <stddef.h> already pulled in by vfs_types.h.
 * ssize_t and off_t are not consistently defined across sa_mach
 * (off_t is u32 in sa_mach/types.h, ssize_t is absent), so we provide
 * local typedefs guarded against the most common system definitions.
 * Wire-side offsets always use vfs_u64_t regardless of off_t width.
 */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long            ssize_t;
#endif
#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED
typedef unsigned int    off_t;          /* matches sa_mach/types.h */
#endif

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

int             vfs_close(vfs_fd_t fd);

ssize_t         vfs_read(vfs_fd_t fd, void *buf, size_t count);
ssize_t         vfs_write(vfs_fd_t fd, const void *buf, size_t count);

off_t           vfs_lseek(vfs_fd_t fd, off_t offset, int whence);

int             vfs_stat(const char *path, vfs_stat_t *out);
int             vfs_fstat(vfs_fd_t fd, vfs_stat_t *out);

int             vfs_sync(vfs_fd_t fd);

#endif /* _LIBVFS_H_ */
