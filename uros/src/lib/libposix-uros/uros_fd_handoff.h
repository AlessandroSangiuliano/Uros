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

/*
 * #904: a pipe end's record.  type is UROS_FD_HANDOFF_FIFO (libvfs's
 * VFS_FT_FIFO, which posix_exec_fds.c checks), fs_port the pipe's port at
 * pipe_server, handle the end's token, both names inserted into the new task
 * as they are, flags the end and O_NONBLOCK, offset 0.  The new image maps
 * the pipe from pipe_server's memory entry and keeps the token, which is the
 * same process's after the exec: its pid does not change.  Pipe records come
 * from fd 0 up, since a pipe on stdin or stdout is what `a | b' is; the
 * files' records still start at 3.
 */
#define UROS_FD_HANDOFF_FIFO            6      /* VFS_FT_FIFO */
#define UROS_FD_HANDOFF_PIPE_WRITE      0x1    /* flags: the write end */
#define UROS_FD_HANDOFF_PIPE_NONBLOCK   0x2    /* flags: O_NONBLOCK */

struct uros_fd_handoff_rec {
    int32_t   posix_fd;     /* POSIX fd number to recreate */
    uint32_t  fs_port;      /* Mach port NAME (inserted into the new task) */
    uint64_t  handle;       /* fs_server open-file cookie */
    uint64_t  offset;       /* current file offset */
    int32_t   flags;        /* VFS_O_* */
    uint8_t   type;         /* VFS_FT_* */
    /*
     * #897: 0, or 1 + the index of an earlier record this descriptor was a
     * dup() of.  The new image then makes it a second name of that record's
     * description, one offset for both, instead of opening it again.  The
     * other fields repeat the earlier record's, so an image built before
     * this field reads it as padding and opens the file twice, as all of
     * them did.  It fits a byte: UROS_FD_HANDOFF_MAX is below 255.
     */
    uint8_t   same_as;
    uint8_t   _pad[2];
};

_Static_assert(sizeof(struct uros_fd_handoff_rec) == 32,
               "a handoff record changed size: images of two builds would "
               "read each other's records at different strides");

struct uros_fd_handoff {
    uint32_t  magic;        /* UROS_FD_HANDOFF_MAGIC */
    uint32_t  count;        /* number of valid recs */
    struct uros_fd_handoff_rec recs[UROS_FD_HANDOFF_MAX];
};

#endif /* UROS_FD_HANDOFF_H */
