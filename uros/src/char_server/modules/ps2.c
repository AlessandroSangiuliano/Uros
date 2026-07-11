/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * char_server/modules/ps2.c — PS/2 keyboard back-end (#206).
 *
 * Owns the i8042 PS/2 controller's port-1 (keyboard) channel.
 * Talks raw to ports 0x60/0x64; takes IRQ 1 via
 * char_core_irq_register().  Translates AT scancode-set 1 into
 * (scancode, keysym, modifiers, pressed) events and fans them out
 * inline as Mach messages to every subscriber.
 *
 * Boundaries (see #206):
 *   - No line discipline (cooked/raw, echo, line buffering).  That
 *     belongs to a future tty.so module / userspace lib.
 *   - No software repeat.  Hardware typewriting handles it.
 *   - No mouse handling — port 2 of the 8042 stays untouched here;
 *     a future ps2_mouse.so module owns it.
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

/* ============================================================
 * 8042 controller registers and commands
 * ============================================================ */

#define PS2_DATA	0x60
#define PS2_STATUS	0x64	/* read */
#define PS2_CMD		0x64	/* write */

#define PS2_STAT_OBF	0x01	/* output buffer full (data ready) */
#define PS2_STAT_IBF	0x02	/* input buffer full (cmd in flight) */
#define PS2_STAT_AUX	0x20	/* data is from port 2 (mouse) */

#define PS2_CMD_READ_CFG	0x20
#define PS2_CMD_WRITE_CFG	0x60
#define PS2_CMD_DISABLE_P1	0xAD
#define PS2_CMD_ENABLE_P1	0xAE
#define PS2_CMD_DISABLE_P2	0xA7

#define KBD_CMD_ENABLE_SCAN	0xF4
#define KBD_CMD_DISABLE_SCAN	0xF5

#define PS2_IRQ			1u

/* ============================================================
 * Inline port I/O.
 * ============================================================ */

static inline uint8_t inb(uint16_t port)
{
	uint8_t v;
	__asm__ __volatile__ ("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static inline void outb(uint16_t port, uint8_t v)
{
	__asm__ __volatile__ ("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* Bounded waits — controller stalls should never block forever. */
static int
wait_input_empty(void)
{
	unsigned int spins;
	for (spins = 0; spins < 100000u; spins++)
		if ((inb(PS2_STATUS) & PS2_STAT_IBF) == 0)
			return 0;
	return -1;
}

static int
wait_output_full(void)
{
	unsigned int spins;
	for (spins = 0; spins < 100000u; spins++)
		if (inb(PS2_STATUS) & PS2_STAT_OBF)
			return 0;
	return -1;
}

static void
drain_output_buffer(void)
{
	unsigned int spins;
	for (spins = 0; spins < 16u; spins++) {
		if ((inb(PS2_STATUS) & PS2_STAT_OBF) == 0)
			return;
		(void)inb(PS2_DATA);
	}
}

static int
ctrl_send(uint8_t cmd)
{
	if (wait_input_empty() < 0)
		return -1;
	outb(PS2_CMD, cmd);
	return 0;
}

static int
ctrl_write_data(uint8_t v)
{
	if (wait_input_empty() < 0)
		return -1;
	outb(PS2_DATA, v);
	return 0;
}

/* ============================================================
 * Scancode-set-1 → keysym table
 *
 * Indexed by scancode (1 byte).  A leading E0 prefix maps a small
 * fixed set of extended keys (arrows, right Ctrl/Alt, ...).
 *
 * Keysyms here are deliberately ASCII-aligned for printable keys so
 * the simplest subscriber can just write `event.keysym` to a tty.
 * Non-printable keys use the X11 0xFF00+ namespace convention.
 * ============================================================ */

#define KSYM_NONE	0x0000
#define KSYM_LSHIFT	0xFFE1
#define KSYM_RSHIFT	0xFFE2
#define KSYM_LCTRL	0xFFE3
#define KSYM_RCTRL	0xFFE4
#define KSYM_LALT	0xFFE9
#define KSYM_RALT	0xFFEA
#define KSYM_CAPSLOCK	0xFFE5
#define KSYM_NUMLOCK	0xFF7F
#define KSYM_SCROLLLOCK	0xFF14
#define KSYM_BACKSPACE	0xFF08
#define KSYM_TAB	0xFF09
#define KSYM_RETURN	0xFF0D
#define KSYM_ESC	0xFF1B
#define KSYM_F1		0xFFBE
#define KSYM_F2		0xFFBF		/* F2..F4 for VT switch (#364) */
#define KSYM_F3		0xFFC0
#define KSYM_F4		0xFFC1
#define KSYM_UP		0xFF52
#define KSYM_DOWN	0xFF54
#define KSYM_LEFT	0xFF51
#define KSYM_RIGHT	0xFF53
#define KSYM_HOME	0xFF50
#define KSYM_END	0xFF57
#define KSYM_PGUP	0xFF55
#define KSYM_PGDN	0xFF56
#define KSYM_INSERT	0xFF63
#define KSYM_DELETE	0xFFFF

/* Plain (no shift) and shift columns.  US QWERTY layout. */
static const uint16_t scset1_plain[128] = {
	[0x01] = KSYM_ESC,
	[0x02] = '1',	[0x03] = '2',	[0x04] = '3',	[0x05] = '4',
	[0x06] = '5',	[0x07] = '6',	[0x08] = '7',	[0x09] = '8',
	[0x0A] = '9',	[0x0B] = '0',	[0x0C] = '-',	[0x0D] = '=',
	[0x0E] = KSYM_BACKSPACE, [0x0F] = KSYM_TAB,
	[0x10] = 'q',	[0x11] = 'w',	[0x12] = 'e',	[0x13] = 'r',
	[0x14] = 't',	[0x15] = 'y',	[0x16] = 'u',	[0x17] = 'i',
	[0x18] = 'o',	[0x19] = 'p',	[0x1A] = '[',	[0x1B] = ']',
	[0x1C] = KSYM_RETURN, [0x1D] = KSYM_LCTRL,
	[0x1E] = 'a',	[0x1F] = 's',	[0x20] = 'd',	[0x21] = 'f',
	[0x22] = 'g',	[0x23] = 'h',	[0x24] = 'j',	[0x25] = 'k',
	[0x26] = 'l',	[0x27] = ';',	[0x28] = '\'',	[0x29] = '`',
	[0x2A] = KSYM_LSHIFT, [0x2B] = '\\',
	[0x2C] = 'z',	[0x2D] = 'x',	[0x2E] = 'c',	[0x2F] = 'v',
	[0x30] = 'b',	[0x31] = 'n',	[0x32] = 'm',	[0x33] = ',',
	[0x34] = '.',	[0x35] = '/',	[0x36] = KSYM_RSHIFT,
	[0x37] = '*',	[0x38] = KSYM_LALT, [0x39] = ' ',
	[0x3A] = KSYM_CAPSLOCK,
	[0x3B] = KSYM_F1, [0x3C] = KSYM_F2, [0x3D] = KSYM_F3, [0x3E] = KSYM_F4,
	/* F5..F12 still omitted — VT switch (#364) only needs F1..F4. */
	[0x45] = KSYM_NUMLOCK, [0x46] = KSYM_SCROLLLOCK,
};

static const uint16_t scset1_shift[128] = {
	[0x01] = KSYM_ESC,
	[0x02] = '!',	[0x03] = '@',	[0x04] = '#',	[0x05] = '$',
	[0x06] = '%',	[0x07] = '^',	[0x08] = '&',	[0x09] = '*',
	[0x0A] = '(',	[0x0B] = ')',	[0x0C] = '_',	[0x0D] = '+',
	[0x0E] = KSYM_BACKSPACE, [0x0F] = KSYM_TAB,
	[0x10] = 'Q',	[0x11] = 'W',	[0x12] = 'E',	[0x13] = 'R',
	[0x14] = 'T',	[0x15] = 'Y',	[0x16] = 'U',	[0x17] = 'I',
	[0x18] = 'O',	[0x19] = 'P',	[0x1A] = '{',	[0x1B] = '}',
	[0x1C] = KSYM_RETURN, [0x1D] = KSYM_LCTRL,
	[0x1E] = 'A',	[0x1F] = 'S',	[0x20] = 'D',	[0x21] = 'F',
	[0x22] = 'G',	[0x23] = 'H',	[0x24] = 'J',	[0x25] = 'K',
	[0x26] = 'L',	[0x27] = ':',	[0x28] = '"',	[0x29] = '~',
	[0x2A] = KSYM_LSHIFT, [0x2B] = '|',
	[0x2C] = 'Z',	[0x2D] = 'X',	[0x2E] = 'C',	[0x2F] = 'V',
	[0x30] = 'B',	[0x31] = 'N',	[0x32] = 'M',	[0x33] = '<',
	[0x34] = '>',	[0x35] = '?',	[0x36] = KSYM_RSHIFT,
	[0x37] = '*',	[0x38] = KSYM_LALT, [0x39] = ' ',
	[0x3A] = KSYM_CAPSLOCK,
	[0x3B] = KSYM_F1, [0x3C] = KSYM_F2, [0x3D] = KSYM_F3, [0x3E] = KSYM_F4,
	[0x45] = KSYM_NUMLOCK, [0x46] = KSYM_SCROLLLOCK,
};

/* Extended (E0-prefixed) lookup — only the keys we care about. */
static uint16_t
e0_keysym(uint8_t sc)
{
	switch (sc) {
	case 0x1D: return KSYM_RCTRL;
	case 0x38: return KSYM_RALT;
	case 0x47: return KSYM_HOME;
	case 0x48: return KSYM_UP;
	case 0x49: return KSYM_PGUP;
	case 0x4B: return KSYM_LEFT;
	case 0x4D: return KSYM_RIGHT;
	case 0x4F: return KSYM_END;
	case 0x50: return KSYM_DOWN;
	case 0x51: return KSYM_PGDN;
	case 0x52: return KSYM_INSERT;
	case 0x53: return KSYM_DELETE;
	default:   return KSYM_NONE;
	}
}

/* ============================================================
 * Per-instance state.  Single-instance: one PS/2 controller per
 * legacy i386 board.
 * ============================================================ */

#define PS2_MAX_SUBSCRIBERS	8

struct ps2_priv {
	int		attached;
	uint32_t	modifiers;	/* CHAR_KBD_MOD_* current state */
	int		e0_pending;	/* next byte is part of an E0 seq */
	mach_port_t	subscribers[PS2_MAX_SUBSCRIBERS];
	unsigned int	n_subscribers;
};

static struct ps2_priv ps2_singleton;

/* ============================================================
 * Event fan-out.  One inline Mach msg per subscriber per event.
 *
 * The msgh_id (CHAR_KBD_EVENT_MSGH_ID) and the wire struct
 * (char_kbd_event_msg_t) are the shared contract in
 * <char/char_module_abi.h> — every subscriber, in-process (the
 * console TTY, #363) or external, decodes with the same layout.
 * ============================================================ */

static void
ps2_fanout(struct ps2_priv *p, const char_kbd_event_t *event)
{
	char_kbd_event_msg_t msg;
	unsigned int i;
	mach_msg_return_t mr;

	memset(&msg, 0, sizeof(msg));
	msg.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	msg.head.msgh_size = sizeof(msg);
	msg.head.msgh_id   = CHAR_KBD_EVENT_MSGH_ID;
	msg.event = *event;

	for (i = 0; i < p->n_subscribers; i++) {
		if (p->subscribers[i] == MACH_PORT_NULL)
			continue;
		msg.head.msgh_remote_port = p->subscribers[i];
		mr = mach_msg(&msg.head,
			      MACH_SEND_MSG | MACH_SEND_TIMEOUT,
			      sizeof(msg), 0,
			      MACH_PORT_NULL, 0,
			      MACH_PORT_NULL);
		if (mr != MACH_MSG_SUCCESS &&
		    mr != MACH_SEND_TIMED_OUT) {
			/* Subscriber dead or unreachable — drop the slot
			 * so we stop firing at it.  A no-senders /
			 * dead-name notification path lands with the
			 * #202-equivalent for char_server. */
			(void)mach_port_deallocate(mach_task_self(),
						   p->subscribers[i]);
			p->subscribers[i] = MACH_PORT_NULL;
		}
	}
}

/* ============================================================
 * IRQ handler — reads scancodes from 0x60, builds events.
 * ============================================================ */

static void
ps2_handle_scancode(struct ps2_priv *p, uint8_t sc)
{
	uint8_t code = sc & 0x7F;
	int pressed = (sc & 0x80) == 0;
	uint16_t keysym = KSYM_NONE;
	int e0 = p->e0_pending;
	uint32_t mod_bit = 0;

	p->e0_pending = 0;

	if (sc == 0xE0) {
		p->e0_pending = 1;
		return;
	}
	if (sc == 0xE1) {
		/* Pause sequence — discard for now. */
		return;
	}

	if (e0) {
		keysym = e0_keysym(code);
	} else {
		int shift = (p->modifiers & CHAR_KBD_MOD_SHIFT) ||
			    (p->modifiers & CHAR_KBD_MOD_CAPSLOCK);
		keysym = shift ? scset1_shift[code] : scset1_plain[code];
	}

	/* Update modifier state for the next event. */
	switch (keysym) {
	case KSYM_LSHIFT: case KSYM_RSHIFT: mod_bit = CHAR_KBD_MOD_SHIFT; break;
	case KSYM_LCTRL:  case KSYM_RCTRL:  mod_bit = CHAR_KBD_MOD_CTRL;  break;
	case KSYM_LALT:   case KSYM_RALT:   mod_bit = CHAR_KBD_MOD_ALT;   break;
	default: break;
	}
	if (mod_bit) {
		if (pressed)
			p->modifiers |= mod_bit;
		else
			p->modifiers &= ~mod_bit;
	}
	if (pressed) {
		switch (keysym) {
		case KSYM_CAPSLOCK:
			p->modifiers ^= CHAR_KBD_MOD_CAPSLOCK; break;
		case KSYM_NUMLOCK:
			p->modifiers ^= CHAR_KBD_MOD_NUMLOCK; break;
		case KSYM_SCROLLLOCK:
			p->modifiers ^= CHAR_KBD_MOD_SCROLLLOCK; break;
		default: break;
		}
	}

	/*
	 * #382: Ctrl+D is the kernel-debugger break key (#335 kept it in
	 * the kernel's own PS/2 snooper, but ps2.so takes IRQ 1 over once
	 * it attaches, so the snooper goes deaf).  Report it to the
	 * kernel; the RPC succeeds only when the -K boot flag armed the
	 * break, and the keystroke is then consumed by the DDB session.
	 */
	if (pressed && keysym == 'd' &&
	    (p->modifiers & CHAR_KBD_MOD_CTRL) &&
	    char_core_ddb_break() == 0)
		return;

	{
		char_kbd_event_t ev;
		ev.scancode  = (e0 ? ((uint32_t)0xE000 | code) : code);
		ev.keysym    = keysym;
		ev.modifiers = p->modifiers;
		ev.pressed   = pressed ? 1u : 0u;
		ev._pad[0]   = 0;
		ev._pad[1]   = 0;
		ev._pad[2]   = 0;
		ps2_fanout(p, &ev);
	}
}

static void
ps2_irq_handler(void *arg)
{
	struct ps2_priv *p = arg;
	unsigned int budget;

	/* Drain whatever the controller has queued.  Bounded budget so
	 * a stuck IRQ doesn't starve the rest of the demux. */
	for (budget = 0; budget < 32u; budget++) {
		uint8_t status = inb(PS2_STATUS);
		if ((status & PS2_STAT_OBF) == 0)
			break;
		if (status & PS2_STAT_AUX) {
			/* Mouse byte — port 2 not ours.  Drain so the
			 * controller releases its OBF; future
			 * ps2_mouse.so will subscribe properly. */
			(void)inb(PS2_DATA);
			continue;
		}
		{
			uint8_t sc = inb(PS2_DATA);
#ifdef PS2_DEBUG_TRACE	/* set via -DPS2_DEBUG_TRACE for bring-up */
			printf("ps2: sc=0x%02x %s\n",
			       (unsigned)sc,
			       (sc & 0x80) ? "rel" : "press");
#endif
			ps2_handle_scancode(p, sc);
		}
	}
}

/* ============================================================
 * Module ops
 * ============================================================ */

static void *
ps2_probe(const struct hal_device_info *dev)
{
	(void)dev;
	if (ps2_singleton.attached)
		return NULL;
	return &ps2_singleton;
}

static int
ps2_attach(void *priv)
{
	struct ps2_priv *p = priv;
	uint8_t cfg;

	/* 8042 init sequence: disable both ports, drain OBF, read+modify
	 * config byte to enable port-1 IRQ + translation, re-enable
	 * port 1, tell the keyboard to scan. */
	(void)ctrl_send(PS2_CMD_DISABLE_P1);
	(void)ctrl_send(PS2_CMD_DISABLE_P2);
	drain_output_buffer();

	if (ctrl_send(PS2_CMD_READ_CFG) < 0 || wait_output_full() < 0) {
		printf("ps2: read config failed\n");
		return -1;
	}
	cfg = inb(PS2_DATA);
	cfg |= 0x01;	/* enable port-1 interrupt */
	cfg |= 0x40;	/* enable scancode translation (set 1 to host) */
	cfg &= ~0x10;	/* clear "disable port-1 clock" */

	if (ctrl_send(PS2_CMD_WRITE_CFG) < 0 || ctrl_write_data(cfg) < 0) {
		printf("ps2: write config failed\n");
		return -1;
	}
	if (ctrl_send(PS2_CMD_ENABLE_P1) < 0) {
		printf("ps2: enable port 1 failed\n");
		return -1;
	}

	/* Tell the keyboard to start sending scancodes.  Some firmwares
	 * left scanning enabled; the command is idempotent. */
	if (ctrl_write_data(KBD_CMD_ENABLE_SCAN) < 0) {
		printf("ps2: enable scan failed\n");
		return -1;
	}
	(void)wait_output_full();
	(void)inb(PS2_DATA);	/* eat the ACK byte (0xFA) */

	if (char_core_irq_register(PS2_IRQ, ps2_irq_handler, p) < 0) {
		printf("ps2: IRQ %u register failed\n", PS2_IRQ);
		return -1;
	}

	p->attached = 1;
	printf("ps2: keyboard attached (IRQ %u)\n", PS2_IRQ);
	return 0;
}

static void
ps2_detach(void *priv)
{
	struct ps2_priv *p = priv;
	unsigned int i;

	(void)char_core_irq_unregister(PS2_IRQ);
	(void)ctrl_send(PS2_CMD_DISABLE_P1);

	for (i = 0; i < p->n_subscribers; i++) {
		if (p->subscribers[i] != MACH_PORT_NULL)
			(void)mach_port_deallocate(mach_task_self(),
						   p->subscribers[i]);
	}
	p->n_subscribers = 0;
	p->attached = 0;
}

static int
ps2_kbd_subscribe(void *priv, mach_port_t notify_port)
{
	struct ps2_priv *p = priv;

	if (p->n_subscribers >= PS2_MAX_SUBSCRIBERS)
		return -1;
	p->subscribers[p->n_subscribers++] = notify_port;
	printf("ps2: subscriber added (port=0x%x, total=%u)\n",
	       (unsigned)notify_port, p->n_subscribers);
	return 0;
}

const char_module_ops_t ps2_module_ops = {
	.name           = "ps2",
	.abi_version    = CHAR_MODULE_ABI_VERSION,
	.priority       = 0,
	.device_class   = CHAR_CLASS_KEYBOARD,
	.probe          = ps2_probe,
	.attach         = ps2_attach,
	.detach         = ps2_detach,
	.kbd_subscribe  = ps2_kbd_subscribe,
	.tty_read       = NULL,
	.tty_write      = NULL,
	.tty_set_attr   = NULL,
	.tty_subscribe  = NULL,
	.mouse_subscribe = NULL,
};
