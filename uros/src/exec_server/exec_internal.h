/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef _EXEC_INTERNAL_H_
#define _EXEC_INTERNAL_H_

/*
 * exec_internal.h — declarations shared between exec_server.c,
 * exec_load.c, and exec_stack.c.  Not exported.
 */

#include "exec_types.h"
#include <mach/port.h>
#include <mach/mach_types.h>
#include <mach/message.h>

/*
 * Top-level orchestration: read binary, parse, create task, install
 * segments, build stack, start thread.  Returns EXEC_OK or one of
 * EXEC_ERR_*.  *out_task and *out_thread are set on success.
 */
int exec_do_load(mach_port_t client_task, const char *path,
                 const void *argv_blob, mach_msg_type_number_t argv_len,
                 const void *envp_blob, mach_msg_type_number_t envp_len,
                 mach_port_t *out_task, mach_port_t *out_thread);

/*
 * Build the System V ABI initial stack in `new_task` (allocates
 * EXEC_STACK_VA..EXEC_STACK_TOP, vm_writes the prepared layout).
 * On success *out_top holds the runtime ESP value.
 *
 * v0.4.0 additions (#236): the caller passes auxv hints so AUXV can
 * carry AT_ENTRY / AT_PHDR / AT_PHENT / AT_PHNUM / AT_SYSINFO_EHDR.
 * Any field set to 0 is omitted from the AUXV (except entry / vdso
 * which we always emit when non-zero — zero means "loader doesn't
 * know", not "value really is zero").
 */
struct exec_auxv_hints {
    vm_address_t entry_va;       /* AT_ENTRY,         0 = skip   */
    vm_address_t phdr_va;        /* AT_PHDR,          0 = skip   */
    uint32_t     phent;          /* AT_PHENT (size of one phdr)   */
    uint32_t     phnum;          /* AT_PHNUM (count)              */
    vm_address_t vdso_base;      /* AT_SYSINFO_EHDR,  0 = skip   */
    vm_address_t interp_base;    /* AT_BASE (dyn-linker load bias), 0 = skip */
};

int exec_build_stack(mach_port_t new_task,
                     const void *argv_blob, mach_msg_type_number_t argv_len,
                     const void *envp_blob, mach_msg_type_number_t envp_len,
                     const struct exec_auxv_hints *hints,
                     vm_address_t *out_top);

/*
 * v0.4.0 (#236): install the placeholder vDSO at EXEC_VDSO_VA in
 * `new_task` (read-only).  On success *out_base holds the runtime VA
 * AUXV's AT_SYSINFO_EHDR should advertise.
 */
int exec_install_vdso(mach_port_t new_task, vm_address_t *out_base);

#endif /* _EXEC_INTERNAL_H_ */
