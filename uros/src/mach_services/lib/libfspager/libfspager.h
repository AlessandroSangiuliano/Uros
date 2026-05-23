/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * libfspager.h — server-side library that turns any Uros file-system
 * server into a Mach memory-object pager (#276).
 *
 * Why a library?
 * --------------
 * File-backed mmap requires the fs server to act as the Mach pager for
 * each mapped file: hand out memory_object send rights, answer
 * memory_object_data_request faults with page content, accept dirty
 * pages via memory_object_data_return.  All of that is identical
 * boilerplate across filesystems; the only fs-specific part is the
 * underlying block I/O.  We split it into a library so every fs server
 * (today: ext_server; later: fs_uros_server, fat_server, ufs_server)
 * picks up mmap by linking us and providing four block-oriented
 * callbacks — no Mach VM code in the fs server itself.
 *
 * Two-pager model
 * ---------------
 * libfspager is the file pager and is per-fs (one server task, many
 * memory_object ports — one per mapped file).  The system default
 * pager (default_pager) is unrelated and untouched: it keeps paging
 * out anonymous memory.  Kernel routes faults by vm_object backing:
 * anonymous → default_pager; file-backed → the fs server that owns
 * the file's memory_object.
 *
 * Scope phases (#276)
 * -------------------
 *   A — this file: API + MIG handler stubs that compile and dispatch
 *       but don't yet allocate ports or move data.  No client.
 *   B — port allocation, lifecycle (init/terminate), in-memory table;
 *       ext_server links + adds fs_mmap RPC; first end-to-end vm_map
 *       with stub read_page returning zeros.
 *   C — real read_page in ext_server (delegates to its read path /
 *       page cache); dlopen_test passes.
 *   D — write_page + msync + MAP_SHARED coherence with the fs page
 *       cache.
 *   E (separate issue) — fs_uros_server adopts libfspager unchanged.
 *
 * Versioning ([[feedback_versioning_bsd]]): bumped per BSD rules — major
 * on callback signature change, minor on new optional ops, patch on
 * fixes.  Phase A is the first cut, so 0.1.0.
 */

#ifndef _LIBFSPAGER_LIBFSPAGER_H_
#define _LIBFSPAGER_LIBFSPAGER_H_

#include <mach.h>
#include <mach/message.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIBFSPAGER_VERSION_MAJOR 0
#define LIBFSPAGER_VERSION_MINOR 1
#define LIBFSPAGER_VERSION_PATCH 0
#define LIBFSPAGER_VERSION_STRING "0.1.0"

/*
 * fs_pager_ops — the four block-oriented callbacks an fs server must
 * provide.  Intentionally byte/offset-based, not extent-based, so any
 * filesystem shape can plug in (ext2/3/4 block, FAT cluster, UFS frag,
 * future extent-based / log-structured / capability-native).
 *
 *   read_page  — pager needs file bytes [off, off+len).  Fill `buf`.
 *                Returns 0 on success, -errno on failure.  Short reads
 *                past EOF should fill the tail with zeros and return 0
 *                so the kernel sees a zero-padded final page.
 *
 *   write_page — kernel returned dirty bytes [off, off+len) for write-
 *                back.  Persist them.  Returns 0 on success, -errno on
 *                failure.  Phase D feature; safe to leave a stub
 *                returning -ENOSYS in earlier phases.
 *
 *   close_file — last mapping went away; fs may release any per-file
 *                pager state.  Not the same as the fs's own close —
 *                a file can stay open via vfs while every mmap of it
 *                is gone.
 *
 *   file_size  — current logical size of the file in bytes; libfspager
 *                uses it to clip read/write requests at EOF.
 */
struct fs_pager_ops {
    int      (*read_page) (void *state, uint64_t file_id,
                           uint64_t off, void *buf, unsigned int len);
    int      (*write_page)(void *state, uint64_t file_id,
                           uint64_t off, const void *buf,
                           unsigned int len);
    void     (*close_file)(void *state, uint64_t file_id);
    uint64_t (*file_size) (void *state, uint64_t file_id);
};

/*
 * fs_pager_init — one-shot library init for the fs server.  Must be
 * called before any fs_pager_create.  `tag` is used in log messages
 * (not copied — keep alive).
 *
 * Phase A: records the tag; no port-set or table yet.
 */
void fs_pager_init(const char *tag);

/*
 * fs_pager_create — register a file with libfspager and get back the
 * memory_object send right the fs server hands to its mmap-RPC client.
 *
 *   file_id — fs-specific opaque identifier (inode, fid, whatever).
 *             Passed back verbatim to every callback.
 *   ops     — the four callbacks above; not copied (keep alive).
 *   state   — opaque cookie passed to each callback.
 *
 * Returns MACH_PORT_NULL on failure.  Phase A: always MACH_PORT_NULL
 * (logs "not yet implemented" so the fs server's mmap path fails
 * loudly until Phase B).
 */
mach_port_t fs_pager_create(uint64_t file_id,
                            const struct fs_pager_ops *ops,
                            void *state);

/*
 * fs_pager_demux — message demultiplexer to OR into the fs server's
 * own demux.  Wraps seqnos_memory_object_server (the MIG dispatch
 * generated from <mach/memory_object.defs> with -DSEQNOS).
 *
 * Usage in the fs server's existing demux:
 *
 *   boolean_t my_fs_demux(mach_msg_header_t *in,
 *                         mach_msg_header_t *out)
 *   {
 *       return my_fs_orig_demux(in, out)
 *           || fs_pager_demux(in, out);
 *   }
 *
 * Phase A: forwards to the MIG dispatch; the dispatch lands in our
 * stub handlers (below in libfspager.c).
 */
boolean_t fs_pager_demux(mach_msg_header_t *in, mach_msg_header_t *out);

#ifdef __cplusplus
}
#endif

#endif /* _LIBFSPAGER_LIBFSPAGER_H_ */
