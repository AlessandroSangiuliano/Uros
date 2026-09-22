/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * char_server/modules/uart.c — 16550 UART back-end (#207).
 *
 * Owns COM1 (I/O 0x3F8, IRQ 4) for runtime userspace serial I/O.
 *
 * 🔴 COEXISTENCE WITH THE KERNEL — AND THE SENTENCE THAT USED TO BE HERE.
 *
 * It read:
 *
 *	"Therefore kernel printf and userspace TX/RX coexist on the same
 *	 UART without collision: writes only touch THR (after waiting
 *	 THR-empty), reads only touch RHR."
 *
 * The READS half is true, and it is what makes the split below work: the
 * kernel's console on i386 never reads RHR, so reception is this module's
 * alone without anybody arranging it.
 *
 * The WRITES half is false, and #544 is the specimen cabinet.  Two writers
 * that both wait for THR-empty and then write THR do not miss each other --
 * they interleave a byte at a time, because "wait, then write" is two steps
 * and neither holds anything across them.  Thirteen failures in fifty-three
 * runs of the i386 acceptance smoke, every one of them two lines woven
 * together inside a word.  A comment that asserts the property the code lacks
 * is not documentation, it is cover.
 *
 * So the coexistence is ARRANGED rather than assumed (#497).  uart_attach()
 * claims the port through the device master, and the kernel's console gives
 * it up at that instant -- writing its bytes to the outputs it still has, the
 * klog ring that has held every byte since #200 and the framebuffer console
 * #568 gave this target.  One writer touches the chip.
 *
 * ⚠️ The dying machine is the exception, and it is stated rather than raced:
 * a panic takes the port back, because a message that arrives possibly
 * garbled beats a message that is lost.
 *
 * ⚠️ i386 keeps both writers for now.  Its console is com.c and reaches the
 * chip with its own instruction, so the claim below does not touch it; that
 * is #544's to close, and this comment is not the place to pretend otherwise.
 *
 * RX model:
 *   Per-IRQ batch: drain RX FIFO into a 4 KB power-of-two ring,
 *   then send ONE header-only Mach msg (CHAR_TTY_NOTIFY_ID) to every
 *   subscriber.  Subscribers wake up and call tty_read to drain.  No
 *   per-byte fan-out — would just multiply IPC cost for no benefit.
 *
 * TX model:
 *   tty_write asks the transmitter for room once per FIFO-full and writes
 *   the bytes one at a time.  No DMA on 16550.  A
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

#include <pthread.h>

#include <char/char_module_abi.h>
#include <char/char_types.h>

/*
 * MIG: device_io_port_{read,write} and device_io_port_{claim,unclaim}.
 *
 * ⚠️ Outside the arch guard below, because the CLAIM is wanted on both
 * targets even though only one of them reaches the chip this way.  What it
 * buys on i386 is narrower and is stated where it is implemented: it stops a
 * second TASK from reaching the range, and it does not stop the kernel, whose
 * console there writes the port with its own instruction.
 */
#include "device_master.h"

/* ============================================================
 * 16550 register layout (offsets from base 0x3F8).
 * ============================================================ */

#define UART_BASE	0x3F8u
#define UART_IRQ	4u

/*
 * How many times one byte may ask the transmitter for room (#497).
 *
 * 🔥 THIS WAS 1000000, WHICH IS NOT A BOUND, IT IS A NUMBER.  #551 is the
 * same defect one layer down -- an unbounded wait on this chip -- and the
 * kernel's console answered it with 4000 polls, reasoned at cons.h: a byte at
 * 115200 baud takes 87 us on the wire and a port poll about one, so a healthy
 * byte polls near 90 times and the bound is forty of those.  The chip is the
 * same chip; there was no reason for the driver's number to be 250 times the
 * kernel's except that nobody had picked it.
 *
 * ⚠️ And on x86-64 it stopped being free.  Each poll is now an RPC (see the
 * accessors below), so the old value was up to a MILLION round trips per
 * byte -- a stall that would have looked like a hang and measured like one.
 *
 * ⚠️ i386 changes with it, from 1000000 to 4000.  Its polls are `inb' and
 * cost differently, but the reasoning above is about the UART and not about
 * the instruction, and one number with one reason beats two with none.
 */
#define UART_TX_POLLS	4000u

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

/* IIR bits (read) */
#define IIR_FIFO_ON	0xC0u	/* both bits: a 16550 with a working FIFO */

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
 * Reaching the chip, which is not the same question on both targets (#497).
 *
 * 🔑 AN x86 I/O PORT IS NOT MEMORY.  A driver for a PCI device maps its BAR
 * and reads it with ordinary loads -- ahci.so does exactly that, and the
 * IOMMU polices the DMA coming back.  COM1 is behind no BAR: it lives in the
 * separate 16-bit I/O address space, reachable only by `in' and `out', which
 * are privileged.  No page table can map it and no IOMMU can see it.
 *
 * So "the driver touches the hardware directly" is true of everything with a
 * BAR and cannot be made true of this one.  What is left is WHO executes the
 * instruction, and the two targets answer differently because they were given
 * different things to answer with:
 *
 *   i386      has the `iopl' device and a per-thread I/O permission bitmap in
 *             the TSS.  char_server opens that device at startup (main.c) and
 *             the kernel grants 0x3F8-0x3FF -- AT386/iopl.c names uart.so in
 *             the comment beside the entry.  The instruction is ours.
 *
 *   x86-64    has neither: no iopl device in the device table, iomap_base set
 *             past the end of the TSS (cpu/desc.c), no trap-and-emulate.  An
 *             `outb' from here is a #GP, which arrives as EXC_BAD_INSTRUCTION
 *             and kills the task -- measured, in uart_probe's scratch test,
 *             before attach ever ran.
 *
 * So on x86-64 the kernel executes it, through the pair virtio_blk.so has
 * used since it crossed: device_io_port_read/write on the device master port.
 * It is a trap, a range check and the instruction -- no second task, no
 * scheduling -- so it is a system call wearing a message's clothes, which is
 * the shape seL4 gives the same problem.
 *
 * ⚠️ A BITMAP WOULD HAVE BEEN THE OTHER ANSWER, and it was not chosen for
 * cost.  It is what GNU Mach, Fiasco and Linux's ioperm() do, and long mode
 * kept the hardware for it.  What decided against it is that a bitmap, once
 * granted, is read by the CPU and never again by the kernel: taking it back
 * means editing the task's copy and interrupting every processor that might
 * hold it.  This system already has revocation -- #511 made a device a right,
 * cap_server hands it out and char_server is subscribed to the notification --
 * and a capability simply stops working when it is revoked.
 *
 * ⚠️ AND IT IS NOT A SPLIT WE HAD TO MAKE.  The RPC works on i386 too: the
 * kernel half is machine-independent and both targets supply device_md_io_*.
 * i386 keeps its instruction because it is the mature target and its
 * acceptance suite is the only net the shared code has -- not because the
 * other road is closed there.
 * ============================================================ */

#if defined(__x86_64__)

/*
 * A refused access reads as an absent chip, and says so once.
 *
 * 🔥 virtio_blk.so does NOT do this: vio_read32/16/8 discard the
 * kern_return_t and return a stack variable the MIG stub never wrote when the
 * call failed, so a refusal there is read as data.  An absent 16550 reads
 * 0xFF on every register, which is a value this driver already knows how to
 * disbelieve -- the scratch test rejects it -- so degrading to it turns a
 * refusal into "no device" instead of into whatever was on the stack.
 */
static unsigned int	uart_rpc_refusals;

static void
uart_rpc_refused(const char *what, uint16_t off, kern_return_t kr)
{
	uart_rpc_refusals++;
	if (uart_rpc_refusals == 1)
		printf("uart: %s of 0x%x refused by the kernel (kr=%d) — "
		       "COM1 reads as an absent chip from here on\n",
		       what, (unsigned)(UART_BASE + off), (int)kr);
}

static inline uint8_t uart_in(uint16_t off)
{
	natural_t	v = 0xFFu;
	kern_return_t	kr;

	kr = device_io_port_read(char_core_device_port(),
				 (natural_t)(UART_BASE + off), 1, &v);
	if (kr != KERN_SUCCESS) {
		uart_rpc_refused("read", off, kr);
		return 0xFFu;
	}
	return (uint8_t)v;
}

static inline void uart_out(uint16_t off, uint8_t v)
{
	kern_return_t kr;

	kr = device_io_port_write(char_core_device_port(),
				  (natural_t)(UART_BASE + off), 1,
				  (natural_t)v);
	if (kr != KERN_SUCCESS)
		uart_rpc_refused("write", off, kr);
}

#elif defined(__i386__)

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

#else
#error "uart.so: this target has no stated way to reach a 16550"
#endif

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

	/*
	 * 🔥 TX SERIALISATION, AND IT USED TO BE AN int (#497).
	 *
	 * The comment here said the server was single-threaded "today" and
	 * that the flag was cheap insurance against a future re-entrant
	 * caller.  It was not insurance: `if (p->tx_busy) return -1;' followed
	 * by `p->tx_busy = 1;' is a test and a set with nothing between them,
	 * so two threads read zero and both proceed.
	 *
	 * The future arrived in this issue.  The klog forwarder writes the
	 * kernel's output through this same entry point while a client may be
	 * writing its own, and the specimen was immediate and familiar:
	 *
	 *	device: task ... died holding DMA region 8 (4096 bytes,
	 *	lent to 0 devices, owner 0char_test: [1] this line left...
	 *
	 * which is #544's shape exactly -- two writers woven inside a word --
	 * arriving in the server that exists to make it impossible.  A real
	 * lock is what the property needs, and a flag that looked like one is
	 * why nobody noticed it was missing.
	 */
	pthread_mutex_t	tx_lock;
	int		tx_lock_ready;
	int		wire_is_ours;	/* the kernel's console stepped back (#497) */

	/* How many bytes the transmitter will take without being asked
	 * again, and how many it takes when it is (#497, after #567).
	 * depth is 16 with the FIFO on and 1 without; attach measures it
	 * from IIR rather than assuming, because a FIFO that failed to
	 * enable and a FIFO of one are the same chip to this code. */
	unsigned int	fifo_depth;
	unsigned int	fifo_room;

	/* How many times a byte exhausted UART_TX_POLLS waiting for room.
	 * #551's lesson: a stall that nobody counted is a stall nobody can
	 * be shown.  Zero on every healthy boot. */
	uint32_t	tx_stalls;

	/* Which path carried a received byte (#497).  The IRQ is the one
	 * this driver is built around; the read-path peek below is #382's
	 * defence against a lost edge.  They are counted apart because
	 * "input works" and "the interrupt works" are two claims, and a
	 * test that reads a byte proves only the first. */
	uint32_t	rx_by_irq;
	uint32_t	rx_by_poll;

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

/* Declared in <mach/mach_traps.h>, already included above (#426). */

static void
uart_drain(struct uart_priv *p, int from_irq)
{
	unsigned int budget;
	int got_any = 0;

#ifdef UART_DEBUG_TRACE	/* set via -DUART_DEBUG_TRACE for bring-up */
	mach_print("uart: IRQ fired\n");
#endif
	for (budget = 0; budget < 64u; budget++) {
		uint8_t lsr = uart_in(UART_LSR);
		if ((lsr & LSR_DR) == 0)
			break;
		{
			uint8_t b = uart_in(UART_RHR);
			uint32_t next = (p->ring_head + 1u) & UART_RING_MASK;

			/*
			 * #382: Ctrl+D is the kernel-debugger break key.
			 * The kernel can't see our RX bytes (we own COM1),
			 * so report it; the RPC returns 0 only when the -K
			 * boot flag armed the break — the byte is then
			 * consumed by the debugger session.  Otherwise fall
			 * through and deliver it as ordinary input (EOF).
			 */
			if (b == 0x04 && char_core_ddb_break() == 0)
				continue;

			/*
			 * #397: ^C / ^Z generate a signal for the foreground
			 * process group and are not delivered as input.
			 */
			if (char_tty_input_isig(p, b))
				continue;

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

	if (!got_any)
		return;

	/*
	 * Say it once, per path.  #497 needs to know whether IRQ 4 is
	 * DELIVERED on this target and not merely registered, and the two
	 * are not the same fact: the peek in uart_tty_read() would carry
	 * every byte on its own and the input would look identical.
	 */
	if (from_irq) {
		p->rx_by_irq++;
		if (p->rx_by_irq == 1)
			printf("uart: IRQ 4 delivered a byte — the interrupt "
			       "path is live on this target (#497)\n");
	} else {
		p->rx_by_poll++;
		if (p->rx_by_poll == 1)
			printf("uart: a byte arrived that no interrupt "
			       "announced; the read-path peek took it "
			       "(#382)\n");
	}

	uart_notify_subscribers(p);
}

static void
uart_irq_handler(void *arg)
{
	uart_drain(arg, 1);
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

	/*
	 * Armed here and not in attach, because attach is where the first
	 * bytes can already be going out.
	 */
	if (!uart_singleton.tx_lock_ready) {
		if (pthread_mutex_init(&uart_singleton.tx_lock, NULL) != 0)
			return NULL;
		uart_singleton.tx_lock_ready = 1;
	}
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
	kern_return_t	 kr;

	/*
	 * Say the port is ours BEFORE touching a register that matters (#497).
	 *
	 * 🔑 The order is the whole point.  Everything below reprograms the
	 * chip -- the divisor, the line, both FIFOs, the modem lines -- and on
	 * x86-64 the kernel's console is writing it until this call returns.
	 * Claiming afterwards would mean programming a 16550 somebody else was
	 * mid-sentence on, which is #544 with the roles swapped.
	 *
	 * ⚠️ A refusal is fatal to the attach and is not worked around.  It
	 * means another task holds this range, and a driver that went ahead
	 * anyway would be the second writer this issue exists to remove.
	 */
	{
		natural_t released = 0;

		kr = device_io_port_claim(char_core_device_port(), UART_BASE,
					  8u, &released);
		p->wire_is_ours = (kr == KERN_SUCCESS && released != 0);
		if (kr == KERN_SUCCESS)
			char_core_set_wire_owned(p->wire_is_ours);
	}
	if (kr != KERN_SUCCESS) {
		printf("uart: COM1 0x%x..0x%x refused (kr=%d) — another task "
		       "holds it; not attaching\n",
		       UART_BASE, UART_BASE + 7u, (int)kr);
		return -1;
	}

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

	/* How deep the transmitter is, ASKED rather than assumed (#497).
	 * The FCR write above requests a FIFO; IIR is where the chip says
	 * whether it has one.  A 16450, or a 16550 with the broken FIFO
	 * that made the A revision famous, answers 1 -- and then every byte
	 * polls, which is correct and slow instead of fast and wrong. */
	p->fifo_depth = ((uart_in(UART_IIR) & IIR_FIFO_ON) == IIR_FIFO_ON)
			? 16u : 1u;
	p->fifo_room  = 0u;

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

	/*
	 * Give the range back LAST (#497), for attach's reason inverted: the
	 * writes above are still this driver's to make, and on x86-64 the
	 * kernel's console resumes the instant this returns.  Releasing first
	 * would put two writers on the chip for the length of a detach.
	 */
	(void)device_io_port_unclaim(char_core_device_port(), UART_BASE);
}

/* ============================================================
 * tty_read — drain the ring (non-blocking).
 * ============================================================ */

static int
uart_tty_read(void *priv, char *buf, size_t max, size_t *out_len)
{
	struct uart_priv *p = priv;
	size_t n = 0;

	/*
	 * #382: opportunistic FIFO drain when the ring is empty.  An
	 * edge-triggered IRQ 4 front can be lost for good whenever
	 * delivery was impossible at the instant the RX line rose (CPUs
	 * parked at IF=0 during a DDB session being the proven case) —
	 * the 16550 then holds INT asserted with a full FIFO and never
	 * fires again.  Readers poll this entry point continuously
	 * anyway (the tty read poll), so one LSR peek per empty-ring
	 * read resurrects the line within a poll period no matter what
	 * ate the front.  Defense in depth on top of the #381 kernel fix.
	 */
	if (p->ring_tail == p->ring_head && (uart_in(UART_LSR) & LSR_DR))
		uart_drain(p, 0);

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

/*
 * Wait until the transmitter will take at least one byte, and record how
 * many it will take (#497).
 *
 * 🔑 THE POLL WAS PER BYTE AND THE FIFO IS SIXTEEN DEEP.  When LSR says
 * THR-empty on a chip whose FIFO is on, the whole FIFO is empty, so sixteen
 * bytes may go in before the question is worth asking again.  Asking once a
 * byte was merely wasteful while a poll was an `inb'; on x86-64 a poll is an
 * RPC, and an 80-byte line went from 160 kernel entries to 85.
 *
 * ⚠️ This is the kernel's own bookkeeping from #567 (cons_fifo_room), on the
 * same chip, for the same reason.  It is duplicated rather than shared
 * because the two writers must not share state -- that is the whole of what
 * #497 is about -- but the arithmetic had better agree.
 *
 * ⚠️ On the bound running out, room is left at ONE.  Not zero, which would
 * poll again for the next byte and turn a stall into a stall per byte; and
 * not depth, which would claim room the chip never reported.  The byte goes
 * out into a transmitter that may not be ready -- one lost byte on a port
 * that is already misbehaving, and the count below says it happened.
 */
static void
uart_tx_wait_room(struct uart_priv *p)
{
	unsigned int polls;

	for (polls = 0; polls < UART_TX_POLLS; polls++) {
		if (uart_in(UART_LSR) & LSR_THRE) {
			p->fifo_room = p->fifo_depth;
			return;
		}
	}

	p->tx_stalls++;
	p->fifo_room = 1u;
}

static int
uart_tty_write(void *priv, const char *buf, size_t len)
{
	struct uart_priv *p = priv;
	size_t i;

	if (!p->tx_lock_ready)
		return -1;
	(void)pthread_mutex_lock(&p->tx_lock);

	for (i = 0; i < len; i++) {
		if (p->fifo_room == 0)
			uart_tx_wait_room(p);
		uart_out(UART_THR, (uint8_t)buf[i]);
		p->fifo_room--;
	}

	(void)pthread_mutex_unlock(&p->tx_lock);
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
	.mouse_subscribe = NULL,
};
