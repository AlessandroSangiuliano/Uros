/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * vfs_flipc_stub.c — no-op FLIPC seam for the umbrella libc.so (#234).
 *
 * The umbrella build links this instead of vfs_flipc.c, so libc.so (the
 * dynamic linker + the libc every dynamic app shares) carries NO FLIPC
 * code: reads/writes go straight to Mach RPC, and no untrusted dynamic
 * app gets a shared-memory channel into an fs_server.  Same seam as
 * vfs_flipc.c (libvfs_internal.h); every entry point reports "nothing
 * done" so libvfs.c stays on the Mach path.  See memory uros-vision /
 * project_234_dynlink.
 */

#include "libvfs.h"
#include "libvfs_internal.h"

ssize_t
vfs_flipc_read_fastpath(vfs_fd_t fd, mach_port_t fs_port,
                        vfs_u64_t handle, void *buf, size_t count)
{
    (void)fd; (void)fs_port; (void)handle; (void)buf; (void)count;
    return -1;                  /* always fall back to Mach */
}

ssize_t
vfs_flipc_write_fastpath(vfs_fd_t fd, mach_port_t fs_port,
                         vfs_u64_t handle, const void *buf, size_t count)
{
    (void)fd; (void)fs_port; (void)handle; (void)buf; (void)count;
    return -1;                  /* always fall back to Mach */
}

int
vfs_flipc_wb_flush(vfs_fd_t fd)
{
    (void)fd;
    return 0;                   /* nothing buffered without the fast-path */
}
