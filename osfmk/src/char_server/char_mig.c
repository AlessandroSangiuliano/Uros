/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * char_server/char_mig.c — server-side handlers for char_server.defs.
 *
 * #205 ships skeleton dispatch:
 *   - query_devices and device_open are functional.
 *   - kbd_subscribe / tty_read / tty_write / tty_set_attr return
 *     KERN_FAILURE if no module backs the dev_id, otherwise route
 *     to the module's op (if NULL → KERN_FAILURE).
 *
 * #206 (ps2.so) and #207 (uart.so) provide the back-end bodies; this
 * file does not change.
 */

#include <mach.h>
#include <mach/kern_return.h>
#include <stdio.h>
#include <string.h>
#include <mach/char_server_types.h>	/* CHR_BUF_MAX */
#include "char_server.h"

/* Pick the right cap op for a given device class. */
static uint64_t
class_to_open_op(uint32_t cls)
{
	switch (cls) {
	case CHAR_CLASS_KEYBOARD:	return CHAR_CAP_KBD_READ;
	case CHAR_CLASS_TTY:		return CHAR_CAP_TTY_RW;
	default:			return CHAR_CAP_DEV_ADMIN;
	}
}

/* ============================================================
 * Device discovery / open
 * ============================================================ */

kern_return_t
char_query_devices(mach_port_t char_port,
		   vm_offset_t *devices, mach_msg_type_number_t *devices_count,
		   uint32_t *n_devices)
{
	unsigned int n = char_core_dev_count();
	vm_size_t bytes;
	vm_offset_t buf;
	kern_return_t kr;

	(void)char_port;

	bytes = (vm_size_t)n * sizeof(struct char_device_info);
	if (bytes == 0) {
		*devices = 0;
		*devices_count = 0;
		*n_devices = 0;
		return KERN_SUCCESS;
	}

	buf = 0;
	kr = vm_allocate(mach_task_self(), &buf, bytes, TRUE);
	if (kr != KERN_SUCCESS)
		return kr;

	(void)char_core_dev_copy_all((struct char_device_info *)buf, n);

	*devices = buf;
	*devices_count = (mach_msg_type_number_t)bytes;
	*n_devices = (uint32_t)n;
	return KERN_SUCCESS;
}

kern_return_t
char_device_open(mach_port_t char_port,
		 char *cap, mach_msg_type_number_t cap_count,
		 uint32_t dev_id)
{
	struct char_device_entry *dev;

	(void)char_port;

	dev = char_core_dev_lookup((char_dev_id_t)dev_id);
	if (dev == NULL)
		return KERN_INVALID_ARGUMENT;

	if (char_core_cap_check(cap, cap_count,
				class_to_open_op(dev->info.class),
				(uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;

	/* No per-client state held in #205 — handle table arrives with
	 * the char_server-equivalent of #202 once subscribers / TTY
	 * sessions accumulate enough to need cap_revoke teardown. */
	return KERN_SUCCESS;
}

/* ============================================================
 * Keyboard plane
 * ============================================================ */

kern_return_t
char_kbd_subscribe(mach_port_t char_port,
		   char *cap, mach_msg_type_number_t cap_count,
		   uint32_t dev_id, mach_port_t notify_port)
{
	struct char_device_entry *dev;

	(void)char_port;

	dev = char_core_dev_lookup((char_dev_id_t)dev_id);
	if (dev == NULL)
		return KERN_INVALID_ARGUMENT;
	if (dev->info.class != CHAR_CLASS_KEYBOARD)
		return KERN_INVALID_ARGUMENT;
	if (char_core_cap_check(cap, cap_count, CHAR_CAP_KBD_READ,
				(uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;

	if (dev->module->kbd_subscribe == NULL)
		return KERN_FAILURE;
	return (dev->module->kbd_subscribe(dev->priv, notify_port) == 0)
		? KERN_SUCCESS : KERN_FAILURE;
}

/* ============================================================
 * TTY plane
 * ============================================================ */

kern_return_t
char_tty_read(mach_port_t char_port,
	      char *cap, mach_msg_type_number_t cap_count,
	      uint32_t dev_id, uint32_t max,
	      char *buf, mach_msg_type_number_t *buf_count)
{
	struct char_device_entry *dev;
	size_t got = 0;

	(void)char_port;

	dev = char_core_dev_lookup((char_dev_id_t)dev_id);
	if (dev == NULL)
		return KERN_INVALID_ARGUMENT;
	if (dev->info.class != CHAR_CLASS_TTY)
		return KERN_INVALID_ARGUMENT;
	if (char_core_cap_check(cap, cap_count, CHAR_CAP_TTY_RW,
				(uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;

	if (dev->module->tty_read == NULL) {
		*buf_count = 0;
		return KERN_FAILURE;
	}

	if (max > CHR_BUF_MAX)
		max = CHR_BUF_MAX;

	if (dev->module->tty_read(dev->priv, buf, (size_t)max, &got) != 0) {
		*buf_count = 0;
		return KERN_FAILURE;
	}
	*buf_count = (mach_msg_type_number_t)got;
	return KERN_SUCCESS;
}

kern_return_t
char_tty_write(mach_port_t char_port,
	       char *cap, mach_msg_type_number_t cap_count,
	       uint32_t dev_id,
	       char *buf, mach_msg_type_number_t buf_count)
{
	struct char_device_entry *dev;

	(void)char_port;

	dev = char_core_dev_lookup((char_dev_id_t)dev_id);
	if (dev == NULL)
		return KERN_INVALID_ARGUMENT;
	if (dev->info.class != CHAR_CLASS_TTY)
		return KERN_INVALID_ARGUMENT;
	if (char_core_cap_check(cap, cap_count, CHAR_CAP_TTY_RW,
				(uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;

	if (dev->module->tty_write == NULL)
		return KERN_FAILURE;
	(void)dev->module->tty_write(dev->priv, buf, (size_t)buf_count);
	return KERN_SUCCESS;
}

kern_return_t
char_tty_set_attr(mach_port_t char_port,
		  char *cap, mach_msg_type_number_t cap_count,
		  uint32_t dev_id, uint32_t baud,
		  uint32_t data_bits, uint32_t parity, uint32_t stop_bits)
{
	struct char_device_entry *dev;

	(void)char_port;

	dev = char_core_dev_lookup((char_dev_id_t)dev_id);
	if (dev == NULL)
		return KERN_INVALID_ARGUMENT;
	if (dev->info.class != CHAR_CLASS_TTY)
		return KERN_INVALID_ARGUMENT;
	if (char_core_cap_check(cap, cap_count, CHAR_CAP_TTY_CONFIG,
				(uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;

	if (dev->module->tty_set_attr == NULL)
		return KERN_FAILURE;
	return (dev->module->tty_set_attr(dev->priv, baud, data_bits,
					  parity, stop_bits) == 0)
		? KERN_SUCCESS : KERN_FAILURE;
}
