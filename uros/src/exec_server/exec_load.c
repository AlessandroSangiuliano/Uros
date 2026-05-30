/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * exec_load.c — exec_server v0.1.0 core: read a static ELF binary
 * via libvfs, parse it via libelf, create a fresh task, lay out
 * its segments, build the initial stack, start the entry thread.
 *
 * Original Uros code, see feedback-study-never-copy.  No derivation
 * from Linux do_execve / FreeBSD do_execve / etc. — flow is dictated
 * by the System V ABI for argv/envp/auxv on the i386 stack and by
 * the OSF Mach task/thread API.
 */

#include "exec_types.h"
#include "exec_internal.h"

#include <libcap.h>             /* cap_provision (#235) */
#include <libelf.h>
#include <libvfs.h>
#include <mach/elf.h>           /* PT_LOAD, PF_*, ET_EXEC */
#include <mach.h>
#include <mach/cap_manifest.h>  /* TLV format (#235) */
#include <mach/mach_traps.h>
#include <mach/mach_interface.h>
#include <mach/task_special_ports.h>  /* TASK_CAP_PORT (#235) */
#include <mach/i386/thread_status.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/*  File slurp via libvfs                                              */
/* ------------------------------------------------------------------ */

/*
 * Read the whole binary into a freshly malloc'd buffer.  Returns 0 on
 * success or one of EXEC_ERR_*.  Caller frees *out on success.
 */
static int
slurp_binary(const char *path, void **out, vm_size_t *out_size)
{
    vfs_fd_t fd;
    vfs_stat_t st;
    void *buf;
    ssize_t got;
    vm_size_t total = 0;

    *out = NULL;
    *out_size = 0;

    fd = vfs_open(path, VFS_O_RDONLY, 0);
    if (fd == VFS_FD_INVALID)
        return EXEC_ERR_NOT_FOUND;

    if (vfs_fstat(fd, &st) != 0) {
        vfs_close(fd);
        return EXEC_ERR_READ;
    }

    if (st.st_size == 0 || st.st_size > 16 * 1024 * 1024) {
        /* v0.1.0: refuse empty or insanely-large images.  Bumps with
         * mmap-backed loading once #228 v0.5.0+ lands. */
        vfs_close(fd);
        return EXEC_ERR_TOO_BIG;
    }

    buf = malloc((size_t)st.st_size);
    if (!buf) {
        vfs_close(fd);
        return EXEC_ERR_VM_SETUP;
    }

    /* Loop in case the fs_server returns a short read (libvfs leaves
     * the per-fd offset pointing at the next byte for us). */
    while (total < st.st_size) {
        got = vfs_read(fd, (char *)buf + total,
                       (size_t)(st.st_size - total));
        if (got < 0) {
            free(buf);
            vfs_close(fd);
            return EXEC_ERR_READ;
        }
        if (got == 0)
            break;     /* EOF before declared size — accept it */
        total += (vm_size_t)got;
    }

    vfs_close(fd);
    *out = buf;
    *out_size = total;
    return EXEC_OK;
}

/* ------------------------------------------------------------------ */
/*  Segment installation in the new task                               */
/* ------------------------------------------------------------------ */

/*
 * Load bias bases (#234).  ET_EXEC loads at bias 0 (its p_vaddr are
 * absolute).  A PT_INTERP dynamic linker is ET_DYN and gets relocated
 * to EXEC_INTERP_BASE; an ET_DYN/PIE main image (no fixed link address)
 * gets EXEC_DYN_BASE.  Both regions sit below the stack/vDSO window and
 * above the typical 0x08048000 ET_EXEC text, so they don't collide.
 */
#define EXEC_INTERP_BASE  0x40000000U
#define EXEC_DYN_BASE     0x56555000U

/*
 * For each PT_LOAD: vm_allocate at (vaddr + bias), vm_write the file
 * content, vm_protect to the segment's RWX flags.  `bias` is 0 for a
 * fixed-address ET_EXEC and the chosen load base for an ET_DYN image
 * (the interpreter, or a PIE main image).  exec_server does not apply
 * relocations — for dynamic images that is the interpreter's job.
 */
