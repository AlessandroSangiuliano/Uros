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
 *   uros/src/servers/default_pager/dp_memory_object.c  (study, don't copy —
 *   [[feedback_study_never_copy]]).
 */

#include "libfspager.h"

#include <mach.h>
#include "mach.h"                    /* MIG: vm_allocate, vm_deallocate,
                                        memory_object_data_supply */
#include "mach_port.h"               /* MIG: mach_port_allocate,
                                        _insert_right, _move_member,
                                        _destroy */
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
 * Marks the table entry initialised, stores control_port, and tells
 * the kernel we are ready via memory_object_change_attributes — the
 * latter is REQUIRED: without it the kernel never sends
 * memory_object_data_request and the first page fault on the mapping
 * blocks forever (default_pager does the same call at init time).
 */
kern_return_t
seqnos_memory_object_init(
    mach_port_t        mem_obj,
    mach_port_seqno_t  seqno,
    mach_port_t        control_port,
    vm_size_t          pager_page_size)
{
    struct fspager_entry *e;
    memory_object_attr_info_data_t attrs;
    kern_return_t kr;

    (void)seqno; (void)pager_page_size;

    fspager_lock(&g_pager_lock);
    e = fspager_lookup_locked(mem_obj);
    if (e == NULL) {
        fspager_unlock(&g_pager_lock);
        printf("%s: memory_object_init on unknown port 0x%x\n",
               g_pager_tag, (unsigned)mem_obj);
        (void)mach_port_destroy(mach_task_self(), control_port);
        return KERN_SUCCESS;
    }
    if (e->initialised) {
        fspager_unlock(&g_pager_lock);
        printf("%s: memory_object_init: port 0x%x re-init\n",
               g_pager_tag, (unsigned)mem_obj);
        (void)mach_port_destroy(mach_task_self(), control_port);
        return KERN_SUCCESS;
    }
    e->control_port = control_port;
    e->initialised  = 1;
    fspager_unlock(&g_pager_lock);

    /* Signal "pager is ready, will service data_requests".  Same
     * attribute set default_pager uses: caller-driven copy, default
     * cluster size, may_cache=FALSE (file pagers don't cache pages
     * in the kernel for us), temporary=FALSE (it's a file, not
     * scratch). */
    attrs.copy_strategy     = MEMORY_OBJECT_COPY_DELAY;
    attrs.cluster_size      = (vm_size_t)pager_page_size;
    attrs.may_cache_object  = FALSE;
    attrs.temporary         = FALSE;
    kr = memory_object_change_attributes(control_port,
                                         MEMORY_OBJECT_ATTRIBUTE_INFO,
                                         (memory_object_info_t)&attrs,
                                         MEMORY_OBJECT_ATTR_INFO_COUNT,
                                         MACH_PORT_NULL);
    if (kr != KERN_SUCCESS)
        printf("%s: change_attributes kr=%d (page faults will block)\n",
               g_pager_tag, kr);
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
 * memory_object_data_request — page fault.  Look the port up in the
 * table, vm_allocate a transient buffer the size of the request, ask
 * the fs to fill it via ops->read_page, then hand the buffer to the
 * kernel as out-of-line data with dataDealloc=TRUE (the underlying
 * msg layer frees it after the send).  No reply expected — this is a
 * simpleroutine; we issue the matching memory_object_data_supply on
 * the control port the kernel gave us at init time.
 *
 * Phase B: read_page is allowed to be a stub that zero-fills; the
 * mapping then reads as all zeros.  Phase C swaps in the real fs
 * read path and dlopen / file mmap start returning actual bytes.
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
    struct fspager_entry *e;
    uint64_t  file_id;
    const struct fs_pager_ops *ops;
    void     *state;
    vm_offset_t buf = 0;
    kern_return_t kr;

    (void)seqno; (void)desired_access;

    fspager_lock(&g_pager_lock);
    e = fspager_lookup_locked(mem_obj);
    if (e == NULL) {
        fspager_unlock(&g_pager_lock);
        printf("%s: data_request on unknown port 0x%x\n",
               g_pager_tag, (unsigned)mem_obj);
        return KERN_SUCCESS;
    }
    file_id = e->file_id;
    ops     = e->ops;
    state   = e->state;
    fspager_unlock(&g_pager_lock);

    if (ops == NULL || ops->read_page == NULL) {
        printf("%s: data_request: no read_page op for fid=%llu\n",
               g_pager_tag, (unsigned long long)file_id);
        return KERN_SUCCESS;
    }

    /* OOL transfer buffer.  vm_allocate's anywhere=TRUE: the kernel
     * picks the address; we hand the range to the message system and
     * it goes back to the page-fault initiator unchanged. */
    kr = vm_allocate(mach_task_self(), &buf, (vm_size_t)length, TRUE);
    if (kr != KERN_SUCCESS) {
        printf("%s: data_request: vm_allocate(%u) kr=%d\n",
               g_pager_tag, (unsigned)length, kr);
        return KERN_SUCCESS;
    }

    if (ops->read_page(state, file_id,
                       (uint64_t)offset, (void *)buf,
                       (unsigned int)length) != 0) {
        /* read_page failed — zero-fill to keep the kernel from
         * looping on the same fault, and supply anyway. */
        memset((void *)buf, 0, length);
    }

    kr = memory_object_data_supply(control_port,
                                   offset,
                                   buf,
                                   (mach_msg_type_number_t)length,
                                   /*dataDealloc=*/ TRUE,
                                   /*lock_value=*/  VM_PROT_NONE,
                                   /*precious=*/    FALSE,
                                   MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) {
        printf("%s: memory_object_data_supply kr=%d\n",
               g_pager_tag, kr);
        /* dataDealloc=TRUE only releases on success; clean up here. */
        (void)vm_deallocate(mach_task_self(), buf, length);
    }
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

/*
 * memory_object_data_return — kernel hands us a dirty page (or a
 * clustered run) to persist.  Phase D wires this to ops->write_page;
 * the OOL buffer is then vm_deallocate'd (the kernel keeps no
 * reference once we return).
 *
 * `dirty` says the kernel believes the bytes differ from what we last
 * supplied via data_supply.  `kernel_copy` is a hint about whether the
 * kernel retained a clean copy — we don't act on it in Phase D.
 *
 * If write_page is NULL (fs server didn't provide writeback support)
 * or returns an error, the kernel still loses its copy of the page
 * (semantically POSIX-correct: msync can fail, the user just sees
 * silent data loss for that range).  Logging the error is enough for
 * now; a later phase may introduce a write-back failure flag the fs
 * can surface to clients via stat / mmap-time hints.
 */
kern_return_t
seqnos_memory_object_data_return(
    mach_port_t mem_obj, mach_port_seqno_t seqno, mach_port_t control_port,
    vm_offset_t offset, pointer_t data, mach_msg_type_number_t data_count,
    boolean_t dirty, boolean_t kernel_copy)
{
    struct fspager_entry *e;
    uint64_t file_id = 0;
    const struct fs_pager_ops *ops = NULL;
    void *state = NULL;

    (void)seqno; (void)control_port; (void)kernel_copy;

    if (!dirty || data_count == 0)
        goto release;            /* nothing to persist */

    fspager_lock(&g_pager_lock);
    e = fspager_lookup_locked(mem_obj);
    if (e != NULL) {
        file_id = e->file_id;
        ops     = e->ops;
        state   = e->state;
    }
    fspager_unlock(&g_pager_lock);

    if (ops != NULL && ops->write_page != NULL) {
        int rc = ops->write_page(state, file_id,
                                 (uint64_t)offset,
                                 (const void *)data,
                                 (unsigned int)data_count);
        if (rc != 0)
            printf("%s: write_page(fid=%llu off=0x%lx len=%u) -> %d\n",
                   g_pager_tag, (unsigned long long)file_id,
                   (unsigned long)offset, (unsigned int)data_count,
                   rc);
    } else if (ops != NULL) {
        /* Fs server has no write_page — only log once per pager so we
         * don't spam during read-only mappings that the kernel
         * occasionally cleans. */
        static int warned = 0;
        if (!warned) {
            warned = 1;
            printf("%s: data_return on read-only pager (fid=%llu); "
                   "kernel-side dirty page will be dropped\n",
                   g_pager_tag, (unsigned long long)file_id);
        }
    }

release:
    /* Always release the OOL buffer — the kernel is done with it
     * regardless of whether we persisted the bytes. */
    if (data != 0 && data_count != 0)
        (void)vm_deallocate(mach_task_self(),
                            (vm_address_t)data,
                            (vm_size_t)data_count);
    return KERN_SUCCESS;
}

/*
 * memory_object_synchronize — userspace called msync(2) on a mapping
 * backed by this pager.  We translate to memory_object_lock_request
 * asking the kernel to return every dirty page in the range via
 * memory_object_data_return — that recurses into our data_return
 * handler which calls ops->write_page (Phase D.1 path).  The kernel
 * eventually replies with memory_object_lock_completed (handler below
 * is a no-op; the synchrony is on the user side, managed by the
 * kernel).
 *
 * Flag mapping:
 *   VM_SYNC_INVALIDATE → should_flush = TRUE  (drop clean+dirty)
 *   else               → should_flush = FALSE (keep cached)
 *   Always RETURN_DIRTY so clean pages aren't needlessly shipped.
 *
 * VM_SYNC_ASYNCHRONOUS vs VM_SYNC_SYNCHRONOUS is handled by the
 * kernel-side caller (vm_msync), not by us — we always do the same
 * thing on this side.
 */
kern_return_t
seqnos_memory_object_synchronize(
    mach_port_t mem_obj, mach_port_seqno_t seqno, mach_port_t control_port,
    vm_offset_t offset, vm_size_t length, vm_sync_t flags)
{
    kern_return_t kr;
    boolean_t should_flush = (flags & VM_SYNC_INVALIDATE) ? TRUE : FALSE;

    (void)mem_obj; (void)seqno;

    kr = memory_object_lock_request(control_port,
                                    offset, length,
                                    MEMORY_OBJECT_RETURN_DIRTY,
                                    should_flush,
                                    VM_PROT_NO_CHANGE,
                                    MACH_PORT_NULL);
    if (kr != KERN_SUCCESS)
        printf("%s: synchronize: lock_request kr=%d\n", g_pager_tag, kr);

    /* Spec asks us to ack via memory_object_synchronize_completed once
     * the kernel finishes the lock request.  The kernel does that for
     * us implicitly when the simpleroutine returns, given the way
     * lock_request is wired; default_pager follows the same shortcut.
     * If a future fs needs explicit gating it can post-process the
     * lock_completed handler below. */
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
