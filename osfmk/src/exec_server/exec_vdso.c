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
#include <mach/mach_interface.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Minimal Elf32 header (no dependency on a system elf.h)             */
/* ------------------------------------------------------------------ */

#define EI_MAG0          0
#define EI_MAG1          1
#define EI_MAG2          2
#define EI_MAG3          3
#define EI_CLASS         4
#define EI_DATA          5
#define EI_VERSION       6
#define EI_OSABI         7
#define EI_NIDENT       16

#define ELFMAG0       0x7f
#define ELFMAG1       'E'
#define ELFMAG2       'L'
#define ELFMAG3       'F'
#define ELFCLASS32       1
#define ELFDATA2LSB      1
#define EV_CURRENT       1
#define ELFOSABI_NONE    0

#define ET_DYN           3
#define EM_386           3

/*
 * 52-byte Elf32_Ehdr laid out by hand — no struct alignment surprises
 * across compilers.  Fields not interesting to a consumer that only
 * needs to confirm "this is an ELF" are left at 0.
 */
static void
build_vdso_blob(uint8_t *page, vm_size_t page_size)
{
    memset(page, 0, page_size);

    /* e_ident */
    page[EI_MAG0]    = ELFMAG0;
    page[EI_MAG1]    = ELFMAG1;
    page[EI_MAG2]    = ELFMAG2;
    page[EI_MAG3]    = ELFMAG3;
    page[EI_CLASS]   = ELFCLASS32;
    page[EI_DATA]    = ELFDATA2LSB;
    page[EI_VERSION] = EV_CURRENT;
    page[EI_OSABI]   = ELFOSABI_NONE;
    /* e_ident[8..15] reserved -> zero */

    /* e_type = ET_DYN (uint16, little-endian) */
    page[16] = (uint8_t)ET_DYN;
    page[17] = (uint8_t)(ET_DYN >> 8);

    /* e_machine = EM_386 */
    page[18] = (uint8_t)EM_386;
    page[19] = (uint8_t)(EM_386 >> 8);

    /* e_version = EV_CURRENT (uint32) */
    page[20] = (uint8_t)EV_CURRENT;
    page[21] = 0;
    page[22] = 0;
    page[23] = 0;

    /* e_entry / e_phoff / e_shoff / e_flags stay 0 — no segments
     * exported in v0.4.0.  e_ehsize = 52 (sizeof Elf32_Ehdr). */
    page[40] = 52;
    page[41] = 0;
    /* e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx all 0. */
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
