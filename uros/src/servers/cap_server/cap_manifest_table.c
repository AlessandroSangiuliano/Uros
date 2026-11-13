/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * cap_manifest_table.c — see cap_manifest_table.h for the contract.
 * Implementation kept deliberately small: fixed-size table, linear
 * scan, no locking (cap_server is single-threaded today).
 */

#include "cap_manifest_table.h"

#include <mach/cap_types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP_MANIFEST_TABLE_SIZE  64

struct cap_manifest_slot {
    int            in_use;
    mach_port_t    port;       /* per-task cap_port receive name */
    mach_port_t    task;       /* the task it was provisioned for (#432) */
    void          *blob;       /* owned copy, freed on uninstall */
    uint32_t       len;
};

static struct cap_manifest_slot g_table[CAP_MANIFEST_TABLE_SIZE];

void
cap_manifest_table_init(void)
{
    memset(g_table, 0, sizeof(g_table));
}

/* ------------------------------------------------------------------ */
/*  Validation                                                          */
/* ------------------------------------------------------------------ */

/*
 * range_ok — offset + length fits in [0, blob_len] without overflow.
 */
static int
range_ok(uint32_t off, uint32_t len, uint32_t total)
{
    if (off > total) return 0;
    if (len > total - off) return 0;
    return 1;
}

int
cap_manifest_validate(const void *blob, uint32_t blob_len,
                      const cap_manifest_header_t **out_hdr)
{
    const cap_manifest_header_t *h;

    if (!blob || blob_len < sizeof(cap_manifest_header_t))
        return CAP_ERR_MANIFEST_INVALID;
    if (blob_len > CAP_MANIFEST_MAX_BYTES)
        return CAP_ERR_MANIFEST_INVALID;

    h = (const cap_manifest_header_t *)blob;
    if (h->magic != CAP_MANIFEST_MAGIC)            return CAP_ERR_MANIFEST_INVALID;
    if (h->version != CAP_MANIFEST_VERSION)        return CAP_ERR_MANIFEST_INVALID;
    if (h->header_size < sizeof(*h))               return CAP_ERR_MANIFEST_INVALID;
    if (h->total_size != blob_len)                 return CAP_ERR_MANIFEST_INVALID;
    if (h->flags != 0)                             return CAP_ERR_MANIFEST_INVALID;

    /* Entry arrays — verify offset + count*entry_size fits. */
    if (h->required_count > CAP_MANIFEST_MAX_ENTRIES)
        return CAP_ERR_MANIFEST_INVALID;
    if (h->delegatable_count > CAP_MANIFEST_MAX_ENTRIES)
        return CAP_ERR_MANIFEST_INVALID;
    if (!range_ok(h->required_offset,
                  h->required_count * sizeof(cap_manifest_entry_t),
                  blob_len))
        return CAP_ERR_MANIFEST_INVALID;
    if (!range_ok(h->delegatable_offset,
                  h->delegatable_count * sizeof(cap_manifest_entry_t),
                  blob_len))
        return CAP_ERR_MANIFEST_INVALID;

    /* Name region — bounded length + NUL terminator inside. */
    if (h->name_length == 0 || h->name_length > CAP_MANIFEST_MAX_NAME)
        return CAP_ERR_MANIFEST_INVALID;
    if (!range_ok(h->name_offset, h->name_length, blob_len))
        return CAP_ERR_MANIFEST_INVALID;
    {
        const char *name = (const char *)blob + h->name_offset;
        if (name[h->name_length - 1] != '\0')
            return CAP_ERR_MANIFEST_INVALID;
    }

    if (out_hdr) *out_hdr = h;
    return CAP_ERR_NONE;
}

/* ------------------------------------------------------------------ */
/*  Install / lookup                                                    */
/* ------------------------------------------------------------------ */

int
cap_manifest_table_install(mach_port_t task_cap_port,
                           mach_port_t task_port,
                           const void *blob, uint32_t blob_len)
{
    int slot = -1;
    int i;
    void *copy;

    for (i = 0; i < CAP_MANIFEST_TABLE_SIZE; i++) {
        if (!g_table[i].in_use) {
            if (slot < 0) slot = i;
        } else if (g_table[i].port == task_cap_port) {
            /* Re-install on the same port name — replace in place. */
            slot = i;
            free(g_table[i].blob);
            g_table[i].blob = NULL;
            g_table[i].len  = 0;
            g_table[i].in_use = 0;
            break;
        }
    }
    if (slot < 0)
        return CAP_ERR_NO_MANIFEST_SLOT;

    copy = malloc(blob_len);
    if (!copy)
        return CAP_ERR_NO_MEMORY;
    memcpy(copy, blob, blob_len);

    g_table[slot].in_use = 1;
    g_table[slot].port   = task_cap_port;
    g_table[slot].task   = task_port;
    g_table[slot].blob   = copy;
    g_table[slot].len    = blob_len;
    return CAP_ERR_NONE;
}

