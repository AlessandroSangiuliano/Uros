/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * char_server/modules/ps2_mouse.c — PS/2 mouse back-end (#215).
 *
 * Owns port 2 (auxiliary) of the 8042 PS/2 controller and IRQ 12.
 * Talks to the same 0x60/0x64 register pair as ps2.so (keyboard,
 * #206) — they coexist by demultiplexing on the AUX status bit
 * (KBD_STAT_AUX, 0x20): each IRQ handler only reads bytes that
 * belong to it.
 *
 * Activates the IntelliMouse 4-byte protocol via the standard
 * "magic knock" (set sample rate 200, 100, 80) and checks the
 * device ID with command 0xF2.  Falls back to 3-byte packets if
 * the host doesn't acknowledge the IntelliMouse extension —
 * preserves compatibility with the most stripped-down emulators.
 *
 * Fan-out: one inline Mach msg per packet to every subscriber,
 * same pattern as ps2.so.  Subscribers receive char_mouse_event_t
 * with state-on-every-packet semantics; button-press / release
 * edges are derived client-side by diffing against the previous
 * packet.
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
 * 8042 controller registers and commands (mirror ps2.c)
 * ============================================================ */

#define PS2_DATA	0x60
#define PS2_STATUS	0x64	/* read */
#define PS2_CMD		0x64	/* write */

#define PS2_STAT_OBF	0x01
#define PS2_STAT_IBF	0x02
#define PS2_STAT_AUX	0x20

#define PS2_CMD_READ_CFG	0x20
#define PS2_CMD_WRITE_CFG	0x60
#define PS2_CMD_ENABLE_AUX	0xA8
#define PS2_CMD_WRITE_AUX	0xD4	/* next data byte → port 2 (mouse) */

/* Mouse-side commands (sent via WRITE_AUX prefix). */
#define MOUSE_CMD_SET_SAMPLE	0xF3
#define MOUSE_CMD_ENABLE_REPORT	0xF4
#define MOUSE_CMD_GET_DEV_ID	0xF2
#define MOUSE_CMD_SET_DEFAULTS	0xF6
#define MOUSE_ACK		0xFA

#define MOUSE_IRQ		12u

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

/* ============================================================
 * Controller helpers — bounded waits and ack handling.
 * ============================================================ */

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

static int
ctrl_send_cmd(uint8_t cmd)
{
	if (wait_input_empty() < 0)
		return -1;
	outb(PS2_CMD, cmd);
	return 0;
}

static int
ctrl_send_data(uint8_t data)
{
	if (wait_input_empty() < 0)
		return -1;
	outb(PS2_DATA, data);
	return 0;
}

/* Send one byte to the mouse (port 2) and consume its ACK. */
static int
mouse_write_byte(uint8_t b)
{
	uint8_t resp;
	if (ctrl_send_cmd(PS2_CMD_WRITE_AUX) < 0)
		return -1;
	if (ctrl_send_data(b) < 0)
		return -1;
	if (wait_output_full() < 0)
		return -1;
	resp = inb(PS2_DATA);
	return (resp == MOUSE_ACK) ? 0 : -1;
}

/* Send a mouse command + arg, returning -1 on any ACK failure. */
static int
mouse_set_sample_rate(uint8_t rate)
{
	if (mouse_write_byte(MOUSE_CMD_SET_SAMPLE) < 0)
		return -1;
	return mouse_write_byte(rate);
}

/* ============================================================
 * Per-instance state.  Single mouse instance for legacy PS/2.
 * ============================================================ */

#define MOUSE_MAX_SUBSCRIBERS	8

struct ps2_mouse_priv {
	int		attached;
	int		intellimouse;	/* 1 = 4-byte packets, 0 = 3-byte */
	unsigned int	packet_size;	/* 3 or 4 */
	uint8_t		buf[4];
	unsigned int	buf_n;		/* bytes accumulated in `buf` */
	mach_port_t	subscribers[MOUSE_MAX_SUBSCRIBERS];
	unsigned int	n_subscribers;
};

static struct ps2_mouse_priv ps2_mouse_singleton;

/* ============================================================
 * Event fan-out — one inline Mach msg per subscriber per packet.
 *
 * msgh_id 4300 sits outside the existing ranges (gpu 4000,
 * char_server RPCs 4100, ps2 keyboard 4200, IRQ notifications
 * >= 3000) so subscribers can route by id alone.
 * ============================================================ */

#define CHAR_MOUSE_EVENT_MSGH_ID	4300

typedef struct {
	mach_msg_header_t	head;
	NDR_record_t		ndr;
	char_mouse_event_t	event;
} ps2_mouse_event_msg_t;

static void
ps2_mouse_fanout(struct ps2_mouse_priv *p, const char_mouse_event_t *ev)
{
	ps2_mouse_event_msg_t msg;
	unsigned int i;
	mach_msg_return_t mr;

	memset(&msg, 0, sizeof(msg));
	msg.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	msg.head.msgh_size = sizeof(msg);
	msg.head.msgh_id   = CHAR_MOUSE_EVENT_MSGH_ID;
	msg.event = *ev;

	for (i = 0; i < p->n_subscribers; i++) {
		if (p->subscribers[i] == MACH_PORT_NULL)
			continue;
		msg.head.msgh_remote_port = p->subscribers[i];
		mr = mach_msg(&msg.head,
			      MACH_SEND_MSG | MACH_SEND_TIMEOUT,
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
 * Packet decoding.
 *
 * Byte 0:  Y_OVF | X_OVF | Y_SIGN | X_SIGN | 1 | MIDDLE | RIGHT | LEFT
 * Byte 1:  X delta (signed via byte 0 X_SIGN bit)
 * Byte 2:  Y delta (signed via byte 0 Y_SIGN bit)
 * Byte 3:  Z delta (IntelliMouse only — 8-bit signed wheel)
 * ============================================================ */

static int32_t
sign_extend(uint8_t v, int sign_bit)
{
	int32_t s = (int32_t)(int8_t)v;
	(void)sign_bit;	/* byte 0 sign bits are redundant with the high
			 * bit of byte 1/2 once interpreted as int8_t */
	return s;
}

static void
ps2_mouse_handle_packet(struct ps2_mouse_priv *p)
{
	char_mouse_event_t ev;
	uint8_t b0 = p->buf[0];

	/* Drop synthesised "sanity" packets where the always-1 bit
	 * isn't set — likely a stream resync misalignment.  Subsequent
	 * bytes will probably realign as the controller catches up. */
	if ((b0 & 0x08) == 0) {
		p->buf_n = 0;
		return;
	}

	memset(&ev, 0, sizeof(ev));
	ev.dx = sign_extend(p->buf[1], b0 & 0x10);
	ev.dy = sign_extend(p->buf[2], b0 & 0x20);
	/* PS/2 reports +Y = up; flip to match screen convention. */
	ev.dy = -ev.dy;

	if (b0 & 0x01) ev.buttons |= CHAR_MOUSE_BTN_LEFT;
	if (b0 & 0x02) ev.buttons |= CHAR_MOUSE_BTN_RIGHT;
	if (b0 & 0x04) ev.buttons |= CHAR_MOUSE_BTN_MIDDLE;

	if (p->intellimouse) {
		/* Sign-extend the lower 4 bits — Logitech and Microsoft
		 * IntelliMouse Explorer pack button 4/5 in the upper
		 * nibble of byte 3, but standard IntelliMouse leaves
		 * those zero.  Use the 4-bit signed form to be robust. */
		int8_t z = (int8_t)(p->buf[3] & 0x0F);
		if (z & 0x08) z |= (int8_t)0xF0;
		ev.wheel = (int32_t)z;
	}

#ifdef PS2_MOUSE_DEBUG_TRACE
	printf("ps2_mouse: dx=%+d dy=%+d btn=0x%x wheel=%+d\n",
	       (int)ev.dx, (int)ev.dy, (unsigned)ev.buttons, (int)ev.wheel);
#endif

	ps2_mouse_fanout(p, &ev);
}

/* ============================================================
 * IRQ handler — drains AUX bytes from the 8042 OBF queue.
 *
 * IRQ 12 only fires when port 2 has data, but the FIFO is shared
 * with the keyboard side: it is theoretically possible to see a
 * KBD byte here if a keystroke landed simultaneously.  Demux on
 * the STAT_AUX bit, leave KBD bytes alone so ps2.so's IRQ 1
 * handler can pick them up on its own pass.
 * ============================================================ */

static void
ps2_mouse_irq_handler(void *arg)
{
	struct ps2_mouse_priv *p = arg;
	unsigned int budget;

	for (budget = 0; budget < 32u; budget++) {
		uint8_t status = inb(PS2_STATUS);
		uint8_t b;

		if ((status & PS2_STAT_OBF) == 0)
			break;
		if ((status & PS2_STAT_AUX) == 0)
			break;	/* leave KBD byte for ps2.so */

		b = inb(PS2_DATA);
#ifdef PS2_MOUSE_DEBUG_TRACE
		printf("ps2_mouse: rx 0x%02x\n", (unsigned)b);
#endif

		if (p->buf_n < p->packet_size)
			p->buf[p->buf_n++] = b;

		if (p->buf_n >= p->packet_size) {
			ps2_mouse_handle_packet(p);
			p->buf_n = 0;
		}
	}
}

/* ============================================================
 * Probe + attach.
 * ============================================================ */

static void *
ps2_mouse_probe(const struct hal_device_info *dev)
{
	(void)dev;
	if (ps2_mouse_singleton.attached)
		return NULL;
	/* No reliable detection without poking the controller; defer
	 * the real "does this 8042 have a mouse?" check to attach,
	 * which can return -1 if the device IDs come back wrong. */
	return &ps2_mouse_singleton;
}

static int
mouse_try_intellimouse_knock(void)
{
	uint8_t id;

	/* Sample-rate sequence 200, 100, 80 — universally recognised
	 * by IntelliMouse-compatible firmware as the "switch to 4-byte
	 * mode" knock.  Each step ACKed individually. */
	if (mouse_set_sample_rate(200) < 0) return -1;
	if (mouse_set_sample_rate(100) < 0) return -1;
	if (mouse_set_sample_rate(80)  < 0) return -1;

	/* Ask the device for its ID.  0x03 = IntelliMouse (wheel),
	 * 0x04 = IntelliMouse Explorer (5-button + wheel),
	 * 0x00 = standard 2/3-button PS/2 mouse. */
	if (mouse_write_byte(MOUSE_CMD_GET_DEV_ID) < 0)
		return -1;
	if (wait_output_full() < 0)
		return -1;
	id = inb(PS2_DATA);
	return (id == 0x03 || id == 0x04) ? 1 : 0;
}

static int
ps2_mouse_attach(void *priv)
{
	struct ps2_mouse_priv *p = priv;
	uint8_t cfg;
	int wheel;

	/* Enable auxiliary device on the controller. */
	if (ctrl_send_cmd(PS2_CMD_ENABLE_AUX) < 0) {
		printf("ps2_mouse: enable_aux command failed\n");
		return -1;
	}

	/* Read controller config, enable IRQ 12 (bit 1) + clear aux
	 * clock-disable (bit 5), write back. */
	if (ctrl_send_cmd(PS2_CMD_READ_CFG) < 0 || wait_output_full() < 0) {
		printf("ps2_mouse: read cfg failed\n");
		return -1;
	}
	cfg = inb(PS2_DATA);
	cfg |=  0x02;	/* enable IRQ 12 */
	cfg &= ~0x20;	/* clear aux clock disable */
	if (ctrl_send_cmd(PS2_CMD_WRITE_CFG) < 0 ||
	    ctrl_send_data(cfg) < 0) {
		printf("ps2_mouse: write cfg failed\n");
		return -1;
	}

	/* Restore defaults on the mouse side before the magic knock —
	 * gets rid of any stale sample-rate / scaling from BIOS. */
	(void)mouse_write_byte(MOUSE_CMD_SET_DEFAULTS);

	/* Try IntelliMouse 4-byte mode.  Fall back to 3-byte on
	 * failure (e.g. emulators that respond to F2 with 0x00). */
	wheel = mouse_try_intellimouse_knock();
	if (wheel < 0) {
		printf("ps2_mouse: knock sequence failed, no mouse?\n");
		return -1;
	}
	p->intellimouse = wheel ? 1 : 0;
	p->packet_size  = wheel ? 4u : 3u;

	/* Final sample rate at a sane value (60 Hz default = reasonable
	 * compromise between latency and bus pressure). */
	(void)mouse_set_sample_rate(60);

	/* Enable data reporting. */
	if (mouse_write_byte(MOUSE_CMD_ENABLE_REPORT) < 0) {
		printf("ps2_mouse: enable report failed\n");
		return -1;
	}

	if (char_core_irq_register(MOUSE_IRQ, ps2_mouse_irq_handler, p) < 0) {
		printf("ps2_mouse: IRQ %u register failed\n", MOUSE_IRQ);
		return -1;
	}

	p->attached = 1;
	printf("ps2_mouse: mouse attached (IRQ %u, %s)\n",
	       MOUSE_IRQ,
	       p->intellimouse ? "IntelliMouse 4-byte + wheel"
				: "standard 3-byte");
	return 0;
}

static void
ps2_mouse_detach(void *priv)
{
	struct ps2_mouse_priv *p = priv;
	unsigned int i;

	(void)char_core_irq_unregister(MOUSE_IRQ);

	for (i = 0; i < p->n_subscribers; i++) {
		if (p->subscribers[i] != MACH_PORT_NULL)
			(void)mach_port_deallocate(mach_task_self(),
						   p->subscribers[i]);
	}
	p->n_subscribers = 0;
	p->attached = 0;
}

static int
ps2_mouse_subscribe(void *priv, mach_port_t notify_port)
{
	struct ps2_mouse_priv *p = priv;

	if (p->n_subscribers >= MOUSE_MAX_SUBSCRIBERS)
		return -1;
	p->subscribers[p->n_subscribers++] = notify_port;
	printf("ps2_mouse: subscriber added (port=0x%x, total=%u)\n",
	       (unsigned)notify_port, p->n_subscribers);
	return 0;
}

/* ============================================================
 * Module ops
 * ============================================================ */

const char_module_ops_t ps2_mouse_module_ops = {
	.name            = "ps2_mouse",
	.abi_version     = CHAR_MODULE_ABI_VERSION,
	.priority        = 0,
	.device_class    = CHAR_CLASS_MOUSE,
	.probe           = ps2_mouse_probe,
	.attach          = ps2_mouse_attach,
	.detach          = ps2_mouse_detach,
	.kbd_subscribe   = NULL,
	.tty_read        = NULL,
	.tty_write       = NULL,
	.tty_set_attr    = NULL,
	.tty_subscribe   = NULL,
	.mouse_subscribe = ps2_mouse_subscribe,
};
