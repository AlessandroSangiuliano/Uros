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
#include <mach/message.h>
#include <char/char_types.h>

#define CHAR_MODULE_ABI_VERSION		3u	/* +mouse_subscribe (#215) */

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

	/* TTY ops (CHAR_CLASS_TTY modules only).
	 *
	 * tty_read is non-blocking — returns out_len=0 if the RX ring is
	 * empty.  Clients that want to wait for data subscribe a notify
	 * port via tty_subscribe; the module sends a wake-up message
	 * (msgh_id = CHAR_TTY_NOTIFY_ID, no payload) when data arrives.
	 * After the wake-up the client calls tty_read to drain.  Coarse
	 * but matches the single-threaded server loop and avoids parking
	 * MIG reply ports across the IRQ. */
	int		(*tty_read)(void *priv, char *buf, size_t max,
				    size_t *out_len);
	int		(*tty_write)(void *priv, const char *buf, size_t len);
	int		(*tty_set_attr)(void *priv, uint32_t baud,
					uint32_t data_bits, uint32_t parity,
					uint32_t stop_bits);
	int		(*tty_subscribe)(void *priv, mach_port_t notify_port);

	/* Mouse subscribe (CHAR_CLASS_MOUSE modules only).  Same
	 * convention as kbd_subscribe: SEND right retained by module,
	 * one Mach msg per packet carrying char_mouse_event_t inline.
	 * Subscribers diff `buttons` against the previous packet to
	 * derive press/release edges — the hardware reports state on
	 * every packet, not events. */
	int		(*mouse_subscribe)(void *priv, mach_port_t notify_port);
} char_module_ops_t;

/* msgh_id used for the data-available wake-up sent by a TTY module
 * to subscribers registered via tty_subscribe.  Distinct from
 * char_kbd_event_t messages so a client subscribed to both planes
 * can disambiguate. */
#define CHAR_TTY_NOTIFY_ID		3201u

/*
 * msgh_id a CHAR_CLASS_KEYBOARD module stamps on every fan-out message
 * carrying one char_kbd_event_t inline (see kbd_subscribe).  Chosen
 * outside the gpu_server (4000) / char_server (4100) MIG subsystems and
 * clear of the IRQ-notification range (>= 3000, < 3016), so a subscriber
 * demux can route it with a single id compare.  In-process subscribers
 * (the console TTY, #363) match on the same id as external ones.
 */
#define CHAR_KBD_EVENT_MSGH_ID		4200

/*
 * Wire format of a keyboard fan-out message.  Producer (ps2.so) and
 * every subscriber — including the in-process console (#363), which
 * receives these via a loopback send right on char_server's own port
 * set — share this one layout so the two ends never drift.
 */
typedef struct char_kbd_event_msg {
	mach_msg_header_t	head;
	NDR_record_t		ndr;
	char_kbd_event_t	event;
} char_kbd_event_msg_t;

#define CHAR_MODULE_ENTRY_SUFFIX	"_module_ops"

/* ============================================================
 * Module → core callbacks.
 *
 * char_server core exports these symbols via --export-dynamic;
 * modules dlopen'd into the same process call them directly.
 * Same wiring trick block_device_server modules use to reach
 * device_master MIG stubs.
 *
 * A module's IRQ handler runs on char_server's main dispatch
 * thread (the one inside mach_msg_server) when an IRQ
 * notification arrives on the registered port.  Keep it short:
 * read a few bytes, build an event, fan out, return.
 * ============================================================ */

/*
 * Register an IRQ handler.  Core allocates a Mach port, adds it
 * to the main port_set, calls device_intr_register on the kernel
 * master_device.  When the IRQ notification arrives on that port,
 * the demux invokes `handler(arg)`.  Returns 0 on success.
 *
 * Only one handler per IRQ line.
 */
extern int char_core_irq_register(uint32_t irq,
				  void (*handler)(void *arg),
				  void *arg);

/*
 * Unregister the handler installed by char_core_irq_register.
 * Safe to call from detach().
 */
extern int char_core_irq_unregister(uint32_t irq);

/*
 * Register a keyboard-event sink (#363).  The console TTY module calls
 * this from attach() so that in-process keyboard fan-out — ps2.so
 * sending CHAR_KBD_EVENT_MSGH_ID messages to the loopback port core
 * opens in char_core_kbd_loopback_wire() — is delivered straight to
 * `fn(arg, event)` from char_server's demux.  Only one sink; the last
 * registration wins.  A module that is not the console never calls
 * this.
 */
extern void char_core_register_kbd_sink(
	void (*fn)(void *arg, const char_kbd_event_t *ev), void *arg);

#endif /* _CHAR_CHAR_MODULE_ABI_H_ */
