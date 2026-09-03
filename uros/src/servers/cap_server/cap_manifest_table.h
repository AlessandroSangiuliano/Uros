/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * cap_manifest_table.h — per-task manifest registry consulted by
 * cap_server's policy layer (#216 v2.1).
 *
 * Each entry maps a per-task cap_port (held receive-side by
 * cap_server, send right handed back to the child task at
 * provisioning) to a validated copy of that task's manifest blob.
 * Identity == port: only the task that holds the send right can
 * make calls arriving on this receive port, so the port name
 * doubles as the principal.
 */

#ifndef _CAP_MANIFEST_TABLE_H_
#define _CAP_MANIFEST_TABLE_H_

#include <mach/cap_manifest.h>
#include <mach/port.h>
#include <mach/kern_return.h>
#include <stdint.h>

/* Initialise the table to empty.  Idempotent. */
void cap_manifest_table_init(void);

/*
 * Validate a freshly-received manifest blob.  Returns CAP_ERR_NONE
 * on success (caller may then call cap_manifest_table_install) or
 * CAP_ERR_MANIFEST_INVALID if any header field, offset, or array
 * size doesn't survive bounds checking against `blob_len`.
 *
 * On success *out_hdr points into `blob` so the caller doesn't have
 * to re-parse.  blob must outlive any subsequent reads via the
 * returned pointer; the table makes its own copy at install time.
 */
int cap_manifest_validate(const void *blob, uint32_t blob_len,
                          const cap_manifest_header_t **out_hdr);

/*
 * Register a manifest under `task_cap_port` (cap_server's receive
 * name).  Copies the blob into table-owned storage so the caller
 * can free its own buffer.  Returns CAP_ERR_NONE,
 * CAP_ERR_NO_MANIFEST_SLOT, or CAP_ERR_NO_MEMORY.
 */
/*
 * ⚠️ `task_port' is remembered, and #216 said it would be: cap_provision_task
 * carries it as "informational for now (audit / future cross-reference)".
 * #432 is that cross-reference -- issuing a capability for a DMA buffer means
 * asking the KERNEL whether the requesting task owns it, and the requester is
 * known here only as a port.
 */
int cap_manifest_table_install(mach_port_t task_cap_port,
                               mach_port_t task_port,
                               const void *blob, uint32_t blob_len);

/*
 * Look up the manifest installed for a given per-task cap_port.
 * Returns NULL when the port has no entry (legacy / well-known
 * path) — the caller treats that as "no enforcement, permissive".
 */
const cap_manifest_header_t *
cap_manifest_table_get(mach_port_t task_cap_port);

/* The task behind that cap_port, or MACH_PORT_NULL. */
mach_port_t cap_manifest_table_task(mach_port_t task_cap_port);

/*
 * Helper: does the given manifest declare `type + resource_id + ops' in its
 * `caps_required' list?  Returns 1 when allowed, 0 when not.  NULL manifest is
 * treated as permissive (returns 1) so callers can pass through the legacy
 * path without special-casing.
 *
 * 🔴 `resource_id' IS THE V2 ADDITION.  A version-1 manifest could say "block
 * devices, with READ" and could not say WHICH -- so a task allowed one was
 * allowed all of them, and the file constrained the kind of authority without
 * constraining its extent.  An entry declaring CAP_MANIFEST_ANY_ID means what
 * every v1 entry meant, and says so.
 */
int cap_manifest_allows(const cap_manifest_header_t *m,
                        uint32_t type, uint64_t resource_id, uint64_t ops);

#endif /* _CAP_MANIFEST_TABLE_H_ */
