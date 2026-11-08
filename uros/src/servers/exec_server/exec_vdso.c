/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * exec_vdso.c — install the vDSO placeholder page in a new task
 * (exec_server v0.4.0 / #236).
 *
 * v0.4.0 ships only the *page* + AUXV entry: a 4 KiB read-only blob
 * whose first bytes form a valid Elf32 ELF header.  No exported
 * symbols, no fast-path syscalls.  This is the minimum surface
 * libposix-uros needs to discover support without runtime branching;
 * actual fast paths (clock_gettime, getpid, signal trampoline) drop
 * in incrementally without changing the wire / VA contract.
 *
 * The ELF header is built from scratch every boot — no host-side
 * vdso.so blob to ship, no objcopy magic, no linker script.  Costs
 * us ~50 bytes of code in exchange for never tripping on a
 * cross-tool mismatch between the host that built the blob and the
 * target that consumes it.
 */

#include "exec_types.h"
#include "exec_internal.h"

#include <mach.h>
#include <mach/elf.h>
#include <mach/mach_interface.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  A minimal ELF image of this target                                 */
/* ------------------------------------------------------------------ */

/*
 * What musl's dynamic linker reads of an AT_SYSINFO_EHDR: the program headers,
 * and from them PT_LOAD for the base and PT_DYNAMIC for a dynamic table, which
 * it walks without asking whether there is one.  So the placeholder has one,
 * with nothing in it.  It had no program headers, ld-musl read a table at
 * address 0, and that went unnoticed for as long as page zero was mapped in an
 * exec'd task, which it was (#421).
 *
 * <mach/elf.h> names this target's pair, so the widths and the field order are
 * the compiler's to get right.
 */
struct vdso_image {
    Elf_Ehdr ehdr;
    Elf_Phdr phdr[2];
    Elf_Dyn  dyn[1];
};

_Static_assert(sizeof(struct vdso_image) <= EXEC_VDSO_SIZE,
               "the placeholder vDSO does not fit its page");

static void
build_vdso_blob(uint8_t *page, vm_size_t page_size)
{
    struct vdso_image *v = (struct vdso_image *)page;

    memset(page, 0, page_size);

    v->ehdr.e_ident[EI_MAG0]    = ELFMAG0;
    v->ehdr.e_ident[EI_MAG1]    = ELFMAG1;
    v->ehdr.e_ident[EI_MAG2]    = ELFMAG2;
    v->ehdr.e_ident[EI_MAG3]    = ELFMAG3;
    v->ehdr.e_ident[EI_CLASS]   = ELF_TARGET_CLASS;
    v->ehdr.e_ident[EI_DATA]    = ELFDATA2LSB;
    v->ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    v->ehdr.e_type      = ET_DYN;
    v->ehdr.e_machine   = ELF_TARGET_MACHINE;
    v->ehdr.e_version   = EV_CURRENT;
    v->ehdr.e_phoff     = offsetof(struct vdso_image, phdr);
    v->ehdr.e_ehsize    = sizeof(v->ehdr);
    v->ehdr.e_phentsize = sizeof(v->phdr[0]);
    v->ehdr.e_phnum     = 2;

    v->phdr[0].p_type   = PT_LOAD;
    v->phdr[0].p_flags  = PF_R | PF_X;
    v->phdr[0].p_filesz = EXEC_VDSO_SIZE;
    v->phdr[0].p_memsz  = EXEC_VDSO_SIZE;
    v->phdr[0].p_align  = EXEC_VDSO_SIZE;

    v->phdr[1].p_type   = PT_DYNAMIC;
    v->phdr[1].p_flags  = PF_R;
    v->phdr[1].p_offset = offsetof(struct vdso_image, dyn);
    v->phdr[1].p_vaddr  = offsetof(struct vdso_image, dyn);
    v->phdr[1].p_filesz = sizeof(v->dyn);
    v->phdr[1].p_memsz  = sizeof(v->dyn);
    v->phdr[1].p_align  = sizeof(void *);

    /* dyn[0] is DT_NULL, by the memset. */
}

/* ------------------------------------------------------------------ */

/*
 * Install the placeholder vDSO at EXEC_VDSO_VA in `new_task`.
 * Returns EXEC_OK on success, EXEC_ERR_VM_SETUP on any underlying
 * kernel failure.  On success *out_base holds the runtime VA the
 * AUXV's AT_SYSINFO_EHDR will point at.
 */
int
exec_install_vdso(mach_port_t new_task, vm_address_t *out_base)
{
    vm_address_t  va = EXEC_VDSO_VA;
    uint8_t       blob[EXEC_VDSO_SIZE];
    kern_return_t kr;

    kr = vm_allocate(new_task, &va, EXEC_VDSO_SIZE, FALSE);
    if (kr != KERN_SUCCESS) {
        printf("exec: vdso vm_allocate kr=%d\n", kr);
        return EXEC_ERR_VM_SETUP;
    }

    build_vdso_blob(blob, sizeof(blob));

    kr = vm_write(new_task, va, (vm_offset_t)blob,
                  (mach_msg_type_number_t)sizeof(blob));
    if (kr != KERN_SUCCESS) {
        printf("exec: vdso vm_write kr=%d\n", kr);
        return EXEC_ERR_VM_SETUP;
    }

    /* Read-only — userspace must not be able to mutate the vDSO.  The
     * max-protection also drops to RX so a malicious mprotect can't
     * re-acquire write. */
    kr = vm_protect(new_task, va, EXEC_VDSO_SIZE, FALSE,
                    VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) {
        printf("exec: vdso vm_protect cur kr=%d\n", kr);
        return EXEC_ERR_VM_SETUP;
    }
    kr = vm_protect(new_task, va, EXEC_VDSO_SIZE, TRUE,
                    VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) {
        printf("exec: vdso vm_protect max kr=%d\n", kr);
        return EXEC_ERR_VM_SETUP;
    }

    *out_base = va;
    return EXEC_OK;
}
