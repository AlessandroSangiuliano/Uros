/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * char_server.h — Internal types and globals for the char_server task.
 *
 * Mirror gpu_server.h.  Public client-visible types live in
 * osfmk/export/include/char/.
 */

#ifndef _CHAR_SERVER_INTERNAL_H_
#define _CHAR_SERVER_INTERNAL_H_

#include <mach.h>
#include <mach/mach_types.h>
#include <stdint.h>
#include <stddef.h>
#include <char/char_types.h>
#include <char/char_module_abi.h>

#define CHAR_MAX_DEVICES	8
#define CHAR_MAX_MODULES	8

struct char_device_entry {
	int				in_use;
	char_dev_id_t			id;
	const char_module_ops_t		*module;
	void				*priv;
	struct char_device_info		info;
};

/* ============================================================
 * Core API
 * ============================================================ */

int  char_core_init(void);
void char_core_run_discovery(const char_module_ops_t * const *modules,
			     unsigned int n_modules,
			     mach_port_t hal_port);

struct char_device_entry *char_core_dev_lookup(char_dev_id_t id);
unsigned int              char_core_dev_count(void);
int                       char_core_dev_copy_all(struct char_device_info *out,
						 unsigned int max);

/*
 * cap_check — same shape as gpu_core_cap_check.  resource_id is the
 * dev_id (or 0 for class-wide ops); the required `op` is selected
 * by the MIG handler from CHAR_CAP_*.  Empty tokens (count == 0)
 * are rejected — char_server has no early-boot exception path
 * comparable to gpu_text_puts.
 */
int char_core_cap_check(const char *token, unsigned int token_count,
			uint64_t op, uint64_t resource_id);

/* ============================================================
 * MIG-side glue
 * ============================================================ */

extern boolean_t char_server_server(mach_msg_header_t *in,
				    mach_msg_header_t *out);

/* ============================================================
 * Global state (defined in main.c)
 * ============================================================ */

extern mach_port_t	char_host_port;
extern mach_port_t	char_device_port;
extern mach_port_t	char_security_port;
extern mach_port_t	char_root_ledger_wired;
extern mach_port_t	char_root_ledger_paged;
extern mach_port_t	char_service_port;

#endif /* _CHAR_SERVER_INTERNAL_H_ */
