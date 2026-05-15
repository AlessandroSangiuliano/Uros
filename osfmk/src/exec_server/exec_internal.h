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
 */
int exec_build_stack(mach_port_t new_task,
                     const void *argv_blob, mach_msg_type_number_t argv_len,
                     const void *envp_blob, mach_msg_type_number_t envp_len,
                     vm_address_t *out_top);

#endif /* _EXEC_INTERNAL_H_ */
