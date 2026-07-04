/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * char_server/modules/console.c — on-screen console TTY (#363).
 *
 * A virtual CHAR_CLASS_TTY device that stitches together the two
 * existing halves of an on-screen terminal:
 *
 *   output — bytes written via tty_write are rendered to the GPU
 *            framebuffer through libgpu_console's gpu_console_puts(),
 *            the same mirror path every server already uses for printf.
 *
 *   input  — key events from the PS/2 keyboard module (ps2.so).  Both
 *            modules live in this one char_server process, so instead
 *            of a Mach subscription across tasks the console registers
 *            an in-process sink (char_core_register_kbd_sink); core
 *            opens a loopback port, hands ps2.so a send right, and
 *            routes the fan-out back to console_kbd_sink().  Translated
 *            bytes land in an RX ring that tty_read drains.
 *
 * Line discipline lives in the shell, not here.  ush's read_line()
 * already echoes, cooks CR/BS and edits the line (it must, since over
 * serial the host terminal used to do it); so the console delivers
 * *raw* bytes and does no echo of its own — ush's write(tty_fd, …)
 * echo comes straight back through tty_write and onto the screen.
 * That keeps this increment thin and avoids double echo.
 *
 * Owns no hardware and takes no IRQ: probe always claims (it is a
 * software device), attach only wires the keyboard sink.  When the
 * bundle omits console.so nothing changes — ush falls back to the UART
 * TTY and the serial console behaves exactly as before.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/message.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include <char/char_module_abi.h>
#include <char/char_types.h>

#include "gpu_console.h"

/* ============================================================
 * X11-namespace keysyms we care about.  Mirror the values ps2.so
 * emits for the non-printable keys (ps2.c KSYM_*); printable keys
 * already arrive ASCII-aligned and pass straight through.
 * ============================================================ */

#define KSYM_BACKSPACE	0xFF08
#define KSYM_TAB	0xFF09
#define KSYM_RETURN	0xFF0D
#define KSYM_ESC	0xFF1B
#define KSYM_F1		0xFFBE		/* F1..F4 = 0xFFBE..0xFFC1 (X11) */

/*
 * Virtual terminals (#364).  The shell's output goes to surface
 * CONSOLE_VT_SURFACE (tty2) so it stays off the system log on surface 0
 * (tty1).  Ctrl+Alt+F1..Fn switches which surface is on screen; keep
 * CONSOLE_VT_MAX in step with the gpu module's VGA_NSURFACES.
 */
#define CONSOLE_VT_SURFACE	1u
#define CONSOLE_VT_MAX		4u

/* ============================================================
 * Per-instance state.  Single virtual console per board.
 * ============================================================ */

#define CON_RING_SIZE		1024u	/* must be power of two */
#define CON_RING_MASK		(CON_RING_SIZE - 1u)
#define CON_MAX_SUBSCRIBERS	8

struct console_priv {
	int		attached;

	/* RX ring: producer = console_kbd_sink (demux thread), consumer =
	 * console_tty_read (same demux thread).  Single-threaded server →
	 * no locking. */
	uint8_t		ring[CON_RING_SIZE];
	uint32_t	ring_head;	/* producer write index */
	uint32_t	ring_tail;	/* consumer read index */
	uint32_t	overrun_drops;	/* bytes lost when the ring is full */

	/* Subscribers get a header-only wake-up per input batch, mirroring
	 * uart.so.  ush polls tty_read so this is belt-and-braces, but a
	 * future notify-based client gets the same contract. */
	mach_port_t	subscribers[CON_MAX_SUBSCRIBERS];
	unsigned int	n_subscribers;
};

static struct console_priv console_singleton;

/*
 * One-shot: flipped the on-screen surface to the shell's VT the first
 * time ush produces output, so the shell is the default view once it is
 * up (#364).  After that the user's Ctrl+Alt+Fn choice is respected — a
 * background write from the shell must not yank the screen off the log.
 */
static int console_shown_ush;

/* ============================================================
 * RX ring helpers.
 * ============================================================ */

static void
console_ring_push(struct console_priv *p, uint8_t b)
{
	uint32_t next = (p->ring_head + 1u) & CON_RING_MASK;

	if (next == p->ring_tail) {
		p->overrun_drops++;
		return;
	}
	p->ring[p->ring_head] = b;
	p->ring_head = next;
}

/* ============================================================
 * Subscriber wake-up: one header-only Mach msg per input batch.
 * ============================================================ */

static void
console_notify_subscribers(struct console_priv *p)
{
	mach_msg_header_t msg;
	unsigned int i;
	mach_msg_return_t mr;

	if (p->n_subscribers == 0)
		return;

	memset(&msg, 0, sizeof(msg));
	msg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	msg.msgh_size = sizeof(msg);
	msg.msgh_id   = (mach_msg_id_t)CHAR_TTY_NOTIFY_ID;

	for (i = 0; i < p->n_subscribers; i++) {
		if (p->subscribers[i] == MACH_PORT_NULL)
			continue;
		msg.msgh_remote_port = p->subscribers[i];
		mr = mach_msg(&msg, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
			      sizeof(msg), 0,
			      MACH_PORT_NULL, 0, MACH_PORT_NULL);
		if (mr != MACH_MSG_SUCCESS && mr != MACH_SEND_TIMED_OUT) {
			(void)mach_port_deallocate(mach_task_self(),
						   p->subscribers[i]);
			p->subscribers[i] = MACH_PORT_NULL;
		}
	}
}

/* ============================================================
 * Keyboard sink — called by char_server's demux for every key event
 * ps2.so fans out to the loopback port (#363).  Translate make events
 * into raw bytes and push them into the RX ring.
 * ============================================================ */

static void
console_kbd_sink(void *priv, const char_kbd_event_t *ev)
{
	struct console_priv *p = priv;
	uint32_t keysym = ev->keysym;
	uint8_t  byte;

	if (!ev->pressed)		/* act on make only, ignore break */
		return;
	if (keysym == 0)		/* KSYM_NONE — unmapped key */
		return;

	/*
	 * VT switch (#364): Ctrl+Alt+F1..Fn selects the on-screen surface
	 * instead of feeding a byte to the shell.  F1 = surface 0 (system
	 * console / tty1), F2 = surface 1 (this shell / tty2), ...  The
	 * keystroke is consumed here — it never reaches the RX ring.
	 */
	if ((ev->modifiers & CHAR_KBD_MOD_CTRL) &&
	    (ev->modifiers & CHAR_KBD_MOD_ALT) &&
	    keysym >= KSYM_F1 && keysym < KSYM_F1 + CONSOLE_VT_MAX) {
		gpu_console_set_active_surface(keysym - KSYM_F1);
		return;
	}

	if (keysym < 0x80) {
		/* Printable / ASCII-aligned key.  Fold Ctrl into the C0
		 * control range so Ctrl-C, Ctrl-D, Ctrl-U … reach the shell
		 * as the bytes it expects. */
		byte = (uint8_t)keysym;
		if (ev->modifiers & CHAR_KBD_MOD_CTRL) {
			if (byte >= 'a' && byte <= 'z')
				byte = (uint8_t)(byte - 'a' + 1);
			else if (byte >= '@' && byte <= '_')
				byte = (uint8_t)(byte & 0x1F);
		}
	} else {
		switch (keysym) {
		case KSYM_RETURN:	byte = '\r'; break;
		case KSYM_BACKSPACE:	byte = 0x08; break;
		case KSYM_TAB:		byte = '\t'; break;
		case KSYM_ESC:		byte = 0x1B; break;
		default:		return;	/* arrows/F-keys: ignore for now */
		}
	}

	console_ring_push(p, byte);
	console_notify_subscribers(p);
}

/* ============================================================
 * Module ops.
 * ============================================================ */

static void *
console_probe(const struct hal_device_info *dev)
{
	(void)dev;
	if (console_singleton.attached)
		return NULL;
	return &console_singleton;
}

static int
console_attach(void *priv)
{
	struct console_priv *p = priv;

	p->ring_head     = 0;
	p->ring_tail     = 0;
	p->overrun_drops = 0;
	p->n_subscribers = 0;

	/* Register the in-process keyboard sink.  The loopback port itself
	 * is opened + subscribed by char_core_kbd_loopback_wire() after
	 * discovery (once ps2.so is attached too). */
	char_core_register_kbd_sink(console_kbd_sink, p);

	p->attached = 1;
	printf("console: on-screen TTY attached (gpu out, ps2 in)\n");
	return 0;
}

static void
console_detach(void *priv)
{
	struct console_priv *p = priv;
	unsigned int i;

	char_core_register_kbd_sink(NULL, NULL);
	for (i = 0; i < p->n_subscribers; i++) {
		if (p->subscribers[i] != MACH_PORT_NULL)
			(void)mach_port_deallocate(mach_task_self(),
						   p->subscribers[i]);
	}
	p->n_subscribers = 0;
	p->attached = 0;
}

/* tty_read — drain the RX ring (non-blocking; libposix polls). */
static int
console_tty_read(void *priv, char *buf, size_t max, size_t *out_len)
{
	struct console_priv *p = priv;
	size_t n = 0;

	while (n < max && p->ring_tail != p->ring_head) {
		buf[n++] = (char)p->ring[p->ring_tail];
		p->ring_tail = (p->ring_tail + 1u) & CON_RING_MASK;
	}
	*out_len = n;
	return 0;
}

/* tty_write — render to this console's own on-screen surface (tty2)
 * via libgpu_console, so the shell stays off the system log (tty1). */
static int
console_tty_write(void *priv, const char *buf, size_t len)
{
	(void)priv;

	/* The shell is starting to talk: make its VT the one on screen, so
	 * the graphical window shows ush by default once it is up (the boot
	 * log was on tty1 until now).  One-shot — see console_shown_ush. */
	if (!console_shown_ush) {
		console_shown_ush = 1;
		gpu_console_set_active_surface(CONSOLE_VT_SURFACE);
	}

	gpu_console_puts_surface(CONSOLE_VT_SURFACE, buf, len);
	return 0;
}

/* tty_set_attr — a virtual console has no baud/framing; accept and
 * ignore so admin clients that reuse the UART code path don't error. */
static int
console_tty_set_attr(void *priv, uint32_t baud, uint32_t data_bits,
		     uint32_t parity, uint32_t stop_bits)
{
	(void)priv; (void)baud; (void)data_bits; (void)parity; (void)stop_bits;
	return 0;
}

static int
console_tty_subscribe(void *priv, mach_port_t notify_port)
{
	struct console_priv *p = priv;

	if (p->n_subscribers >= CON_MAX_SUBSCRIBERS)
		return -1;
	p->subscribers[p->n_subscribers++] = notify_port;
	return 0;
}

const char_module_ops_t console_module_ops = {
	.name           = "console",
	.abi_version    = CHAR_MODULE_ABI_VERSION,
	.priority       = 10,		/* cosmetic: higher than legacy back-ends */
	.device_class   = CHAR_CLASS_TTY,
	.probe          = console_probe,
	.attach         = console_attach,
	.detach         = console_detach,
	.kbd_subscribe  = NULL,
	.tty_read       = console_tty_read,
	.tty_write      = console_tty_write,
	.tty_set_attr   = console_tty_set_attr,
	.tty_subscribe  = console_tty_subscribe,
	.mouse_subscribe = NULL,
};
