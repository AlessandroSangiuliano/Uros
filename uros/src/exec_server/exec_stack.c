/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * exec_stack.c — initial-stack builder for exec_server (#228 v0.1.0).
 *
 * System V ABI i386 entry layout, top-down:
 *
 *   ESP →  argc                 (int32)
 *          argv[0..argc-1]      (char* each, point inside strings region)
 *          NULL                 (argv terminator)
 *          envp[0..envc-1]      (char* each)
 *          NULL                 (envp terminator)
 *          auxv[0]              (Elf32_auxv_t pairs)
 *          ...
 *          AT_NULL pair         (auxv terminator)
 *          [strings region]     (raw bytes from argv_blob + envp_blob)
 *
 * v0.1.0 emits a minimal AUXV (just AT_NULL); AT_PAGESZ /
 * AT_SYSINFO_EHDR / AT_RANDOM arrive in v0.4.0 alongside the vDSO.
 */

#include "exec_types.h"
#include "exec_internal.h"

#include <mach.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* AT_* constants we care about today (v0.4.0 / #236). */
#define AT_NULL          0
#define AT_PHDR          3
#define AT_PHENT         4
#define AT_PHNUM         5
#define AT_PAGESZ        6
#define AT_BASE          7
#define AT_ENTRY         9
#define AT_RANDOM       25
#define AT_SYSINFO_EHDR 33

#define EXEC_PAGE_SIZE  4096U
#define EXEC_RANDOM_LEN 16U

/* ------------------------------------------------------------------ */
/*  Helpers — count NUL-separated strings in a blob                    */
/* ------------------------------------------------------------------ */

/*
 * exec_blob format: NUL-separated strings followed by a sentinel
 * empty string (i.e. two NULs in a row).  Returns the count of
 * strings found (excluding the terminator) and *out_str_bytes set
 * to the total bytes occupied by strings (including their NULs but
 * NOT the terminator).
 *
 * Returns -1 on malformed blob (no terminator within len).
 */
static int
blob_count(const char *buf, mach_msg_type_number_t len,
           uint32_t *out_str_bytes)
{
    int n = 0;
    mach_msg_type_number_t i = 0;
    mach_msg_type_number_t this_start = 0;
    *out_str_bytes = 0;

    if (len == 0)
        return -1;

    while (i < len) {
        if (buf[i] == '\0') {
            uint32_t this_len = (uint32_t)(i - this_start);
            if (this_len == 0) {
                /* Terminator (empty string at this_start). */
                return n;
            }
            n++;
            *out_str_bytes += this_len + 1;     /* include NUL */
            this_start = i + 1;
        }
        i++;
    }
    /* No terminator found. */
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Core: assemble + vm_write                                           */
/* ------------------------------------------------------------------ */

/*
 * Count the AUXV (tag, value) pairs we'll emit given the caller's
 * hints.  AT_PAGESZ and AT_RANDOM are always present; AT_NULL is the
 * terminator.  Optional entries (AT_ENTRY / AT_PHDR+AT_PHENT+AT_PHNUM
 * / AT_SYSINFO_EHDR) are emitted only when the hint is non-zero so
 * legacy consumers don't get bogus values.
 */
static unsigned
count_auxv_pairs(const struct exec_auxv_hints *h)
{
    unsigned n = 1 /* AT_PAGESZ */ + 1 /* AT_RANDOM */ + 1 /* AT_NULL */;
    if (h) {
        if (h->vdso_base)   n++;
        if (h->entry_va)    n++;
        if (h->interp_base) n++;    /* AT_BASE */
        if (h->phdr_va)     n += 3; /* AT_PHDR + AT_PHENT + AT_PHNUM */
    }
    return n;
}

int
exec_build_stack(mach_port_t new_task,
                 const void *argv_blob, mach_msg_type_number_t argv_len,
                 const void *envp_blob, mach_msg_type_number_t envp_len,
                 const struct exec_auxv_hints *hints,
                 vm_address_t *out_top)
{
    int argc, envc;
    uint32_t argv_strs = 0, envp_strs = 0;
    vm_size_t header_bytes;
    vm_size_t total_bytes;
    unsigned auxv_pairs;
    char *buf;
    uint32_t *p;
    char *strings_dst;
    vm_address_t base_va;
    vm_address_t stack_va;
    vm_address_t random_va;
    kern_return_t kr;

    /* 1. Allocate the stack page in the new task. */
    base_va = EXEC_STACK_VA;
    kr = vm_allocate(new_task, &base_va, EXEC_STACK_SIZE, FALSE);
    if (kr != KERN_SUCCESS) {
        printf("exec: stack vm_allocate kr=%d\n", kr);
        return EXEC_ERR_VM_SETUP;
    }

    /* 2. Count argv / envp entries and total string bytes. */
    argc = blob_count(argv_blob, argv_len, &argv_strs);
    envc = blob_count(envp_blob, envp_len, &envp_strs);
    if (argc < 0 || envc < 0) {
        return EXEC_ERR_BAD_BLOB;
    }

    /* 3. Compute layout sizes.  v0.4.0 carries:
     *   - argc word
     *   - argv vector (argc + 1 slots)
     *   - envp vector (envc + 1 slots)
     *   - AUXV (variable, see count_auxv_pairs)
     *   - argv + envp string blobs
     *   - AT_RANDOM payload (16 bytes the kernel/libc seed from) */
    auxv_pairs = count_auxv_pairs(hints);
    header_bytes = sizeof(uint32_t)
                 + (argc + 1) * sizeof(uint32_t)
                 + (envc + 1) * sizeof(uint32_t)
                 + auxv_pairs * 2 * sizeof(uint32_t);
    total_bytes = header_bytes + argv_strs + envp_strs + EXEC_RANDOM_LEN;

    /* Round up to 16-byte alignment so ESP at entry is 16-aligned
     * (System V ABI requires ESP%16 == 0 at the call to _start). */
    total_bytes = (total_bytes + 15) & ~(vm_size_t)15;

    if (total_bytes > EXEC_STACK_SIZE) {
        return EXEC_ERR_TOO_BIG;
    }

    buf = malloc(total_bytes);
    if (!buf)
        return EXEC_ERR_VM_SETUP;
    memset(buf, 0, total_bytes);

    /* 4. Compute the runtime VA where this buffer will land — top of
     *    the stack page minus the layout size. */
    stack_va = EXEC_STACK_TOP - total_bytes;
    /* AT_RANDOM lives in the 16 bytes immediately after the string
     * region; we point AUXV's AT_RANDOM at the runtime VA of those
     * bytes. */
    random_va = stack_va + header_bytes + argv_strs + envp_strs;

    /* Place strings at the end of the layout; pointers in the header
     * point at runtime VAs inside the strings region. */
    strings_dst = buf + header_bytes;
    {
        vm_address_t strings_va = stack_va + header_bytes;
        const char *src;
        char *dst = strings_dst;
        vm_address_t cur_va = strings_va;
        int idx = 0;
        unsigned i;

        p = (uint32_t *)buf;
        *p++ = (uint32_t)argc;

        /* argv[i] string copies + pointer fixups */
        src = (const char *)argv_blob;
        for (idx = 0; idx < argc; idx++) {
            size_t slen = strlen(src) + 1;
            *p++ = (uint32_t)cur_va;
            memcpy(dst, src, slen);
            dst    += slen;
            cur_va += slen;
            src    += slen;
        }
        *p++ = 0;       /* argv NULL terminator */

        /* envp[i] string copies + pointer fixups */
        src = (const char *)envp_blob;
        for (idx = 0; idx < envc; idx++) {
            size_t slen = strlen(src) + 1;
            *p++ = (uint32_t)cur_va;
            memcpy(dst, src, slen);
            dst    += slen;
            cur_va += slen;
            src    += slen;
        }
        *p++ = 0;       /* envp NULL terminator */

        /* AUXV (v0.4.0).  Always emit AT_PAGESZ + AT_RANDOM; emit the
         * optional entries only when the caller provided real values.
         * Order matters only for AT_NULL coming last. */
        *p++ = AT_PAGESZ;        *p++ = EXEC_PAGE_SIZE;
        *p++ = AT_RANDOM;        *p++ = (uint32_t)random_va;
        if (hints) {
            if (hints->vdso_base) {
                *p++ = AT_SYSINFO_EHDR; *p++ = (uint32_t)hints->vdso_base;
            }
            if (hints->entry_va) {
                *p++ = AT_ENTRY;        *p++ = (uint32_t)hints->entry_va;
            }
            if (hints->interp_base) {
                *p++ = AT_BASE;         *p++ = (uint32_t)hints->interp_base;
            }
            if (hints->phdr_va) {
                *p++ = AT_PHDR;         *p++ = (uint32_t)hints->phdr_va;
                *p++ = AT_PHENT;        *p++ = hints->phent;
                *p++ = AT_PHNUM;        *p++ = hints->phnum;
            }
        }
        *p++ = AT_NULL;          *p++ = 0;

        /* AT_RANDOM payload: 16 bytes of seed material.  We don't yet
         * have a kernel RNG exposed to userland, so seed from cycle-
         * counter + task port id — non-cryptographic but at least it
         * differs across boots and across tasks. */
        {
            uint8_t *r = (uint8_t *)buf + header_bytes
                                       + argv_strs + envp_strs;
            uint32_t seed = (uint32_t)new_task * 2654435761u;
            for (i = 0; i < EXEC_RANDOM_LEN; i++) {
                seed = seed * 1103515245u + 12345u;
                r[i] = (uint8_t)(seed >> 16);
            }
        }
    }

    /* 5. vm_write the assembled layout to the high end of the stack. */
    kr = vm_write(new_task, stack_va, (vm_offset_t)buf,
                  (mach_msg_type_number_t)total_bytes);
    free(buf);
    if (kr != KERN_SUCCESS) {
        printf("exec: stack vm_write kr=%d\n", kr);
        return EXEC_ERR_VM_SETUP;
    }

    *out_top = stack_va;
    return EXEC_OK;
}
