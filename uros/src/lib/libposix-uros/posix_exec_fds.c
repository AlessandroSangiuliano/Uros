/*
 * libposix-uros — execve fd-inheritance: serialise (parent) + absorb
 * (new image).  See uros_fd_handoff.h for the wire format and the
 * end-to-end rationale.  This TU owns the Mach + libvfs glue; posix_fd.c
 * stays freestanding and only exposes the POSIX-fd table accessors.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_interface.h>

#include "libvfs.h"                 /* vfs_export_fd / vfs_import_fd / vfs_fd_t */
#include "uros_fd_handoff.h"
#include "exec_types.h"             /* EXEC_FD_HANDOFF_VA */
#include "posix_pipe.h"             /* pipes across execve (#904) */
#include "pipe_types.h"             /* PIPE_END_READ, PIPE_END_WRITE */

_Static_assert(UROS_FD_HANDOFF_FIFO == VFS_FT_FIFO,
               "a pipe's handoff record must carry libvfs's FIFO type");

/* POSIX-fd table accessors (posix_fd.c). */
extern int  __uros_pfd_enum_inheritable(int *posix_fds, vfs_fd_t *vfds, int max);
extern void __uros_pfd_install(int posix_fd, vfs_fd_t vfd);
extern int  __uros_pfd_install_alias(int posix_fd, int of_fd);
extern int  __uros_pfd_enum_pipes(int *posix_fds, struct uros_pipe **pipes,
                                  int *ends, int *nonblock, int *desc, int max);
extern int  __uros_pfd_install_pipe(int posix_fd, struct uros_pipe *p, int end,
                                    int nonblock);

/* libvfs handoff helpers (header-less: libvfs.h omits them by design). */
extern int      vfs_export_fd(vfs_fd_t fd, mach_port_t *port, uint64_t *handle,
                              uint64_t *offset, int *flags, uint8_t *type);
extern vfs_fd_t vfs_import_fd(mach_port_t port, uint64_t handle, uint64_t offset,
                              int flags, uint8_t type);

/*
 * Parent side: fill 'blob' with every surviving (non-CLOEXEC) open fd.
 * Returns the record count.  The caller (posix_exec.c) then inserts each
 * rec.fs_port send right into the new task and vm_write's the blob.
 */
int
__uros_serialize_fds(struct uros_fd_handoff *blob)
{
    int posix_fds[UROS_FD_HANDOFF_MAX];
    vfs_fd_t vfds[UROS_FD_HANDOFF_MAX];
    vfs_fd_t rec_vfd[UROS_FD_HANDOFF_MAX];  /* the libvfs fd behind recs[k] */
    int n, i, j, out = 0;

    /* #232: push any buffered writes to the fs_servers before execve —
     * the write-behind buffers live in this (about-to-be-replaced) task's
     * heap and would otherwise be lost. */
    vfs_flush_all();

    memset(blob, 0, sizeof(*blob));
    blob->magic = UROS_FD_HANDOFF_MAGIC;

    n = __uros_pfd_enum_inheritable(posix_fds, vfds, UROS_FD_HANDOFF_MAX);
    for (i = 0; i < n; i++) {
        mach_port_t port = MACH_PORT_NULL;
        uint64_t    handle = 0, offset = 0;
        int         flags = 0;
        uint8_t     type = 0;
        int         first = -1;

        /*
         * #897: two descriptors with one libvfs fd are one description, a
         * dup() of each other.  The second is the first's record again,
         * marked as a second name of it, so that the new image gives both
         * one offset; exported twice, they came out two opens of the file,
         * each writing over what the other wrote.
         */
        for (j = 0; j < out; j++)
            if (rec_vfd[j] == vfds[i] && blob->recs[j].same_as == 0) {
                first = j;
                break;
            }
        if (first >= 0) {
            blob->recs[out] = blob->recs[first];
            blob->recs[out].posix_fd = posix_fds[i];
            blob->recs[out].same_as  = (uint8_t)(first + 1);
            rec_vfd[out] = vfds[i];
            out++;
            continue;
        }
        if (vfs_export_fd(vfds[i], &port, &handle, &offset, &flags, &type) != 0)
            continue;
        if (port == MACH_PORT_NULL)
            continue;
        blob->recs[out].posix_fd = posix_fds[i];
        blob->recs[out].fs_port  = (uint32_t)port;
        blob->recs[out].handle   = handle;
        blob->recs[out].offset   = offset;
        blob->recs[out].flags    = flags;
        blob->recs[out].type     = type;
        blob->recs[out].same_as  = 0;
        rec_vfd[out] = vfds[i];
        out++;
    }

    /*
     * #904: the pipe ends, after the files, from fd 0 up.  Two descriptors
     * of one description are its record and a second name of it, as for a
     * file; same_as is the earlier record's index in the whole blob.
     */
    {
        int               pfd[UROS_FD_HANDOFF_MAX], ends[UROS_FD_HANDOFF_MAX];
        int               nb[UROS_FD_HANDOFF_MAX], desc[UROS_FD_HANDOFF_MAX];
        int               rec_of[UROS_FD_HANDOFF_MAX];
        struct uros_pipe *pp[UROS_FD_HANDOFF_MAX];
        int               k = __uros_pfd_enum_pipes(pfd, pp, ends, nb, desc,
                                                    UROS_FD_HANDOFF_MAX - out);

        for (i = 0; i < k && out < UROS_FD_HANDOFF_MAX; i++) {
            struct uros_fd_handoff_rec *r = &blob->recs[out];
            int first = -1;

            for (j = 0; j < i; j++)
                if (desc[j] == desc[i]) {
                    first = rec_of[j];
                    break;
                }
            rec_of[i] = out;
            if (first >= 0) {
                *r = blob->recs[first];
                r->posix_fd = pfd[i];
                r->same_as  = (uint8_t)(first + 1);
                out++;
                continue;
            }
            r->posix_fd = pfd[i];
            r->fs_port  = pp[i]->port;
            r->handle   = pp[i]->token[ends[i]];
            r->offset   = 0;
            r->flags    = (ends[i] == PIPE_END_WRITE ?
                           UROS_FD_HANDOFF_PIPE_WRITE : 0) |
                          (nb[i] ? UROS_FD_HANDOFF_PIPE_NONBLOCK : 0);
            r->type     = UROS_FD_HANDOFF_FIFO;
            r->same_as  = 0;
            out++;
        }
    }
    blob->count = (uint32_t)out;
    return out;
}