mach_port_t
cap_manifest_table_task(mach_port_t task_cap_port)
{
    for (uint32_t i = 0; i < CAP_MANIFEST_TABLE_SIZE; i++)
        if (g_table[i].in_use && g_table[i].port == task_cap_port)
            return g_table[i].task;

    return MACH_PORT_NULL;
}

unsigned
cap_manifest_table_in_use(void)
{
    unsigned n = 0;

    for (uint32_t i = 0; i < CAP_MANIFEST_TABLE_SIZE; i++)
        if (g_table[i].in_use)
            n++;
    return n;
}

unsigned
cap_manifest_table_size(void)
{
    return CAP_MANIFEST_TABLE_SIZE;
}

mach_port_t
cap_manifest_table_remove(mach_port_t task_cap_port)
{
    mach_port_t task;

    for (uint32_t i = 0; i < CAP_MANIFEST_TABLE_SIZE; i++)
        if (g_table[i].in_use && g_table[i].port == task_cap_port) {
            task = g_table[i].task;
            free(g_table[i].blob);
            memset(&g_table[i], 0, sizeof(g_table[i]));
            return task;
        }

    return MACH_PORT_NULL;
}

const cap_manifest_header_t *
cap_manifest_table_get(mach_port_t task_cap_port)
{
    int i;
    for (i = 0; i < CAP_MANIFEST_TABLE_SIZE; i++) {
        if (g_table[i].in_use && g_table[i].port == task_cap_port)
            return (const cap_manifest_header_t *)g_table[i].blob;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Derivation (#743)                                                   */
/* ------------------------------------------------------------------ */

static const cap_manifest_entry_t *
manifest_entries(const cap_manifest_header_t *m, uint32_t offset)
{
    return (const cap_manifest_entry_t *)((const char *)m + offset);
}

/*
 * One list of `asked', cut to what `may' lets a parent hand on.  An entry
 * survives for a type `may' names, for an instance it names -- one that asks
 * for any instance gets the instance `may' names, or any if `may' says any --
 * and for the operations both name; an entry left with no operation is
 * dropped.  Answers how many entries went to `out', or -1 when more than
 * CAP_MANIFEST_MAX_ENTRIES would.
 */
static int
derive_list(const cap_manifest_entry_t *asked, uint32_t n_asked,
            const cap_manifest_entry_t *may, uint32_t n_may,
            cap_manifest_entry_t *out)
{
    int n = 0;

    for (uint32_t i = 0; i < n_asked; i++) {
        for (uint32_t j = 0; j < n_may; j++) {
            uint64_t id, ops;

            if (asked[i].type != may[j].type)
                continue;
            if (asked[i].resource_id == CAP_MANIFEST_ANY_ID)
                id = may[j].resource_id;
            else if (may[j].resource_id == CAP_MANIFEST_ANY_ID ||
                     may[j].resource_id == asked[i].resource_id)
                id = asked[i].resource_id;
            else
                continue;
            ops = asked[i].ops & may[j].ops;
            if (ops == 0)
                continue;
            if (n == CAP_MANIFEST_MAX_ENTRIES)
                return -1;
            out[n].type        = asked[i].type;
            out[n]._pad        = 0;
            out[n].resource_id = id;
            out[n].ops         = ops;
            n++;
        }
    }
    return n;
}

int
cap_manifest_derive(const cap_manifest_header_t *parent,
                    const cap_manifest_header_t *asked,
                    void **out_blob, uint32_t *out_len)
{
    cap_manifest_entry_t req[CAP_MANIFEST_MAX_ENTRIES];
    cap_manifest_entry_t del[CAP_MANIFEST_MAX_ENTRIES];
    const cap_manifest_entry_t *may;
    cap_manifest_header_t *h;
    uint32_t name_off, total;
    char *blob;
    int nr, nd;

    /*
     * 🔑 BOTH LISTS ARE CUT BY THE PARENT'S delegatable[].  The child's
     * required[] is what it may hold; its delegatable[] is what it may hand
     * on in turn, and a grandchild must get no more than the parent could
     * have given -- so the parent's limit is carried down a whole chain of
     * execs, and the child's own words can only narrow it.
     */
    may = manifest_entries(parent, parent->delegatable_offset);
    nr = derive_list(manifest_entries(asked, asked->required_offset),
                     asked->required_count, may, parent->delegatable_count,
                     req);
    nd = derive_list(manifest_entries(asked, asked->delegatable_offset),
                     asked->delegatable_count, may,
                     parent->delegatable_count, del);
    if (nr < 0 || nd < 0)
        return CAP_ERR_MANIFEST_INVALID;

    name_off = (uint32_t)(sizeof(*h) +
                          (uint32_t)(nr + nd) * sizeof(cap_manifest_entry_t));
    total = name_off + ((asked->name_length + 3u) & ~3u);
    blob = malloc(total);
    if (blob == NULL)
        return CAP_ERR_NO_MEMORY;
    memset(blob, 0, total);

    h = (cap_manifest_header_t *)blob;
    h->magic              = CAP_MANIFEST_MAGIC;
    h->version            = CAP_MANIFEST_VERSION;
    h->header_size        = (uint16_t)sizeof(*h);
    h->total_size         = total;
    h->flags              = 0;
    h->required_offset    = (uint32_t)sizeof(*h);
    h->required_count     = (uint32_t)nr;
    h->delegatable_offset = (uint32_t)(sizeof(*h) +
                                       (uint32_t)nr * sizeof(cap_manifest_entry_t));
    h->delegatable_count  = (uint32_t)nd;
    h->name_offset        = name_off;
    h->name_length        = asked->name_length;
    memcpy(blob + h->required_offset, req,
           (size_t)nr * sizeof(cap_manifest_entry_t));
    memcpy(blob + h->delegatable_offset, del,
           (size_t)nd * sizeof(cap_manifest_entry_t));
    memcpy(blob + name_off, (const char *)asked + asked->name_offset,
           asked->name_length);

    /* What this function builds, the table's own check must accept. */
    if (cap_manifest_validate(blob, total, NULL) != CAP_ERR_NONE) {
        free(blob);
        return CAP_ERR_INTERNAL;
    }
    *out_blob = blob;
    *out_len  = total;
    return CAP_ERR_NONE;
}

/* ------------------------------------------------------------------ */
/*  Predicate                                                           */
/* ------------------------------------------------------------------ */

int
cap_manifest_allows(const cap_manifest_header_t *m,
                    uint32_t type, uint64_t resource_id, uint64_t ops)
{
    const cap_manifest_entry_t *req;
    uint32_t i;

    /*
     * ── What "this task shipped no manifest" means (#511) ──────────────
     *
     * 🔴 IT MEANT "ALLOW EVERYTHING", and it meant it at the CALL SITES:
     * both of them read `if (m && !cap_manifest_allows(...))', so with no
     * manifest this predicate was never reached at all.  Ten manifests
     * exist in a tree with dozens of tasks, and none of them belongs to a
     * server that drives hardware -- so the one policy file this system has
     * was silent about exactly the tasks it most needed to speak for, and
     * any check downstream of it was a guard that could not fail for them.
     *
     * 🔑 The decision lives HERE now, in the one function that answers the
     * question, rather than in a `m &&' repeated at every place that asks
     * it.  While it was spelt at the call sites, a third call site added
     * later would have got the old answer by default and nobody would have
     * had to decide anything.
     *
     * 🔑 NO MANIFEST NOW MEANS NO CAPABILITY, full stop, and it got there
     * in two steps on purpose.  The first refused hardware alone, which was
     * free -- the census said no request relied on the legacy path for
     * anything else.  The second is this one, taken once the census read
     * zero for every resource: three requests before, from two tasks, none
     * after those two were given policy files.
     *
     * ⚠️ Reaching this with a null manifest is now a task holding a
     * per-task cap port whose manifest is not in the table, which is a
     * broken installation rather than a policy question.  Refusing is the
     * only honest answer to it.
     */
    if (!m)
        return 0;

    req = (const cap_manifest_entry_t *)((const char *)m +
                                         m->required_offset);
    for (i = 0; i < m->required_count; i++) {
        if (req[i].type != type) continue;

        /*
         * 🔴 THE INSTANCE AS WELL AS THE KIND (v2).  A version-1 entry could
         * only say "block devices, with READ" -- so a task allowed one block
         * device was allowed every block device, and the manifest constrained
         * the KIND of authority without constraining its extent.
         *
         * CAP_MANIFEST_ANY_ID is what every v1 entry meant, and it is spelt
         * rather than implied: an entry that names a resource names it, and
         * one that means "any" says so.
         */
        if (req[i].resource_id != CAP_MANIFEST_ANY_ID &&
            req[i].resource_id != resource_id)
            continue;

        /* All requested ops must be subset of declared ops. */
        if ((ops & ~req[i].ops) == 0)
            return 1;
    }
    return 0;
}
