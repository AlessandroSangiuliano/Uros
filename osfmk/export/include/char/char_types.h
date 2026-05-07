/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * char_types.h — Public types shared between char_server, its
 * back-end modules (ps2.so #206, uart.so #207, future usb_hid /
 * gpio / parallel) and char_server clients.
 *
 * See docs/char_server_design.md (TBD) for the full architecture; the
 * pattern is the gpu_server one applied to character devices.
 */

#ifndef _CHAR_CHAR_TYPES_H_
#define _CHAR_CHAR_TYPES_H_

#include <stdint.h>

#define CHAR_TYPES_VERSION	1u

/* ============================================================
 * Opaque handles (server-local, per char_server task instance).
 * ============================================================ */

typedef uint32_t	char_dev_id_t;	/* index into char_server device table */
typedef uint64_t	char_event_id_t;/* monotonic per-device event sequence */

/* ============================================================
 * Capability ops — see #205.
 *
 * Carried in the `ops` field of a uros_cap token (cap_server) and
 * checked by char_server on every Mach RPC that targets a resource.
 * ============================================================ */

#define CHAR_CAP_KBD_READ		(1ull << 0)
#define CHAR_CAP_TTY_RW			(1ull << 1)
#define CHAR_CAP_TTY_CONFIG		(1ull << 2)
#define CHAR_CAP_DEV_ADMIN		(1ull << 3)

#define CHAR_CAP_RESOURCE_TYPE		0x43485200u	/* 'CHR\0' */

/* ============================================================
 * Device classes — discriminator inside char_server's device table.
 * Modules tag themselves at probe time so cap_check can route the
 * right cap mask to the right device.
 * ============================================================ */

#define CHAR_CLASS_NONE			0u
#define CHAR_CLASS_KEYBOARD		1u	/* ps2.so, future usb_hid_kbd */
#define CHAR_CLASS_TTY			2u	/* uart.so, future usb_serial */
#define CHAR_CLASS_MOUSE		3u	/* future ps2_mouse, usb_hid_mouse */

/* ============================================================
 * Keyboard event payload — wire-format, fixed size, sent inline
 * via Mach msg from ps2.so to subscribers (#206).
 *
 * Modifier bitmap layout (lower 8 bits used today):
 *   bit 0: Shift   bit 1: Ctrl    bit 2: Alt    bit 3: Meta/Win
 *   bit 4: CapsLock (state)       bit 5: NumLock (state)
 *   bit 6: ScrollLock (state)     bit 7: reserved
 * ============================================================ */

#define CHAR_KBD_MOD_SHIFT		(1u << 0)
#define CHAR_KBD_MOD_CTRL		(1u << 1)
#define CHAR_KBD_MOD_ALT		(1u << 2)
#define CHAR_KBD_MOD_META		(1u << 3)
#define CHAR_KBD_MOD_CAPSLOCK		(1u << 4)
#define CHAR_KBD_MOD_NUMLOCK		(1u << 5)
#define CHAR_KBD_MOD_SCROLLLOCK		(1u << 6)

typedef struct char_kbd_event {
	uint32_t	scancode;	/* raw scancode (with E0/E1 prefix shifted in) */
	uint32_t	keysym;		/* normalized key code (X11-ish) */
	uint32_t	modifiers;	/* CHAR_KBD_MOD_* bitmap */
	uint8_t		pressed;	/* 1 = make, 0 = break */
	uint8_t		_pad[3];
} char_kbd_event_t;

/* ============================================================
 * Device info (returned by char_query_devices).  POD layout, OOL
 * array.  Mirrors gpu_device_info.
 * ============================================================ */

#define CHAR_DEV_NAME_LEN	32

struct char_device_info {
	char_dev_id_t	id;
	uint32_t	class;		/* CHAR_CLASS_* */
	uint32_t	flags;		/* class-specific bitmap, 0 today */
	char		module_name[CHAR_DEV_NAME_LEN];	/* "ps2", "uart", ... */
};

#endif /* _CHAR_CHAR_TYPES_H_ */
