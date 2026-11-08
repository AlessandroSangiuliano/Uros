/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The server side of vfs.defs under other names (#716).
 *
 * vfs.defs gives its server functions the prefix vfs_, and libvfs's API is
 * spelled the same: a task that both reads through libvfs and serves a mount
 * would define vfs_open, vfs_read and the rest twice.  mount_test is that
 * task, so the server stubs MIG generates for it are compiled with this file
 * included first (CMakeLists.txt), and so is mount_liar.c, which implements
 * them.  Sixteen names, one per routine in vfs.defs: a routine added there and
 * not here is a link error against libvfs, or a call into it.
 */

#ifndef MOUNT_TEST_LIAR_NAMES_H
#define MOUNT_TEST_LIAR_NAMES_H

#define vfs_open		liar_open
#define vfs_close		liar_close
#define vfs_read		liar_read
#define vfs_write		liar_write
#define vfs_truncate		liar_truncate
#define vfs_stat		liar_stat
#define vfs_fstat		liar_fstat
#define vfs_readdir		liar_readdir
#define vfs_unlink		liar_unlink
#define vfs_mkdir		liar_mkdir
#define vfs_rmdir		liar_rmdir
#define vfs_rename		liar_rename
#define vfs_sync		liar_sync
#define vfs_flipc_endpoint	liar_flipc_endpoint
#define vfs_mmap		liar_mmap
#define vfs_unmount		liar_unmount

#endif /* MOUNT_TEST_LIAR_NAMES_H */
