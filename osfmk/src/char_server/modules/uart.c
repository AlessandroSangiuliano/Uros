/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * char_server/modules/uart.c — 16550 UART back-end (#207).
 *
 * Owns COM1 (I/O 0x3F8, IRQ 4) for runtime userspace serial I/O.
 *
 * Coexistence with the kernel:
 *   The kernel keeps a polled-write COM1 path in com.c (cnputc →
 *   com_putc) that is used for printf, panic, and early boot output.
 *   That path only writes — never enables IER, never reads RHR.  This
 *   module owns reception, IRQ 4, and runtime configuration; it does
 *   not change the divisor away from the kernel's setting unless
 *   tty_set_attr is called by an admin client.  Therefore kernel
 *   printf and userspace TX/RX coexist on the same UART without
 *   collision: writes only touch THR (after waiting THR-empty), reads
 *   only touch RHR.
 *
 * RX model:
 *   Per-IRQ batch: drain RX FIFO into a 4 KB power-of-two ring,
 *   then send ONE header-only Mach msg (CHAR_TTY_NOTIFY_ID) to every
 *   subscriber.  Subscribers wake up and call tty_read to drain.  No
 *   per-byte fan-out — would just multiply IPC cost for no benefit.
 *
 * TX model:
 *   tty_write polls THR-empty, byte-by-byte.  No DMA on 16550.  A
 *   per-instance flag prevents concurrent writers (the server is
 *   single-threaded today, but the flag is cheap insurance against
 *   future re-entrancy if char_server ever spawns helper threads).
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
 * 16550 register layout (offsets from base 0x3F8).
 * ============================================================ */

#define UART_BASE	0x3F8u
#define UART_IRQ	4u

#define UART_RHR	0u	/* DLAB=0: receive holding		*/
#define UART_THR	0u	/* DLAB=0: transmit holding		*/
#define UART_DLL	0u	/* DLAB=1: divisor latch low		*/
#define UART_IER	1u	/* interrupt enable			*/
#define UART_DLM	1u	/* DLAB=1: divisor latch high		*/
#define UART_IIR	2u	/* interrupt identification (read)	*/
#define UART_FCR	2u	/* FIFO control (write)			*/
#define UART_LCR	3u	/* line control				*/
#define UART_MCR	4u	/* modem control			*/
#define UART_LSR	5u	/* line status				*/
#define UART_MSR	6u	/* modem status				*/
#define UART_SCR	7u	/* scratch (loopback test)		*/

/* IER bits */
#define IER_RXRDY	0x01u	/* received data available */
#define IER_THRE	0x02u	/* transmitter holding empty */
#define IER_LSI		0x04u	/* line status interrupt */

/* FCR bits */
#define FCR_ENABLE	0x01u
#define FCR_RX_RESET	0x02u
#define FCR_TX_RESET	0x04u
#define FCR_TRIG_1	0x00u	/* RX trigger: 1 byte (low latency) */

/* LCR bits */
#define LCR_8N1		0x03u	/* 8 data, no parity, 1 stop */
#define LCR_DLAB	0x80u

/* LSR bits */
#define LSR_DR		0x01u	/* data ready (RHR has a byte) */
#define LSR_OE		0x02u	/* overrun error */
#define LSR_THRE	0x20u	/* THR empty (can write) */

/* MCR bits */
#define MCR_DTR		0x01u
#define MCR_RTS		0x02u
#define MCR_OUT2	0x08u	/* must be 1 for IRQ to leave the chip */

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

static inline uint8_t uart_in(uint16_t off)
{
	return inb((uint16_t)(UART_BASE + off));
}

static inline void uart_out(uint16_t off, uint8_t v)
{
	outb((uint16_t)(UART_BASE + off), v);
}

/* ============================================================
 * Per-instance state.  Single COM1 instance.
 * ============================================================ */

#define UART_RING_SIZE		4096u	/* must be power of two */
#define UART_RING_MASK		(UART_RING_SIZE - 1u)
#define UART_MAX_SUBSCRIBERS	8

struct uart_priv {
	int		attached;

	/* RX ring buffer.  Producer = irq_handler (single thread,
	 * the bottom-half kthread reaches us via mach_msg → demux),
	 * consumer = tty_read (the same dispatch thread).  Single
	 * thread on both ends → no locking needed. */
	uint8_t		ring[UART_RING_SIZE];
	uint32_t	ring_head;	/* producer write index */
	uint32_t	ring_tail;	/* consumer read index */
	uint32_t	overrun_drops;	/* bytes lost when ring full */

	/* TX serialization: the server is single-threaded today, but
	 * mark "in tty_write" anyway so a future re-entrant caller
	 * can't interleave bytes mid-string. */
	int		tx_busy;

	/* Subscribers receive a header-only wake-up per RX batch. */
	mach_port_t	subscribers[UART_MAX_SUBSCRIBERS];
	unsigned int	n_subscribers;
};

static struct uart_priv uart_singleton;

/* ============================================================
 * Subscriber wake-up: one header-only Mach msg per RX batch.
 * ============================================================ */

static void
uart_notify_subscribers(struct uart_priv *p)
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
 * IRQ handler: drain RX FIFO into the ring, then notify.
 *
 * Called from the char_server demux thread when an IRQ-4
 * notification arrives on the UART's port (the kernel side runs
 * the bottom-half kthread that turns the raw IRQ into a Mach msg
 * — see device_master.c #206 fix).
 * ============================================================ */

static void
uart_irq_handler(void *arg)
{
	struct uart_priv *p = arg;
	unsigned int budget;
	int got_any = 0;

	for (budget = 0; budget < 64u; budget++) {
		uint8_t lsr = uart_in(UART_LSR);
		if ((lsr & LSR_DR) == 0)
			break;
		{
			uint8_t b = uart_in(UART_RHR);
			uint32_t next = (p->ring_head + 1u) & UART_RING_MASK;
			if (next == p->ring_tail) {
				p->overrun_drops++;
			} else {
				p->ring[p->ring_head] = b;
				p->ring_head = next;
				got_any = 1;
			}
#ifdef UART_DEBUG_TRACE	/* set via -DUART_DEBUG_TRACE for bring-up */
			printf("uart: rx 0x%02x\n", (unsigned)b);
#endif
		}
		if (lsr & LSR_OE) {
			/* Hardware-level overrun (FIFO lost a byte
			 * before we got here).  Count separately from
			 * software ring overflow so a future stat MIG
			 * can split them — for #207 we just bump the
			 * same counter. */
			p->overrun_drops++;
		}
	}

	if (got_any)
		uart_notify_subscribers(p);
}

/* ============================================================
 * Probe: scratch-register loopback.  Verifies an actual 16450/16550
 * is wired at 0x3F8 — refuse to claim the device on a host that
 * doesn't emulate COM1.
 * ============================================================ */

static int
uart_scratch_test(void)
{
	uart_out(UART_SCR, 0x55);
	if (uart_in(UART_SCR) != 0x55)
		return -1;
	uart_out(UART_SCR, 0xAA);
	if (uart_in(UART_SCR) != 0xAA)
		return -1;
	return 0;
}

static void *
uart_probe(const struct hal_device_info *dev)
{
	(void)dev;
	if (uart_singleton.attached)
		return NULL;
	if (uart_scratch_test() < 0)
		return NULL;
	return &uart_singleton;
}

/* ============================================================
 * Attach: program 8N1 @ 115200, enable FIFO + RX IRQ.
 * ============================================================ */

static void
uart_set_divisor(uint16_t div)
{
	uint8_t lcr = uart_in(UART_LCR);
	uart_out(UART_LCR, lcr | LCR_DLAB);
	uart_out(UART_DLL, (uint8_t)(div & 0xFFu));
	uart_out(UART_DLM, (uint8_t)((div >> 8) & 0xFFu));
	uart_out(UART_LCR, lcr & (uint8_t)~LCR_DLAB);
}

static int
uart_attach(void *priv)
{
	struct uart_priv *p = priv;

	/* Disable interrupts while we reconfigure. */
	uart_out(UART_IER, 0x00);

	/* 8N1, baud 115200 (divisor 1 with the standard 1.8432 MHz
	 * crystal / 16x clock).  The kernel boot path leaves the UART
	 * at the same setting, so existing kernel printfs continue to
	 * land at the right speed. */
	uart_out(UART_LCR, LCR_8N1);
	uart_set_divisor(1);

	/* Enable + reset both FIFOs, RX trigger 1 byte (low latency). */
	uart_out(UART_FCR, FCR_ENABLE | FCR_RX_RESET | FCR_TX_RESET |
			   FCR_TRIG_1);

	/* DTR + RTS, plus OUT2 — without OUT2 the IRQ stays gated
	 * inside the chip and we'd never see IRQ 4. */
	uart_out(UART_MCR, MCR_DTR | MCR_RTS | MCR_OUT2);

	/* Drain anything stale in RHR / LSR before unmasking IRQ. */
	while (uart_in(UART_LSR) & LSR_DR)
		(void)uart_in(UART_RHR);

	if (char_core_irq_register(UART_IRQ, uart_irq_handler, p) < 0) {
		printf("uart: IRQ %u register failed\n", UART_IRQ);
		return -1;
	}

	/* Enable RX-data and line-status interrupts.  Leave THRE off:
	 * we drive TX polled, so a THRE IRQ would just be noise. */
	uart_out(UART_IER, IER_RXRDY | IER_LSI);

	p->attached = 1;
	printf("uart: COM1 attached @ 115200 8N1 (IRQ %u)\n", UART_IRQ);
	return 0;
}

static void
uart_detach(void *priv)
{
	struct uart_priv *p = priv;
	unsigned int i;

	uart_out(UART_IER, 0x00);
	(void)char_core_irq_unregister(UART_IRQ);

	for (i = 0; i < p->n_subscribers; i++) {
		if (p->subscribers[i] != MACH_PORT_NULL)
			(void)mach_port_deallocate(mach_task_self(),
						   p->subscribers[i]);
	}
	p->n_subscribers = 0;
	p->attached = 0;
}

/* ============================================================
 * tty_read — drain the ring (non-blocking).
 * ============================================================ */

static int
uart_tty_read(void *priv, char *buf, size_t max, size_t *out_len)
{
	struct uart_priv *p = priv;
	size_t n = 0;

	while (n < max && p->ring_tail != p->ring_head) {
		buf[n++] = (char)p->ring[p->ring_tail];
		p->ring_tail = (p->ring_tail + 1u) & UART_RING_MASK;
	}
	*out_len = n;
	return 0;
}

/* ============================================================
 * tty_write — polled THR-empty, byte-by-byte.
 * ============================================================ */

static int
uart_tty_write(void *priv, const char *buf, size_t len)
{
	struct uart_priv *p = priv;
	size_t i;
	unsigned int spins;

	if (p->tx_busy)
		return -1;
	p->tx_busy = 1;

	for (i = 0; i < len; i++) {
		for (spins = 0; spins < 1000000u; spins++) {
			if (uart_in(UART_LSR) & LSR_THRE)
				break;
		}
		uart_out(UART_THR, (uint8_t)buf[i]);
	}

	p->tx_busy = 0;
	return 0;
}

/* ============================================================
 * tty_set_attr — reprogram divisor + LCR.
 *
 * Only baud / 8N1-style framing for #207.  Parity/stop_bits are
 * accepted but baud is the only knob with real consequences for
 * QEMU bring-up.  Reject zero/garbage instead of locking the line.
 * ============================================================ */

static int
uart_tty_set_attr(void *priv, uint32_t baud, uint32_t data_bits,
		  uint32_t parity, uint32_t stop_bits)
{
	uint32_t divisor;
	uint8_t  lcr = LCR_8N1;

	(void)priv;

	if (baud == 0u || baud > 115200u)
		return -1;
	divisor = 115200u / baud;
	if (divisor == 0u || divisor > 0xFFFFu)
		return -1;

	/* For #207 we honour baud and ignore framing knobs (the only
	 * sane setting today is 8N1 — anything else needs a richer
	 * LCR builder we don't have a use for yet). */
	(void)data_bits;
	(void)parity;
	(void)stop_bits;

	uart_out(UART_IER, 0x00);
	uart_out(UART_LCR, lcr);
	uart_set_divisor((uint16_t)divisor);
	uart_out(UART_IER, IER_RXRDY | IER_LSI);

	return 0;
}

/* ============================================================
 * tty_subscribe — register a wake-up port.
 * ============================================================ */

static int
uart_tty_subscribe(void *priv, mach_port_t notify_port)
{
	struct uart_priv *p = priv;

	if (p->n_subscribers >= UART_MAX_SUBSCRIBERS)
		return -1;
	p->subscribers[p->n_subscribers++] = notify_port;
	printf("uart: subscriber added (port=0x%x, total=%u)\n",
	       (unsigned)notify_port, p->n_subscribers);
	return 0;
}

/* ============================================================
 * Module ops
 * ============================================================ */

const char_module_ops_t uart_module_ops = {
	.name           = "uart",
	.abi_version    = CHAR_MODULE_ABI_VERSION,
	.priority       = 0,
	.device_class   = CHAR_CLASS_TTY,
	.probe          = uart_probe,
	.attach         = uart_attach,
	.detach         = uart_detach,
	.kbd_subscribe  = NULL,
	.tty_read       = uart_tty_read,
	.tty_write      = uart_tty_write,
	.tty_set_attr   = uart_tty_set_attr,
	.tty_subscribe  = uart_tty_subscribe,
};