static int
install_segments(mach_port_t new_task, const elf_image_t *img,
                 vm_address_t bias)
{
    int n = elf_phdr_count(img);
    int i;
    kern_return_t kr;

    for (i = 0; i < n; i++) {
        elf_phdr_view_t ph;
        vm_address_t addr;
        vm_address_t seg_va;
        vm_prot_t prot;

        if (elf_phdr_get(img, i, &ph) != ELF_OK)
            continue;
        if (ph.type != PT_LOAD || ph.memsz == 0)
            continue;

        seg_va = (vm_address_t)ph.vaddr + bias;

        /* Round to page boundaries for the kernel allocator. */
        addr = seg_va & ~(vm_address_t)0xFFFu;
        {
            vm_size_t size = (vm_size_t)((seg_va + ph.memsz + 0xFFFu)
                                         & ~(vm_address_t)0xFFFu) - addr;
            kr = vm_allocate(new_task, &addr, size, FALSE);
            if (kr != KERN_SUCCESS) {
                printf("exec: vm_allocate(0x%x, 0x%x) failed kr=%d\n",
                       (unsigned)addr, (unsigned)size, kr);
                return EXEC_ERR_VM_SETUP;
            }
        }

        /* Copy the file portion of the segment.  The tail
         * memsz - filesz is already zero from vm_allocate. */
        if (ph.filesz > 0 && ph.data) {
            kr = vm_write(new_task, seg_va,
                          (vm_offset_t)ph.data,
                          (mach_msg_type_number_t)ph.filesz);
            if (kr != KERN_SUCCESS) {
                printf("exec: vm_write(0x%x, %u) failed kr=%d\n",
                       (unsigned)seg_va, (unsigned)ph.filesz, kr);
                return EXEC_ERR_VM_SETUP;
            }
        }

        prot = VM_PROT_NONE;
        if (ph.flags & PF_R) prot |= VM_PROT_READ;
        if (ph.flags & PF_W) prot |= VM_PROT_WRITE;
        if (ph.flags & PF_X) prot |= VM_PROT_EXECUTE;

        kr = vm_protect(new_task, addr,
                        (vm_size_t)((seg_va + ph.memsz + 0xFFFu)
                                    & ~(vm_address_t)0xFFFu) - addr,
                        FALSE, prot);
        if (kr != KERN_SUCCESS) {
            printf("exec: vm_protect(0x%x) failed kr=%d\n",
                   (unsigned)addr, kr);
            return EXEC_ERR_VM_SETUP;
        }
    }
    return EXEC_OK;
}

/*
 * Load a PT_INTERP dynamic linker (#234).  Reads the interpreter image
 * named by `interp_path`, parses it, and maps its PT_LOADs at
 * EXEC_INTERP_BASE.  On success *out_entry is the interpreter's runtime
 * entry point (base + e_entry), where control is handed off instead of
 * the application's e_entry.
 */
