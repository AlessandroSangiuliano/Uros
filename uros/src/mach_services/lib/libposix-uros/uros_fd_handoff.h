/*
 * libposix-uros — execve fd-inheritance handoff format (#262 step 3).
 *
 * When a task execve()s, the new image is a fresh task (fresh VM, fresh
 * IPC space) created by exec_server, so the open fd set cannot ride
 * along via CoW the way it does across fork().  Instead the execing
 * parent serialises its surviving (non-O_CLOEXEC) fds into this blob,
 * vm_write's it to EXEC_FD_HANDOFF_VA in the suspended new task, inserts
 * the matching fs_server send rights into the new task's IPC space, and
 * resumes.  The new task's startup (__uros_absorb_inherited_fds) reads
 * the blob and rebuilds its libvfs + POSIX fd tables before main runs.
 *
 * Both ends link the same libposix-uros, so the struct layout matches
 * even when the execve target is a different binary.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#ifndef UROS_FD_HANDOFF_H
#define UROS_FD_HANDOFF_H

#include <stdint.h>

#define UROS_FD_HANDOFF_MAGIC   0x46444831u    /* "FDH1" */
#define UROS_FD_HANDOFF_MAX     61             /* fits a 4 KiB page */

struct uros_fd_handoff_rec {
    int32_t   posix_fd;     /* POSIX fd number to recreate */
    uint32_t  fs_port;      /* Mach port NAME (inserted into the new task) */
    uint64_t  handle;       /* fs_server open-file cookie */
    uint64_t  offset;       /* current file offset */
    int32_t   flags;        /* VFS_O_* */
    uint8_t   type;         /* VFS_FT_* */
    uint8_t   _pad[3];
};

struct uros_fd_handoff {
    uint32_t  magic;        /* UROS_FD_HANDOFF_MAGIC */
    uint32_t  count;        /* number of valid recs */
    struct uros_fd_handoff_rec recs[UROS_FD_HANDOFF_MAX];
};

#endif /* UROS_FD_HANDOFF_H */
