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
#include <mach/mach_port.h>
#include <stdio.h>
#include <string.h>
#include <mach/char_server_types.h>	/* CHR_BUF_MAX */
#include "char_server.h"
#include "proc.h"			/* MIG user stubs for proc_server */
#include "proc_types.h"			/* PROC_OK, proc_pid_t */

/* Pick the right cap op for a given device class. */
static uint64_t
class_to_open_op(uint32_t cls)
{
	switch (cls) {
	case CHAR_CLASS_KEYBOARD:	return CHAR_CAP_KBD_READ;
	case CHAR_CLASS_TTY:		return CHAR_CAP_TTY_RW;
	case CHAR_CLASS_MOUSE:		return CHAR_CAP_MOUSE_READ;
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

/*
 * tty_get_ctty handler — walk the device table for an entry whose
 * ctty_sid matches.  Linear scan over CHAR_MAX_DEVICES (8 today) is
 * cheap.  No cap_check; see the defs comment.
 */
kern_return_t
char_tty_get_ctty(mach_port_t char_port, int sid,
		  uint32_t *dev_id, int *result)
{
	struct char_device_entry *dev;
	unsigned int n, i, found = 0;

	(void)char_port;

	*dev_id = 0;
	*result = CHR_ERR_INVAL;

	if (sid <= 0)
		return KERN_SUCCESS;

	n = char_core_dev_count();
	for (i = 0; i < n; i++) {
		dev = char_core_dev_lookup((char_dev_id_t)(i + 1));
		if (dev == NULL || dev->info.class != CHAR_CLASS_TTY)
			continue;
		if (dev->ctty_sid == sid) {
			*dev_id = (uint32_t)dev->id;
			*result = CHR_OK;
			found = 1;
			break;
		}
	}
	if (!found)
		*result = CHR_ERR_INVAL;
	return KERN_SUCCESS;
}

/*
 * tty_session_owner_check — cap-check bypass for processes that are
 * members of the session that owns this tty as ctty.  Returns 1 if
 * the bypass applies (no cap needed), 0 otherwise.  Cheap proc_getsid
 * round-trip — only invoked on tty_read / tty_write entry, and only
 * after dev->ctty_sid is known to be set.
 */
static int
tty_session_owner_check(struct char_device_entry *dev, int caller_pid)
{
	int sid = 0, rc = 0;

	if (caller_pid <= 0 || dev->ctty_sid == 0)
		return 0;
	char_proc_port_resolve();
	if (char_proc_port == MACH_PORT_NULL)
		return 0;
	if (proc_getsid(char_proc_port, (proc_pid_t)caller_pid,
			(proc_pid_t *)&sid, &rc) != KERN_SUCCESS)
		return 0;
	if (rc != PROC_OK)
		return 0;
	return sid == dev->ctty_sid;
}

/*
 * tty_jobctl_check — implements POSIX background-read/write semantics
 * (#275.2 / #275.3).  Returns CHR_OK if the caller is allowed to
 * proceed; CHR_TTY_BACKGROUND after dispatching SIGTTIN/SIGTTOU to the
 * caller's pgrp; CHR_OK with no check if the device has no ctty owner
 * or caller_pid == 0 (privileged/debug path).
 *
 * Three proc_server round-trips today (getsid, getpgid, tcgetpgrp).
 * A future compound RPC can collapse them; for v0.1 keep it composed
 * out of the existing primitives so proc_server stays untouched.
 */
static int
tty_jobctl_check(struct char_device_entry *dev, int caller_pid, int signo)
{
	int sid = 0, pgrp = 0, fg_pgrp = 0, rc = 0;
	kern_return_t kr;
	unsigned n_sent = 0;

	if (caller_pid == 0 || dev->ctty_sid == 0)
		return CHR_OK;
	char_proc_port_resolve();
	if (char_proc_port == MACH_PORT_NULL)
		return CHR_OK;

	kr = proc_getsid(char_proc_port, (proc_pid_t)caller_pid,
			 (proc_pid_t *)&sid, &rc);
	if (kr != KERN_SUCCESS || rc != PROC_OK || sid != dev->ctty_sid)
		return CHR_OK;

	kr = proc_tcgetpgrp(char_proc_port, (proc_pid_t)sid,
			    (proc_pid_t *)&fg_pgrp, &rc);
	if (kr != KERN_SUCCESS || rc != PROC_OK)
		return CHR_OK;

	kr = proc_getpgid(char_proc_port, (proc_pid_t)caller_pid,
			  (proc_pid_t *)&pgrp, &rc);
	if (kr != KERN_SUCCESS || rc != PROC_OK)
		return CHR_OK;

	if (pgrp == fg_pgrp)
		return CHR_OK;

	/* Background pgrp — deliver the signal and tell the caller to bail.
	 * Per POSIX the signal goes to the whole pgrp, not just the caller;
	 * proc_killpg returns the number of pids reached (irrelevant here). */
	(void)proc_killpg(char_proc_port, (proc_pid_t)pgrp, signo,
			  &n_sent, &rc);
	return CHR_TTY_BACKGROUND;
}

kern_return_t
char_tty_read(mach_port_t char_port,
	      char *cap, mach_msg_type_number_t cap_count,
	      uint32_t dev_id, int caller_pid, uint32_t max,
	      char *buf, mach_msg_type_number_t *buf_count,
	      int *result)
{
	struct char_device_entry *dev;
	size_t got = 0;
	int jc;

	(void)char_port;

	*buf_count = 0;
	*result = CHR_ERR_INVAL;

	dev = char_core_dev_lookup((char_dev_id_t)dev_id);
	if (dev == NULL)
		return KERN_INVALID_ARGUMENT;
	if (dev->info.class != CHAR_CLASS_TTY)
		return KERN_INVALID_ARGUMENT;
	if (!tty_session_owner_check(dev, caller_pid)
	    && char_core_cap_check(cap, cap_count, CHAR_CAP_TTY_RW,
				   (uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;

	jc = tty_jobctl_check(dev, caller_pid, PROC_SIGTTIN);
	if (jc == CHR_TTY_BACKGROUND) {
		*result = CHR_TTY_BACKGROUND;
		return KERN_SUCCESS;
	}

	if (dev->module->tty_read == NULL) {
		*result = CHR_ERR_NO_MODULE;
		return KERN_FAILURE;
	}

	if (max > CHR_BUF_MAX)
		max = CHR_BUF_MAX;

	if (dev->module->tty_read(dev->priv, buf, (size_t)max, &got) != 0) {
		*result = CHR_ERR_KERNEL;
		return KERN_FAILURE;
	}
	*buf_count = (mach_msg_type_number_t)got;
	*result = CHR_OK;
	return KERN_SUCCESS;
}

kern_return_t
char_tty_write(mach_port_t char_port,
	       char *cap, mach_msg_type_number_t cap_count,
	       uint32_t dev_id, int caller_pid,
	       char *buf, mach_msg_type_number_t buf_count,
	       int *result)
{
	struct char_device_entry *dev;
	int jc;

	(void)char_port;

	*result = CHR_ERR_INVAL;

	dev = char_core_dev_lookup((char_dev_id_t)dev_id);
	if (dev == NULL)
		return KERN_INVALID_ARGUMENT;
	if (dev->info.class != CHAR_CLASS_TTY)
		return KERN_INVALID_ARGUMENT;
	if (!tty_session_owner_check(dev, caller_pid)
	    && char_core_cap_check(cap, cap_count, CHAR_CAP_TTY_RW,
				   (uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;

	/* TOSTOP gates the bg-write policy.  When it's off, writes from
	 * bg pgrps pass through unmodified (Linux/BSD default). */
	if (dev->tty_tostop) {
		jc = tty_jobctl_check(dev, caller_pid, PROC_SIGTTOU);
		if (jc == CHR_TTY_BACKGROUND) {
			*result = CHR_TTY_BACKGROUND;
			return KERN_SUCCESS;
		}
	}

	if (dev->module->tty_write == NULL) {
		*result = CHR_ERR_NO_MODULE;
		return KERN_FAILURE;
	}
	(void)dev->module->tty_write(dev->priv, buf, (size_t)buf_count);
	*result = CHR_OK;
	return KERN_SUCCESS;
}

kern_return_t
char_tty_set_tostop(mach_port_t char_port,
		    char *cap, mach_msg_type_number_t cap_count,
		    uint32_t dev_id, int enable, int *result)
{
	struct char_device_entry *dev;

	(void)char_port;

	*result = CHR_ERR_INVAL;

	dev = char_core_dev_lookup((char_dev_id_t)dev_id);
	if (dev == NULL)
		return KERN_INVALID_ARGUMENT;
	if (dev->info.class != CHAR_CLASS_TTY)
		return KERN_INVALID_ARGUMENT;
	if (char_core_cap_check(cap, cap_count, CHAR_CAP_TTY_CONFIG,
				(uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;

	dev->tty_tostop = (enable != 0);
	*result = CHR_OK;
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

kern_return_t
char_tty_subscribe(mach_port_t char_port,
		   char *cap, mach_msg_type_number_t cap_count,
		   uint32_t dev_id, mach_port_t notify_port)
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

	if (dev->module->tty_subscribe == NULL)
		return KERN_FAILURE;
	return (dev->module->tty_subscribe(dev->priv, notify_port) == 0)
		? KERN_SUCCESS : KERN_FAILURE;
}

/*
 * tty_acquire_ctty — bind this tty as the controlling terminal of the
 * caller's session (#275.1).  We hand proc_server a fresh send right
 * to char_service_port; once #275.2 wires per-tty signal delivery, the
 * passed port will become a per-device handle instead.
 */
kern_return_t
char_tty_acquire_ctty(mach_port_t char_port,
		      char *cap, mach_msg_type_number_t cap_count,
		      uint32_t dev_id, int sid, int *result)
{
	struct char_device_entry *dev;
	mach_port_t              tty_port;
	kern_return_t            kr;
	int                      proc_rc = 0;

	(void)char_port;

	dev = char_core_dev_lookup((char_dev_id_t)dev_id);
	if (dev == NULL)
		return KERN_INVALID_ARGUMENT;
	if (dev->info.class != CHAR_CLASS_TTY)
		return KERN_INVALID_ARGUMENT;
	/*
	 * Cap-check policy on bind (#275.5): unbound ttys can be claimed
	 * without a cap so the first session leader (ush, getty) doesn't
	 * need cap_acquire bootstrapping just to grab its ctty.  Once
	 * dev->ctty_sid is set, proc_server's PROC_ERR_PERM blocks any
	 * second claim from another session anyway.  Bound ttys keep the
	 * cap_check so an unrelated caller can't trample the binding via
	 * a re-bind race after the owner releases.
	 */
	if (dev->ctty_sid != 0
	    && char_core_cap_check(cap, cap_count, CHAR_CAP_TTY_RW,
				   (uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;

	char_proc_port_resolve();
	if (char_proc_port == MACH_PORT_NULL) {
		printf("char_server: tty_acquire_ctty dev=%u sid=%d: "
		       "proc_server unavailable\n", dev_id, sid);
		return KERN_FAILURE;
	}

	/* Fresh send right — proc_server keeps it and deallocates on
	 * proc_clear_ctty / session-leader exit (see proc.defs). */
	kr = mach_port_insert_right(mach_task_self(),
				    char_service_port, char_service_port,
				    MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("char_server: tty_acquire_ctty insert_right "
		       "failed (kr=%d)\n", (int)kr);
		return KERN_FAILURE;
	}
	tty_port = char_service_port;

	kr = proc_set_ctty(char_proc_port, (proc_pid_t)sid,
			   tty_port, &proc_rc);
	if (kr != KERN_SUCCESS) {
		/* MIG transport failure — proc_server did not consume the
		 * send right, so we drop the userref we just created. */
		(void)mach_port_deallocate(mach_task_self(), tty_port);
		printf("char_server: proc_set_ctty MIG kr=%d\n", (int)kr);
		return kr;
	}

	if (proc_rc == PROC_OK)
		dev->ctty_sid = sid;

	*result = proc_rc;
	return KERN_SUCCESS;
}

/* ============================================================
 * Mouse plane (#215)
 * ============================================================ */

kern_return_t
char_mouse_subscribe(mach_port_t char_port,
		     char *cap, mach_msg_type_number_t cap_count,
		     uint32_t dev_id, mach_port_t notify_port)
{
	struct char_device_entry *dev;

	(void)char_port;

	dev = char_core_dev_lookup((char_dev_id_t)dev_id);
	if (dev == NULL)
		return KERN_INVALID_ARGUMENT;
	if (dev->info.class != CHAR_CLASS_MOUSE)
		return KERN_INVALID_ARGUMENT;
	if (char_core_cap_check(cap, cap_count, CHAR_CAP_MOUSE_READ,
				(uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;

	if (dev->module->mouse_subscribe == NULL)
		return KERN_FAILURE;
	return (dev->module->mouse_subscribe(dev->priv, notify_port) == 0)
		? KERN_SUCCESS : KERN_FAILURE;
}