static int
load_interp(mach_port_t new_task, const char *interp_path,
            vm_address_t *out_base, uintptr_t *out_entry)
{
    void *ibuf = NULL;
    vm_size_t isz = 0;
    elf_image_t iimg;
    int rc;

    rc = slurp_binary(interp_path, &ibuf, &isz);
    if (rc != EXEC_OK) {
        printf("exec: cannot read interpreter \"%s\" rc=%d\n",
               interp_path, rc);
        return rc;
    }
    if (elf_open(ibuf, (size_t)isz, &iimg) != ELF_OK) {
        free(ibuf);
        return EXEC_ERR_PARSE;
    }
    /* The interpreter must itself be statically self-contained
     * (ET_DYN with no further PT_INTERP). */
    if (elf_type(&iimg) != ET_DYN || elf_interp(&iimg) != NULL) {
        elf_close(&iimg);
        free(ibuf);
        printf("exec: interpreter \"%s\" is not a bare ET_DYN\n",
               interp_path);
        return EXEC_ERR_PARSE;
    }

    rc = install_segments(new_task, &iimg, EXEC_INTERP_BASE);
    if (rc == EXEC_OK) {
        *out_base  = EXEC_INTERP_BASE;
        *out_entry = (uintptr_t)(EXEC_INTERP_BASE + elf_entry(&iimg));
    }

    elf_close(&iimg);
    free(ibuf);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Initial thread bring-up                                            */
/* ------------------------------------------------------------------ */

static int
start_thread(mach_port_t new_task, uintptr_t entry, vm_address_t stack_top,
             mach_port_t *out_thread)
{
    mach_port_t th;
    struct i386_thread_state regs;
    unsigned int reg_count = i386_THREAD_STATE_COUNT;
    kern_return_t kr;

    kr = thread_create(new_task, &th);
    if (kr != KERN_SUCCESS) {
        printf("exec: thread_create kr=%d\n", kr);
        return EXEC_ERR_THREAD_CREATE;
    }

    /* Pull the default state from the kernel so segment selectors
     * and EFLAGS are the right user-mode defaults; we only override
     * EIP and ESP. */
    kr = thread_get_state(th, i386_THREAD_STATE,
                          (thread_state_t)&regs, &reg_count);
    if (kr != KERN_SUCCESS) {
        printf("exec: thread_get_state kr=%d\n", kr);
        return EXEC_ERR_THREAD_CREATE;
    }

    regs.eip  = (unsigned int)entry;
    regs.uesp = (unsigned int)stack_top;

    kr = thread_set_state(th, i386_THREAD_STATE,
                          (thread_state_t)&regs, reg_count);
    if (kr != KERN_SUCCESS) {
        printf("exec: thread_set_state kr=%d\n", kr);
        return EXEC_ERR_THREAD_CREATE;
    }

    /*
     * Leave the thread SUSPENDED (#262 step 3).  thread_create starts
     * it suspended; we no longer resume here.  The caller (libposix-uros
     * execve/spawn) hands over any inherited fds into the new task and
     * then issues thread_resume itself, so the new image's first user
     * instruction sees a fully populated fd table.
     */
    *out_thread = th;
    return EXEC_OK;
}

/* ------------------------------------------------------------------ */
/*  Top-level: orchestrate a full exec                                  */
/* ------------------------------------------------------------------ */

static int
fail_after_task(mach_port_t new_task, elf_image_t *img, void *file_buf, int rc)
{
    (void)task_terminate(new_task);
    elf_close(img);
    free(file_buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Manifest provisioning (#235)                                       */
/* ------------------------------------------------------------------ */

/*
 * Empty-default manifest used as fallback when the binary has no
 * embedded .uros_manifest section.  Hand-laid TLV blob — no need to
 * call mkmanifest at runtime.  Layout:
 *
 *   - 48-byte cap_manifest_header_t
 *   - 0 required entries
 *   - 0 delegatable entries
 *   - 12-byte name region "(noname)\0" padded to 4-byte align
 *
 * Total 64 bytes.  All offsets / counts validated by cap_server's
 * cap_manifest_validate so this must round-trip.
 */
#define DEFAULT_NAME      "(noname)"
#define DEFAULT_NAME_LEN  9            /* including the NUL */

static const struct {
    cap_manifest_header_t hdr;
    char                  name[12];    /* padded to multiple of 4 */
} default_manifest_blob = {
    .hdr = {
        .magic              = CAP_MANIFEST_MAGIC,
        .version            = CAP_MANIFEST_VERSION,
        .header_size        = (uint16_t)sizeof(cap_manifest_header_t),
        .total_size         = sizeof(cap_manifest_header_t) + 12u,
        .flags              = 0,
        .name_offset        = sizeof(cap_manifest_header_t),
        .name_length        = DEFAULT_NAME_LEN,
        .required_offset    = sizeof(cap_manifest_header_t),
        .required_count     = 0,
        .delegatable_offset = sizeof(cap_manifest_header_t),
        .delegatable_count  = 0,
        ._reserved          = { 0, 0 },
    },
    .name = DEFAULT_NAME,
};

/*
 * Look up the .uros_manifest ELF section.  When present, returns a
 * pointer + length into the file buffer (no copy).  When absent,
 * points to the static empty-default blob above.  Either way the
 * returned bytes live for the duration of exec_do_load.
 */
static void
extract_manifest(const elf_image_t *img,
                 const void **out_blob, uint32_t *out_len)
{
    elf_shdr_view_t sh;
    if (elf_shdr_find(img, ".uros_manifest", &sh) == ELF_OK &&
        sh.data != NULL &&
        sh.size >= sizeof(cap_manifest_header_t) &&
        sh.size <= CAP_MANIFEST_MAX_BYTES) {
        *out_blob = sh.data;
        *out_len  = (uint32_t)sh.size;
        return;
    }
    *out_blob = &default_manifest_blob;
    *out_len  = (uint32_t)sizeof(default_manifest_blob);
}

/*
 * Provision a per-task cap_port for `new_task` from the manifest
 * embedded in `img` (or the empty default).  Best-effort: failure
 * here is logged but non-fatal — the spawned task boots without a
 * TASK_CAP_PORT and falls through to the legacy permissive
 * well-known cap_server path, same behaviour as exec_server before
 * #235.  Returns 1 on a successful install, 0 otherwise.
 */
static int
provision_cap_port(mach_port_t new_task, const elf_image_t *img,
                   const char *path)
{
    const void   *blob;
    uint32_t      blen;
    mach_port_t   cap_port = MACH_PORT_NULL;
    kern_return_t kr;

    extract_manifest(img, &blob, &blen);

    kr = cap_provision(new_task, blob, blen, &cap_port);
    if (kr != KERN_SUCCESS || cap_port == MACH_PORT_NULL) {
        printf("exec: cap_provision(%s) failed kr=%d — legacy path\n",
               path ? path : "(?)", kr);
        return 0;
    }

    kr = task_set_special_port(new_task, TASK_CAP_PORT, cap_port);
    if (kr != KERN_SUCCESS) {
        printf("exec: task_set_special_port(TASK_CAP_PORT) failed kr=%d\n",
               kr);
        (void)mach_port_deallocate(mach_task_self(), cap_port);
        return 0;
    }

    printf("exec: %s -> TASK_CAP_PORT=0x%x (manifest %u bytes%s)\n",
           path ? path : "(?)", (unsigned)cap_port, blen,
           (blob == &default_manifest_blob) ? ", default" : "");
    return 1;
}

int
exec_do_load(mach_port_t client_task, const char *path,
             const void *argv_blob, mach_msg_type_number_t argv_len,
             const void *envp_blob, mach_msg_type_number_t envp_len,
             mach_port_t *out_task, mach_port_t *out_thread)
{
    void *file_buf = NULL;
    vm_size_t file_sz = 0;
    elf_image_t img;
    mach_port_t new_task = MACH_PORT_NULL;
    mach_port_t new_thread = MACH_PORT_NULL;
    vm_address_t stack_top;
    vm_address_t main_bias = 0;     /* ET_DYN load bias, 0 for ET_EXEC */
    vm_address_t interp_base = 0;   /* AT_BASE, 0 if no PT_INTERP */
    uintptr_t    start_entry;       /* where the first thread begins */
    const char  *interp;
    int rc;
    kern_return_t kr;

    *out_task   = MACH_PORT_NULL;
    *out_thread = MACH_PORT_NULL;

    /* 1. Read the binary. */
    rc = slurp_binary(path, &file_buf, &file_sz);
    if (rc != EXEC_OK)
        return rc;

    /* 2. Parse with libelf. */
    if (elf_open(file_buf, (size_t)file_sz, &img) != ELF_OK) {
        free(file_buf);
        return EXEC_ERR_PARSE;
    }
    /* v0.2.0 (#234): accept ET_EXEC (bias 0) and ET_DYN/PIE (loaded at
     * EXEC_DYN_BASE).  A PT_INTERP, if present, names a dynamic linker
     * we load below and hand control to instead of the app entry. */
    {
        uint32_t et = elf_type(&img);
        if (et != ET_EXEC && et != ET_DYN) {
            elf_close(&img);
            free(file_buf);
            return EXEC_ERR_NOT_STATIC;
        }
    }

    /* 3. Create the child task — IPC space inherited from the
     *    requester (caps copy through), VM is fresh. */
    kr = task_create(client_task, (ledger_port_array_t)0, 0,
                     FALSE, &new_task);
    if (kr != KERN_SUCCESS) {
        elf_close(&img);
        free(file_buf);
        return EXEC_ERR_TASK_CREATE;
    }

    /* ET_DYN/PIE images load at a bias; fixed ET_EXEC at 0. */
    main_bias = (elf_type(&img) == ET_DYN) ? EXEC_DYN_BASE : 0;

    /* 4. Map every PT_LOAD into the child. */
    rc = install_segments(new_task, &img, main_bias);
    if (rc != EXEC_OK)
        return fail_after_task(new_task, &img, file_buf, rc);

    /* 4.1. If the image names a dynamic linker (PT_INTERP), load it and
     *      hand control to its entry instead of the app's e_entry; the
     *      interpreter relocates the app and chains to AT_ENTRY (#234). */
    interp = elf_interp(&img);
    if (interp != NULL) {
        rc = load_interp(new_task, interp, &interp_base, &start_entry);
        if (rc != EXEC_OK)
            return fail_after_task(new_task, &img, file_buf, rc);
    } else {
        start_entry = (uintptr_t)(elf_entry(&img) + main_bias);
    }

    /* 4.25. Provision the child's per-task cap_port from the binary's
     *       .uros_manifest section (#235).  Done before vDSO + stack
     *       so libmach init reads TASK_CAP_PORT on the very first
     *       instruction.  Best-effort — see provision_cap_port. */
    (void)provision_cap_port(new_task, &img, path);

    /* 4.5. Install the vDSO placeholder (v0.4.0 / #236).  Failure
     *      here is non-fatal — the binary can still run, libposix
     *      will just take the slow path on every syscall. */
    {
        vm_address_t vdso_base = 0;
        struct exec_auxv_hints hints;
        int vrc = exec_install_vdso(new_task, &vdso_base);
        if (vrc != EXEC_OK) {
            printf("exec: vdso install failed rc=%d (non-fatal)\n", vrc);
            vdso_base = 0;
        }

        memset(&hints, 0, sizeof(hints));
        /* AT_ENTRY is always the application's entry (with bias), even
         * when control first goes to the interpreter — the interpreter
         * reads AT_ENTRY to chain into the app after relocations. */
        hints.entry_va    = (vm_address_t)(elf_entry(&img) + main_bias);
        hints.vdso_base   = vdso_base;
        hints.interp_base = interp_base;   /* AT_BASE, 0 if static */

        /* AT_PHDR derivation (#248): the phdr table lives at file
         * offset e_phoff; for ET_EXEC the program headers are mapped
         * as part of whichever PT_LOAD whose file range covers that
         * offset.  Compute the runtime VA the same way Linux's
         * fs/binfmt_elf.c does, by scanning the PT_LOADs.  If no
         * segment claims the phdr range we just skip the hint —
         * static binaries without a self-referencing PT_LOAD don't
         * need AT_PHDR. */
        {
            uint64_t phoff = elf_phoff(&img);
            int n_ph = elf_phdr_count(&img);
            int i;
            for (i = 0; i < n_ph; i++) {
                elf_phdr_view_t ph;
                if (elf_phdr_get(&img, i, &ph) != ELF_OK)
                    continue;
                if (ph.type != PT_LOAD)
                    continue;
                if (phoff >= ph.offset &&
                    phoff <  ph.offset + ph.filesz) {
                    hints.phdr_va = (vm_address_t)
                        (ph.vaddr + (phoff - ph.offset) + main_bias);
                    hints.phent   = elf_phentsize(&img);
                    hints.phnum   = (uint32_t)n_ph;
                    break;
                }
            }
        }

        /* 5. Set up the initial stack with argv/envp/auxv. */
        rc = exec_build_stack(new_task, argv_blob, argv_len,
                              envp_blob, envp_len, &hints, &stack_top);
    }
    if (rc != EXEC_OK)
        return fail_after_task(new_task, &img, file_buf, rc);

    /* 6. Bring up the entry thread — at the interpreter's entry when a
     *    PT_INTERP was present, otherwise at the app's (biased) entry. */
    rc = start_thread(new_task, start_entry, stack_top, &new_thread);
    if (rc != EXEC_OK)
        return fail_after_task(new_task, &img, file_buf, rc);

    elf_close(&img);
    free(file_buf);

    *out_task   = new_task;
    *out_thread = new_thread;
    return EXEC_OK;
}