/*
 * New-image side: rebuild the fd tables from the handoff page, if one
 * was planted.  Probe with vm_read (no fault if the VA is unmapped, as
 * for every bootstrap-launched task) and bail unless the magic matches.
 * Called from musl's __uros_libc_init before main runs.
 */
void
__uros_absorb_inherited_fds(void)
{
    pointer_t data = 0;
    mach_msg_type_number_t cnt = 0;
    kern_return_t kr;
    const struct uros_fd_handoff *h;
    uint32_t i;

    kr = vm_read(mach_task_self(), (vm_address_t)EXEC_FD_HANDOFF_VA,
                 sizeof(struct uros_fd_handoff), &data, &cnt);
    if (kr != KERN_SUCCESS)
        return;                     /* no handoff page — normal startup */

    if (cnt < sizeof(struct uros_fd_handoff)) {
        (void)vm_deallocate(mach_task_self(), (vm_address_t)data, cnt);
        return;
    }
    h = (const struct uros_fd_handoff *)data;
    if (h->magic != UROS_FD_HANDOFF_MAGIC) {
        (void)vm_deallocate(mach_task_self(), (vm_address_t)data, cnt);
        return;
    }

    for (i = 0; i < h->count && i < UROS_FD_HANDOFF_MAX; i++) {
        const struct uros_fd_handoff_rec *r = &h->recs[i];

        /* #897: a second name of an earlier record's description.  Only an
         * earlier one: same_as is 1 + its index, so it is below i + 1. */
        if (r->same_as != 0 && r->same_as <= i) {
            (void)__uros_pfd_install_alias(r->posix_fd,
                                           h->recs[r->same_as - 1].posix_fd);
            continue;
        }
        if (r->same_as != 0)
            continue;               /* names a record at or after itself */
        /* #904: a pipe end, mapped from pipe_server, its token kept. */
        if (r->type == UROS_FD_HANDOFF_FIFO) {
            int end = (r->flags & UROS_FD_HANDOFF_PIPE_WRITE) ?
                      PIPE_END_WRITE : PIPE_END_READ;
            struct uros_pipe *p = __uros_pipe_adopt(r->fs_port,
                                                    (unsigned int)r->handle,
                                                    end);
            if (p != NULL &&
                __uros_pfd_install_pipe(r->posix_fd, p, end,
                    (r->flags & UROS_FD_HANDOFF_PIPE_NONBLOCK) != 0) != 0 &&
                p->refs == 0)
                __uros_pipe_free(p);    /* no description took it */
            continue;
        }
        vfs_fd_t vfd = vfs_import_fd((mach_port_t)r->fs_port, r->handle,
                                     r->offset, r->flags, r->type);
        if (vfd != VFS_FD_INVALID)
            __uros_pfd_install(r->posix_fd, vfd);
    }

    (void)vm_deallocate(mach_task_self(), (vm_address_t)data, cnt);
}
