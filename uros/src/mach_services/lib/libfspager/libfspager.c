/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * libfspager.c — Phase A (#276): library scaffolding.
 *
 * The 10 seqnos_memory_object_* handlers below are the targets of the
 * MIG dispatch generated from <mach/memory_object.defs> (with
 * -DSEQNOS).  Phase A's job is to compile, link and dispatch — every
 * handler is a no-op returning KERN_SUCCESS so the link resolves and
 * the message machinery completes without error.  Phase B will fill
 * in init/terminate + port allocation; Phase C the data path; Phase D
 * write-back / msync (see [[project_libfspager]] for the full plan).
 *
 * fs_pager_create returns MACH_PORT_NULL in Phase A and logs once so
 * the (still hypothetical) caller fails loudly: nothing in tree links
 * us yet, but better to be honest than silent.
 *
 * Reference implementation for the eventual non-stub bodies:
 *   uros/src/default_pager/dp_memory_object.c
 * (study, do not copy — [[feedback_study_never_copy]]).
 */

#include "libfspager.h"

#include <mach.h>
#include <mach/memory_object.h>
#include <mach/vm_sync.h>
#include <stdio.h>

/* MIG-generated server prototype.  Defined in memory_object_server.c
 * (with -DSEQNOS); declared here so we can call it from fs_pager_demux
 * without dragging the MIG sheader into our public interface. */
extern boolean_t seqnos_memory_object_server(mach_msg_header_t *in,
                                             mach_msg_header_t *out);

/* ------------------------------------------------------------------ */
/*  Library-level state                                                */
/* ------------------------------------------------------------------ */

static const char *fspager_tag = "fspager";

void
fs_pager_init(const char *tag)
{
    if (tag != NULL)
        fspager_tag = tag;
}

mach_port_t
fs_pager_create(uint64_t file_id,
                const struct fs_pager_ops *ops,
                void *state)
{
    (void)file_id; (void)ops; (void)state;
    /* Phase A: no port allocation, no table — Phase B lands those.
     * Print once so a (future) early caller knows why mmap fails. */
    static int warned = 0;
    if (!warned) {
        warned = 1;
        printf("%s: fs_pager_create: Phase A stub (returns NULL)\n",
               fspager_tag);
    }
    return MACH_PORT_NULL;
}

boolean_t
fs_pager_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
    return seqnos_memory_object_server(in, out);
}

/* ================================================================== */
/*  MIG handler stubs (seqnos_memory_object_*)                          */
/*                                                                      */
/*  All return KERN_SUCCESS so the dispatch completes; data-bearing     */
/*  handlers (data_request, data_unlock, data_return) ignore their      */
/*  inputs in Phase A.  Real bodies arrive in B/C/D.                    */
/* ================================================================== */

kern_return_t
seqnos_memory_object_init(
    mach_port_t        mem_obj,
    mach_port_seqno_t  seqno,
    mach_port_t        control_port,
    vm_size_t          pager_page_size)
{
    (void)mem_obj; (void)seqno; (void)control_port; (void)pager_page_size;
    return KERN_SUCCESS;
}

kern_return_t
seqnos_memory_object_terminate(
    mach_port_t        mem_obj,
    mach_port_seqno_t  seqno,
    mach_port_t        control_port)
{
    (void)mem_obj; (void)seqno; (void)control_port;
    return KERN_SUCCESS;
}

kern_return_t
seqnos_memory_object_data_request(
    mach_port_t        mem_obj,
    mach_port_seqno_t  seqno,
    mach_port_t        control_port,
    vm_offset_t        offset,
    vm_size_t          length,
    vm_prot_t          desired_access)
{
    (void)mem_obj; (void)seqno; (void)control_port;
    (void)offset;  (void)length; (void)desired_access;
    /* Phase C will read from the fs callback and reply with
     * memory_object_data_supply. */
    return KERN_SUCCESS;
}

kern_return_t
seqnos_memory_object_data_unlock(
    mach_port_t        mem_obj,
    mach_port_seqno_t  seqno,
    mach_port_t        control_port,
    vm_offset_t        offset,
    vm_size_t          length,
    vm_prot_t          desired_access)
{
    (void)mem_obj; (void)seqno; (void)control_port;
    (void)offset;  (void)length; (void)desired_access;
    return KERN_SUCCESS;
}

kern_return_t
seqnos_memory_object_lock_completed(
    mach_port_t        mem_obj,
    mach_port_seqno_t  seqno,
    mach_port_t        control_port,
    vm_offset_t        offset,
    vm_size_t          length)
{
    (void)mem_obj; (void)seqno; (void)control_port;
    (void)offset;  (void)length;
    return KERN_SUCCESS;
}

kern_return_t
seqnos_memory_object_supply_completed(
    mach_port_t        mem_obj,
    mach_port_seqno_t  seqno,
    mach_port_t        control_port,
    vm_offset_t        offset,
    vm_size_t          length,
    kern_return_t      result,
    vm_offset_t        error_offset)
{
    (void)mem_obj; (void)seqno; (void)control_port;
    (void)offset;  (void)length; (void)result; (void)error_offset;
    return KERN_SUCCESS;
}

kern_return_t
seqnos_memory_object_data_return(
    mach_port_t        mem_obj,
    mach_port_seqno_t  seqno,
    mach_port_t        control_port,
    vm_offset_t        offset,
    pointer_t          data,
    mach_msg_type_number_t data_count,
    boolean_t          dirty,
    boolean_t          kernel_copy)
{
    (void)mem_obj; (void)seqno; (void)control_port;
    (void)offset;  (void)data;  (void)data_count;
    (void)dirty;   (void)kernel_copy;
    /* Phase D will dispatch this to write_page after vm_deallocate'ing
     * the OOL buffer. */
    return KERN_SUCCESS;
}

kern_return_t
seqnos_memory_object_synchronize(
    mach_port_t        mem_obj,
    mach_port_seqno_t  seqno,
    mach_port_t        control_port,
    vm_offset_t        offset,
    vm_size_t          length,
    vm_sync_t          flags)
{
    (void)mem_obj; (void)seqno; (void)control_port;
    (void)offset;  (void)length; (void)flags;
    return KERN_SUCCESS;
}

kern_return_t
seqnos_memory_object_change_completed(
    mach_port_t        mem_obj,
    mach_port_seqno_t  seqno,
    mach_port_t        control_port,
    memory_object_flavor_t flavor)
{
    (void)mem_obj; (void)seqno; (void)control_port; (void)flavor;
    return KERN_SUCCESS;
}

kern_return_t
seqnos_memory_object_discard_request(
    mach_port_t        mem_obj,
    mach_port_seqno_t  seqno,
    mach_port_t        control_port,
    vm_offset_t        offset,
    vm_size_t          length)
{
    (void)mem_obj; (void)seqno; (void)control_port;
    (void)offset;  (void)length;
    return KERN_SUCCESS;
}
