/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * char_module_abi.h — Back-end module ABI for char_server.
 *
 * A back-end module is a userspace shared object loaded into the
 * char_server process via libdl from /mach_servers/modules/char/.
 * Each module exports `<basename>_module_ops` of type
 * `char_module_ops_t` — same convention block_device_server / hal_server
 * / gpu_server use.
 *
 * Bumping CHAR_MODULE_ABI_VERSION is a hard ABI break.  Adding new
 * function-pointer fields is done by appending to char_module_ops_t
 * and bumping the version; old modules with the smaller layout are
 * rejected by the loader.
 */

#ifndef _CHAR_CHAR_MODULE_ABI_H_
#define _CHAR_CHAR_MODULE_ABI_H_

#include <stdint.h>
#include <stddef.h>
#include <mach.h>
#include <char/char_types.h>

#define CHAR_MODULE_ABI_VERSION		1u

/* ============================================================
 * Forward decls — module instance state is opaque to char_server
 * core; the module returns a void * from probe and core hands it
 * back unchanged to every other op.
 * ============================================================ */

struct hal_device_info;		/* hal_server.h, optional probe hint */

/* ============================================================
 * Core → module entry points.  Function-pointer fields the module
 * does not implement may be NULL; char_server core treats a NULL
 * op as "KERN_FAILURE for this resource class" — same fallback
 * convention as gpu_module_ops_t.
 * ============================================================ */

typedef struct char_module_ops {
	/* Identification ------------------------------------------------ */
	const char	*name;		/* "ps2", "uart", ... */
	uint32_t	abi_version;	/* must == CHAR_MODULE_ABI_VERSION */
	uint32_t	priority;	/* higher = tried first */
	uint32_t	device_class;	/* CHAR_CLASS_* — affects cap_check */

	/* Probe — HAL hint may be NULL when the module claims a fixed
	 * legacy resource (ps2 unconditionally, uart by scratch-register
	 * test).  Return non-NULL on claim, NULL on skip. */
	void *		(*probe)(const struct hal_device_info *dev);

	/* Lifecycle ----------------------------------------------------- */
	int		(*attach)(void *priv);
	void		(*detach)(void *priv);

	/* Keyboard subscribe (CHAR_CLASS_KEYBOARD modules only).
	 * `notify_port` is a SEND right the module retains; every key
	 * event is sent to it as one Mach msg carrying char_kbd_event_t
	 * inline.  Send failures (full buffer, dead receiver) drop the
	 * event for that subscriber only, never block the IRQ thread. */
	int		(*kbd_subscribe)(void *priv, mach_port_t notify_port);

	/* TTY ops (CHAR_CLASS_TTY modules only). */
	int		(*tty_read)(void *priv, char *buf, size_t max,
				    size_t *out_len);
	int		(*tty_write)(void *priv, const char *buf, size_t len);
	int		(*tty_set_attr)(void *priv, uint32_t baud,
					uint32_t data_bits, uint32_t parity,
					uint32_t stop_bits);
} char_module_ops_t;

#define CHAR_MODULE_ENTRY_SUFFIX	"_module_ops"

#endif /* _CHAR_CHAR_MODULE_ABI_H_ */
