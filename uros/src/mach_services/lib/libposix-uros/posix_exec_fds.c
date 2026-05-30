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

/* POSIX-fd table accessors (posix_fd.c). */
extern int  __uros_pfd_enum_inheritable(int *posix_fds, vfs_fd_t *vfds, int max);
extern void __uros_pfd_install(int posix_fd, vfs_fd_t vfd);

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
    int n, i, out = 0;

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
        out++;
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
        vfs_fd_t vfd = vfs_import_fd((mach_port_t)r->fs_port, r->handle,
                                     r->offset, r->flags, r->type);
        if (vfd != VFS_FD_INVALID)
            __uros_pfd_install(r->posix_fd, vfd);
    }

    (void)vm_deallocate(mach_task_self(), (vm_address_t)data, cnt);
}
