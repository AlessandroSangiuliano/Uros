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

#include <libelf.h>
#include <libvfs.h>
#include <mach/elf.h>           /* PT_LOAD, PF_*, ET_EXEC */
#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_interface.h>
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
 * For each PT_LOAD: vm_allocate at the requested vaddr, vm_write the
 * file content, vm_protect to the segment's RWX flags.  v0.1.0 only
 * supports static ET_EXEC (no load bias) — ET_DYN/PIE arrives with
 * exec_server v0.2.0 alongside PT_INTERP.
 */
static int
install_segments(mach_port_t new_task, const elf_image_t *img)
{
    int n = elf_phdr_count(img);
    int i;
    kern_return_t kr;

    for (i = 0; i < n; i++) {
        elf_phdr_view_t ph;
        vm_address_t addr;
        vm_prot_t prot;

        if (elf_phdr_get(img, i, &ph) != ELF_OK)
            continue;
        if (ph.type != PT_LOAD || ph.memsz == 0)
            continue;

        /* Round to page boundaries for the kernel allocator. */
        addr = (vm_address_t)(ph.vaddr & ~(vm_address_t)0xFFFu);
        {
            vm_size_t size = (vm_size_t)((ph.vaddr + ph.memsz + 0xFFFu)
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
            kr = vm_write(new_task, (vm_address_t)ph.vaddr,
                          (vm_offset_t)ph.data,
                          (mach_msg_type_number_t)ph.filesz);
            if (kr != KERN_SUCCESS) {
                printf("exec: vm_write(0x%x, %u) failed kr=%d\n",
                       (unsigned)ph.vaddr, (unsigned)ph.filesz, kr);
                return EXEC_ERR_VM_SETUP;
            }
        }

        prot = VM_PROT_NONE;
        if (ph.flags & PF_R) prot |= VM_PROT_READ;
        if (ph.flags & PF_W) prot |= VM_PROT_WRITE;
        if (ph.flags & PF_X) prot |= VM_PROT_EXECUTE;

        kr = vm_protect(new_task, addr,
                        (vm_size_t)((ph.vaddr + ph.memsz + 0xFFFu)
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

    kr = thread_resume(th);
    if (kr != KERN_SUCCESS) {
        printf("exec: thread_resume kr=%d\n", kr);
        return EXEC_ERR_THREAD_CREATE;
    }

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
    if (elf_type(&img) != ET_EXEC) {
        /* v0.1.0: only static fully-linked executables (ET_EXEC).
         * ET_DYN + PT_INTERP is exec_server v0.2.0 (#234). */
        elf_close(&img);
        free(file_buf);
        return EXEC_ERR_NOT_STATIC;
    }
    if (elf_interp(&img) != NULL) {
        elf_close(&img);
        free(file_buf);
        return EXEC_ERR_NOT_STATIC;
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

    /* 4. Map every PT_LOAD into the child. */
    rc = install_segments(new_task, &img);
    if (rc != EXEC_OK)
        return fail_after_task(new_task, &img, file_buf, rc);

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
        hints.entry_va  = (vm_address_t)elf_entry(&img);
        hints.vdso_base = vdso_base;

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
                        (ph.vaddr + (phoff - ph.offset));
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

    /* 6. Bring up the entry thread. */
    rc = start_thread(new_task, elf_entry(&img), stack_top, &new_thread);
    if (rc != EXEC_OK)
        return fail_after_task(new_task, &img, file_buf, rc);

    elf_close(&img);
    free(file_buf);

    *out_task   = new_task;
    *out_thread = new_thread;
    return EXEC_OK;
}
