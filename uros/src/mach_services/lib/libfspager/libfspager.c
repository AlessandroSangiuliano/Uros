/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * libfspager.c — Phase B.1 (#276): port allocation + memory_object
 * lifecycle (init/terminate) over an in-memory port table.
 *
 * State of play:
 *   - fs_pager_init records the caller's port-set so we can join new
 *     memory_object ports to it.
 *   - fs_pager_create allocates a fresh receive right, inserts a
 *     send right into the table indexed by port name, makes the port
 *     a member of the saved set, and returns the send-right name to
 *     the caller (the fs server's vfs_mmap handler hands this back
 *     across MIG to libvfs → h_mmap2 → vm_map).
 *   - seqnos_memory_object_init / _terminate walk the table to mark
 *     lifecycle so a later data_request can be matched to a known
 *     entry.  Stub data_request / data_unlock / data_return remain;
 *     Phase C lights them up.
 *
 * Table sizing: VFS_MAX_FDS is 64; if every open fd in every task is
 * mmap'd at once we hit 64 × tasks distinct mappings.  Phase B keeps
 * the array small (FSPAGER_MAX_ENTRIES = 64) and ASSUMES low concurrent
 * mmap counts; the limit ratchets up trivially when the umbrella user-
 * app set grows.
 *
 * Reference for the eventual non-stub bodies:
 *   uros/src/default_pager/dp_memory_object.c  (study, don't copy —
 *   [[feedback_study_never_copy]]).
 */

#include "libfspager.h"

#include <mach.h>
#include "mach_port.h"               /* MIG-generated stubs:
                                        mach_port_allocate, _insert_right,
                                        _move_member, _destroy */
#include <mach/memory_object.h>
#include <mach/vm_sync.h>
#include <mach/mach_traps.h>
#include <stdio.h>
#include <string.h>

/* Tiny atomic spinlock so libfspager does not depend on the fs-server
 * threading model (libpthreads / cthreads / single-threaded all work).
 * Table ops are O(N) over 64 slots — contention is irrelevant. */
typedef struct { int v; } fspager_lock_t;
#define FSPAGER_LOCK_INIT  { 0 }

static inline void fspager_lock(fspager_lock_t *l)
{
    while (__atomic_exchange_n(&l->v, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause" ::: "memory");
}
static inline void fspager_unlock(fspager_lock_t *l)
{
    __atomic_store_n(&l->v, 0, __ATOMIC_RELEASE);
}

/* MIG-generated server prototype.  Defined in
 * generated/libfspager_memory_object_server.c (with -DSEQNOS); declared
 * here so we can call it from fs_pager_demux without dragging the MIG
 * sheader into the public interface. */
extern boolean_t seqnos_memory_object_server(mach_msg_header_t *in,
                                             mach_msg_header_t *out);

/* ------------------------------------------------------------------ */
/*  Port table                                                          */
/* ------------------------------------------------------------------ */

#define FSPAGER_MAX_ENTRIES 64

/* Per-file pager record.  Indexed by mem_obj_port (the receive right
 * libfspager allocated and gave back as send right to the client). */
struct fspager_entry {
    int                       in_use;
    mach_port_t               mem_obj_port;   /* receive right we hold */
    mach_port_t               control_port;   /* MAKE_SEND from kernel
                                                 via memory_object_init,
                                                 used to call back into
                                                 the kernel (data_supply,
                                                 lock_request, …) */
    uint64_t                  file_id;
    const struct fs_pager_ops *ops;
    void                     *state;
    int                       initialised;    /* memory_object_init seen */
};

static struct fspager_entry  g_pager_table[FSPAGER_MAX_ENTRIES];
static fspager_lock_t        g_pager_lock = FSPAGER_LOCK_INIT;
static const char           *g_pager_tag  = "fspager";
static mach_port_t           g_port_set   = MACH_PORT_NULL;

static struct fspager_entry *
fspager_lookup_locked(mach_port_t port)
{
    int i;
    for (i = 0; i < FSPAGER_MAX_ENTRIES; i++) {
        if (g_pager_table[i].in_use &&
            g_pager_table[i].mem_obj_port == port)
            return &g_pager_table[i];
    }
    return NULL;
}

static struct fspager_entry *
fspager_alloc_locked(void)
{
    int i;
    for (i = 0; i < FSPAGER_MAX_ENTRIES; i++) {
        if (!g_pager_table[i].in_use) {
            memset(&g_pager_table[i], 0, sizeof(g_pager_table[i]));
            g_pager_table[i].in_use = 1;
            return &g_pager_table[i];
        }
    }
    return NULL;
}

static void
fspager_free_locked(struct fspager_entry *e)
{
    memset(e, 0, sizeof(*e));   /* in_use cleared */
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

int
fs_pager_init(const char *tag, mach_port_t port_set)
{
    if (port_set == MACH_PORT_NULL) {
        printf("fspager: fs_pager_init: port_set is MACH_PORT_NULL\n");
        return -1;
    }
    fspager_lock(&g_pager_lock);
    if (tag != NULL)
        g_pager_tag = tag;
    g_port_set = port_set;
    fspager_unlock(&g_pager_lock);
    return 0;
}

mach_port_t
fs_pager_create(uint64_t file_id,
                const struct fs_pager_ops *ops,
                void *state)
{
    mach_port_t       mem_obj = MACH_PORT_NULL;
    kern_return_t     kr;
    struct fspager_entry *e;

    if (ops == NULL)
        return MACH_PORT_NULL;

    fspager_lock(&g_pager_lock);
    if (g_port_set == MACH_PORT_NULL) {
        fspager_unlock(&g_pager_lock);
        printf("%s: fs_pager_create: fs_pager_init not called yet\n",
               g_pager_tag);
        return MACH_PORT_NULL;
    }
    e = fspager_alloc_locked();
    fspager_unlock(&g_pager_lock);
    if (e == NULL) {
        printf("%s: fs_pager_create: table full (max %d)\n",
               g_pager_tag, FSPAGER_MAX_ENTRIES);
        return MACH_PORT_NULL;
    }

    /* Allocate a receive right — this is the memory_object port.
     * The send-right name the caller gets back IS this same name; in
     * Mach the receive-right holder always also has an implicit send
     * right to the same port via mach_port_insert_right + MAKE_SEND. */
    kr = mach_port_allocate(mach_task_self(),
                            MACH_PORT_RIGHT_RECEIVE, &mem_obj);
    if (kr != KERN_SUCCESS) {
        printf("%s: fs_pager_create: mach_port_allocate kr=%d\n",
               g_pager_tag, kr);
        goto fail_free;
    }

    kr = mach_port_insert_right(mach_task_self(), mem_obj, mem_obj,
                                MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        printf("%s: fs_pager_create: insert_right kr=%d\n",
               g_pager_tag, kr);
        goto fail_dealloc;
    }

    /* Join the port set so the fs server's existing mach_msg_receive
     * loop dispatches messages destined to this memory_object. */
    kr = mach_port_move_member(mach_task_self(), mem_obj, g_port_set);
    if (kr != KERN_SUCCESS) {
        printf("%s: fs_pager_create: move_member kr=%d\n",
               g_pager_tag, kr);
        goto fail_dealloc;
    }

    fspager_lock(&g_pager_lock);
    e->mem_obj_port = mem_obj;
    e->file_id      = file_id;
    e->ops          = ops;
    e->state        = state;
    e->initialised  = 0;
    fspager_unlock(&g_pager_lock);

    return mem_obj;

fail_dealloc:
    (void)mach_port_destroy(mach_task_self(), mem_obj);
fail_free:
    fspager_lock(&g_pager_lock);
    fspager_free_locked(e);
    fspager_unlock(&g_pager_lock);
    return MACH_PORT_NULL;
}

void
fs_pager_destroy(uint64_t file_id)
{
    int i;
    mach_port_t to_destroy = MACH_PORT_NULL;

    fspager_lock(&g_pager_lock);
    for (i = 0; i < FSPAGER_MAX_ENTRIES; i++) {
        if (g_pager_table[i].in_use &&
            g_pager_table[i].file_id == file_id) {
            to_destroy = g_pager_table[i].mem_obj_port;
            fspager_free_locked(&g_pager_table[i]);
            break;
        }
    }
    fspager_unlock(&g_pager_lock);

    if (to_destroy != MACH_PORT_NULL)
        (void)mach_port_destroy(mach_task_self(), to_destroy);
}

boolean_t
fs_pager_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
    return seqnos_memory_object_server(in, out);
}

/* ================================================================== */
/*  MIG handler implementations                                         */
/* ================================================================== */

/*
 * memory_object_init — kernel acknowledges our memory object and
 * supplies the control port (back-channel into the kernel's VM, used
 * later for memory_object_data_supply, lock_request, etc.).
 *
 * Marks the table entry initialised and stores control_port.  Refusing
 * (returning != KERN_SUCCESS) would leave the object in limbo; we
 * always accept and log if the port isn't ours (shouldn't happen, but
 * helps debug stray messages).
 */
kern_return_t
seqnos_memory_object_init(
    mach_port_t        mem_obj,
    mach_port_seqno_t  seqno,
    mach_port_t        control_port,
    vm_size_t          pager_page_size)
{
    struct fspager_entry *e;

    (void)seqno; (void)pager_page_size;

    fspager_lock(&g_pager_lock);
    e = fspager_lookup_locked(mem_obj);
    if (e == NULL) {
        fspager_unlock(&g_pager_lock);
        printf("%s: memory_object_init on unknown port 0x%x\n",
               g_pager_tag, (unsigned)mem_obj);
        /* Release the control port the kernel handed us — we won't
         * be using it. */
        (void)mach_port_destroy(mach_task_self(), control_port);
        return KERN_SUCCESS;
    }
    if (e->initialised) {
        fspager_unlock(&g_pager_lock);
        printf("%s: memory_object_init: port 0x%x re-init (was %d)\n",
               g_pager_tag, (unsigned)mem_obj, e->initialised);
        (void)mach_port_destroy(mach_task_self(), control_port);
        return KERN_SUCCESS;
    }
    e->control_port = control_port;
    e->initialised  = 1;
    fspager_unlock(&g_pager_lock);
    return KERN_SUCCESS;
}

/*
 * memory_object_terminate — kernel is done with the object (last
 * mapping released).  We invoke the fs-level close_file callback so
 * the fs can release per-file pager state, then drop the table entry
 * and the receive right.  Control port comes back with MOVE_RECEIVE
 * semantics; release it.
 */
kern_return_t
seqnos_memory_object_terminate(
    mach_port_t        mem_obj,
    mach_port_seqno_t  seqno,
    mach_port_t        control_port)
{
    struct fspager_entry *e;
    uint64_t  file_id = 0;
    const struct fs_pager_ops *ops = NULL;
    void *state = NULL;

    (void)seqno;

    fspager_lock(&g_pager_lock);
    e = fspager_lookup_locked(mem_obj);
    if (e != NULL) {
        file_id = e->file_id;
        ops     = e->ops;
        state   = e->state;
        fspager_free_locked(e);
    }
    fspager_unlock(&g_pager_lock);

    if (ops != NULL && ops->close_file != NULL)
        ops->close_file(state, file_id);

    /* The control_port is MOVE_RECEIVE (see memory_object.defs); we own
     * it now and must release.  Same for the memory_object's receive
     * right we held — destroy both. */
    if (control_port != MACH_PORT_NULL)
        (void)mach_port_destroy(mach_task_self(), control_port);
    (void)mach_port_destroy(mach_task_self(), mem_obj);

    return KERN_SUCCESS;
}

/*
 * memory_object_data_request — page fault.  Phase B leaves this a
 * stub returning KERN_SUCCESS so the kernel doesn't error, but the
 * mapping will read zeros (no memory_object_data_supply ever sent).
 * Phase C calls ops->read_page and replies with data_supply.
 */
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
    return KERN_SUCCESS;
}

/*
 * The remaining seven handlers are Phase D / E concerns; for now
 * accept and ignore so the dispatch table is complete.
 */

kern_return_t
seqnos_memory_object_data_unlock(
    mach_port_t mem_obj, mach_port_seqno_t seqno, mach_port_t control_port,
    vm_offset_t offset, vm_size_t length, vm_prot_t desired_access)
{
    (void)mem_obj; (void)seqno; (void)control_port;
    (void)offset;  (void)length; (void)desired_access;
    return KERN_SUCCESS;
}

kern_return_t
seqnos_memory_object_lock_completed(
    mach_port_t mem_obj, mach_port_seqno_t seqno, mach_port_t control_port,
    vm_offset_t offset, vm_size_t length)
{
    (void)mem_obj; (void)seqno; (void)control_port;
    (void)offset;  (void)length;
    return KERN_SUCCESS;
}

kern_return_t
seqnos_memory_object_supply_completed(
    mach_port_t mem_obj, mach_port_seqno_t seqno, mach_port_t control_port,
    vm_offset_t offset, vm_size_t length,
    kern_return_t result, vm_offset_t error_offset)
{
    (void)mem_obj; (void)seqno; (void)control_port;
    (void)offset;  (void)length; (void)result; (void)error_offset;
    return KERN_SUCCESS;
}

kern_return_t
seqnos_memory_object_data_return(
    mach_port_t mem_obj, mach_port_seqno_t seqno, mach_port_t control_port,
    vm_offset_t offset, pointer_t data, mach_msg_type_number_t data_count,
    boolean_t dirty, boolean_t kernel_copy)
{
    (void)mem_obj; (void)seqno; (void)control_port;
    (void)offset;  (void)data;  (void)data_count;
    (void)dirty;   (void)kernel_copy;
    return KERN_SUCCESS;
}

kern_return_t
seqnos_memory_object_synchronize(
    mach_port_t mem_obj, mach_port_seqno_t seqno, mach_port_t control_port,
    vm_offset_t offset, vm_size_t length, vm_sync_t flags)
{
    (void)mem_obj; (void)seqno; (void)control_port;
    (void)offset;  (void)length; (void)flags;
    return KERN_SUCCESS;
}

kern_return_t
seqnos_memory_object_change_completed(
    mach_port_t mem_obj, mach_port_seqno_t seqno, mach_port_t control_port,
    memory_object_flavor_t flavor)
{
    (void)mem_obj; (void)seqno; (void)control_port; (void)flavor;
    return KERN_SUCCESS;
}

kern_return_t
seqnos_memory_object_discard_request(
    mach_port_t mem_obj, mach_port_seqno_t seqno, mach_port_t control_port,
    vm_offset_t offset, vm_size_t length)
{
    (void)mem_obj; (void)seqno; (void)control_port;
    (void)offset;  (void)length;
    return KERN_SUCCESS;
}
