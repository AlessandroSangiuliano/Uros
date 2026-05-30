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
    g_table[slot].blob   = copy;
    g_table[slot].len    = blob_len;
    return CAP_ERR_NONE;
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
/*  Predicate                                                           */
/* ------------------------------------------------------------------ */

int
cap_manifest_allows(const cap_manifest_header_t *m,
                    uint32_t type, uint64_t ops)
{
    const cap_manifest_entry_t *req;
    uint32_t i;

    if (!m)
        return 1;       /* permissive: no manifest -> legacy path */

    req = (const cap_manifest_entry_t *)((const char *)m +
                                         m->required_offset);
    for (i = 0; i < m->required_count; i++) {
        if (req[i].type != type) continue;
        /* All requested ops must be subset of declared ops. */
        if ((ops & ~req[i].ops) == 0)
            return 1;
    }
    return 0;
}
