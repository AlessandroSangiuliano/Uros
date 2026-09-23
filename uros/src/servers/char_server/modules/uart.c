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
 * TX model (#497):
 *   tty_write puts bytes in a ring, fills the transmitter once, arms the
 *   THRE interrupt if anything is left, and returns.  The interrupt refills
 *   one FIFO-full per fire and disarms itself on an empty ring.  On a wire
 *   the kernel still writes (the claim reply says), the fill is instead
 *   "everything the chip takes right now", so a line stays tight.  The only
 *   wait is a full ring, and it yields with the lock released rather than
 *   spinning; after UART_TX_RING_WAITS of that the bytes are dropped,
 *   counted, and the write says so.  No DMA on 16550.  A mutex, not a
 *   flag, serialises the two writers this server now has.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/message.h>
#include <mach/thread_switch.h>	/* #497: a full ring yields, never spins */
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
 * The transmit ring, and how long a writer may wait for room in it (#497).
 *
 * 🔑 THE DRIVER WAS WRITTEN WITH THE CONSOLE'S TECHNIQUE.  It polled THR-empty
 * before every byte -- first a million times, then 4000, then once per FIFO --
 * and every version of that is the right shape for a panic console, where
 * interrupts cannot be relied on, and the wrong one for a driver.  Linux's
 * 8250, FreeBSD's and NetBSD's all do the same thing instead: bytes go into a
 * ring, the transmitter is filled once, and the THRE interrupt refills it as
 * it drains.  The writer never waits for the chip.  In a microkernel that
 * matters more than in Linux, because a userspace thread spinning on a port
 * is burning a scheduling quantum to do nothing.
 *
 * The ring is twice CHR_BUF_MAX so that one MIG write always fits an empty
 * ring (a ring of N holds N-1) with a second in flight behind it.
 *
 * ⚠️ UART_TX_RING_WAITS is the one wait left, and it is a count of YIELDS and
 * not of polls: a writer that finds the ring full gives the CPU up for a
 * millisecond at a time and looks again.  At 115200 baud the chip frees a
 * FIFO-full every 1.4 ms, so 64 tries is some forty FIFOs of wire time -- a
 * ring that stays full that long is a chip that has stopped, and the bytes
 * are then dropped and counted rather than waited for (#551).
 */
#define UART_TX_RING_SIZE	8192u	/* power of two, > CHR_BUF_MAX */
#define UART_TX_RING_MASK	(UART_TX_RING_SIZE - 1u)
#define UART_TX_RING_WAITS	64u
#define UART_TX_YIELD_MS	1

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

	/* How many bytes the transmitter will take without being asked
	 * again, and how many it takes when it is (#497, after #567).
	 * depth is 16 with the FIFO on and 1 without; attach measures it
	 * from IIR rather than assuming, because a FIFO that failed to
	 * enable and a FIFO of one are the same chip to this code. */
	unsigned int	fifo_depth;
	unsigned int	fifo_room;

	/*
	 * The transmit ring (#497).  Producer = tty_write, from whichever
	 * thread calls it; consumer = the pump, from the THRE interrupt on
	 * the dispatch thread or from a writer's own kick.  All of it under
	 * tx_lock, because the two really are different threads now.
	 */
	uint8_t		tx_ring[UART_TX_RING_SIZE];
	uint32_t	tx_head;
	uint32_t	tx_tail;
	int		thre_armed;	/* IER_THRE is set in the chip */
	int		wire_is_ours;	/* the kernel's console stepped back */

	/*
	 * Which path fed the FIFO, and what it cost.  Counted apart for the
	 * same reason rx_by_irq and rx_by_poll are: "bytes went out" and
	 * "the interrupt drove them" are two claims.  tx_waits is how often
	 * a writer found the ring full and yielded; tx_drops is bytes given
	 * up on after UART_TX_RING_WAITS of that.  Both zero on a healthy
	 * boot under qemu, whose transmitter is always ready.
	 */
	uint32_t	tx_by_irq;
	uint32_t	tx_by_kick;
	uint32_t	tx_waits;
	uint32_t	tx_drops;

	/*
	 * A THRE edge that fired while a writer held the ring (#538).
	 *
	 * 🔴 THRE IS A TRANSITION, NOT A LEVEL.  The chip says "transmitter
	 * empty" once, as it becomes so, and says nothing more until THR is
	 * written again.  The interrupt's transmit half cannot take the lock
	 * (see uart_drain) and steps aside -- and the first version stepped
	 * aside in silence, on the belief that the holder's closing kick
	 * would read LSR fresh.  It did, BEFORE the FIFO had drained: no
	 * THRE yet, nothing to fill, unlock.  Then the FIFO drained, the edge
	 * fired into a failed trylock, and with the boot's last lines already
	 * in the ring and nobody left to write, no kick ever came again:
	 * 7626 bytes waiting, chip idle, THRE enabled, wire dead -- read off
	 * the live machine.
	 *
	 * So the edge is RECORDED, before the trylock, and every writer looks
	 * here after releasing: whoever held the lock when the chip emptied
	 * carries the refill.  Set by the dispatch thread, cleared by whoever
	 * refills, ordered so that one of the two always sees it.
	 */
	volatile int	tx_irq_missed;
	uint32_t	tx_by_rescue;	/* bytes a releasing writer sent for such an edge */

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
 * Transmit: a ring, one FIFO per kick, one FIFO per interrupt (#497).
 *
 * 🔑 THE WRITER NEVER WAITS FOR THE CHIP.  tty_write puts its bytes in the
 * ring, fills the transmitter once if it has room, arms the THRE interrupt
 * if anything is left, and returns.  The interrupt refills one FIFO-full
 * each time the transmitter empties and disarms itself when the ring is
 * empty -- a THRE that stays enabled over an empty ring fires for ever.
 * This is serial8250_tx_chars() by another name, and it is what every
 * 16550 driver that is not a panic console does.
 *
 * ⚠️ ONE FIFO PER KICK, NOT "AS MUCH AS THE CHIP TAKES NOW", and the
 * reason is honesty about what a boot can show.  qemu's transmitter is
 * ready again before the write that filled it returns (#567), so a kick
 * that looped on LSR would drain any burst synchronously and the
 * interrupt path would never run under the only machine this is tested
 * on.  Filling once and handing the rest to the interrupt costs a message
 * round trip per sixteen bytes -- noise beside the 80 us each of those
 * bytes costs under KVM -- and makes the path that matters on real
 * hardware the path a boot actually exercises.
 *
 * ⚠️ IER is written from three places and always through uart_ier_write,
 * so that what the chip has is a function of the ring and never of
 * whichever caller wrote it last.
 * ============================================================ */

static void
uart_ier_write(struct uart_priv *p)
{
	uint8_t ier = IER_RXRDY | IER_LSI;

	p->thre_armed = (p->tx_head != p->tx_tail);
	if (p->thre_armed)
		ier |= IER_THRE;
	uart_out(UART_IER, ier);
}

/* Write IER only if the ring's emptiness changed since the chip last
 * heard: an RPC per byte would otherwise become two. */
static void
uart_ier_sync(struct uart_priv *p)
{
	int want = (p->tx_head != p->tx_tail);

	if (want != p->thre_armed)
		uart_ier_write(p);
}

/*
 * Ask the chip once whether it has room, and move what fits.  No loop on
 * LSR: a transmitter that is not ready gets the THRE interrupt, not a
 * second look.  The counter says which path is paying.
 */
static void
uart_tx_kick(struct uart_priv *p, uint32_t *counter)
{
	for (;;) {
		if (p->fifo_room == 0 && p->tx_tail != p->tx_head
		    && (uart_in(UART_LSR) & LSR_THRE))
			p->fifo_room = p->fifo_depth;
		if (p->tx_tail == p->tx_head || p->fifo_room == 0)
			break;

		while (p->tx_tail != p->tx_head && p->fifo_room != 0) {
			uart_out(UART_THR, p->tx_ring[p->tx_tail]);
			p->tx_tail = (p->tx_tail + 1u) & UART_TX_RING_MASK;
			p->fifo_room--;
			(*counter)++;
		}

		/*
		 * 🔑 ONE FIFO WHEN THE WIRE IS OURS, EVERYTHING THE CHIP WILL
		 * TAKE WHEN IT IS NOT -- and the kernel decided which, in the
		 * reply to the claim.  On a wire we own, the rest belongs to
		 * the THRE interrupt and a line spread over interrupts costs
		 * nothing.  On a wire the kernel still writes (i386, until
		 * #544), a line spread over interrupts is a line with gaps
		 * in it, and every gap is a place the other writer lands: the
		 * first boot of interrupt-driven TX there failed the smoke
		 * twice in two, GARBLED, a kernel line inside "hello".  So a
		 * sharer drains while the chip says ready and stops only when
		 * it says not -- the tight lines it always had -- and the
		 * interrupt carries only the tail a slow chip leaves.
		 */
		if (p->wire_is_ours)
			break;
	}

	uart_ier_sync(p);
}

/*
 * The ring is full.  Make room without depending on anybody else, and
 * without spinning.
 *
 * 🔴 IT CANNOT WAIT FOR THE INTERRUPT, and the reason is structural: the
 * THRE interrupt is dispatched by char_server's main thread, and a client's
 * tty_write runs ON that thread.  A writer that parked itself waiting for
 * the interrupt to drain the ring would be waiting for itself.  So it kicks
 * the chip directly -- one LSR read, one FIFO -- and between kicks it gives
 * the CPU up for a millisecond.
 *
 * 🔥 WITH THE LOCK HELD, AND THE FIRST VERSION RELEASED IT.  Releasing it
 * looked like courtesy to the other thread; what it actually did was let
 * the other writer enter the ring in the middle of this one's line -- the
 * kernel's klog and a client's reply woven together at ring granularity,
 * which is #544 rebuilt one layer up in the code that exists to remove it.
 * A line is atomic or the single writer buys nothing.  The other writer
 * sleeps on the mutex; the interrupt's transmit half trylocks and steps
 * aside, leaving a mark that the releasing writer acts on (#538,
 * tx_irq_missed) -- because "whoever holds this lock is pumping the chip
 * already" was true of this loop and false of a writer whose closing kick
 * came before the FIFO drained.
 *
 * Returns 1 when a byte fits, 0 after UART_TX_RING_WAITS tries.
 */
static int
uart_tx_wait_ring(struct uart_priv *p)
{
	unsigned int tries;

	for (tries = 0; tries < UART_TX_RING_WAITS; tries++) {
		uart_tx_kick(p, &p->tx_by_kick);
		if (((p->tx_head + 1u) & UART_TX_RING_MASK) != p->tx_tail)
			return 1;
		p->tx_waits++;
		if (p->tx_waits == 1)
			printf("uart: the transmit ring filled and a writer "
			       "yielded for the first time — the chip is "
			       "slower than what is being said to it "
			       "(#497)\n");
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT,
			      UART_TX_YIELD_MS);
	}
	return 0;
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

	/*
	 * The transmit half (#497), and it runs whether or not a byte came
	 * IN: a THRE interrupt on an idle line is exactly the case with
	 * nothing to receive.  Under tx_lock because the ring is shared with
	 * whichever thread is writing.
	 */
	if (from_irq && p->tx_lock_ready) {
		int first = 0;

		/*
		 * Record the edge FIRST, then try the lock (#538).  trylock
		 * and not lock: a holder is a writer mid-line, and blocking
		 * here would stall the thread that dispatches every interrupt
		 * -- the RX line included -- for the length of that write.
		 * The order is the mechanism: a writer reads the flag after it
		 * unlocks, so a store made before a trylock that fails is seen
		 * by the holder that unlocks after it, and a trylock that
		 * succeeds means there was nobody to tell.  See tx_irq_missed.
		 */
		__atomic_store_n(&p->tx_irq_missed, 1, __ATOMIC_SEQ_CST);
		if (pthread_mutex_trylock(&p->tx_lock) == 0) {
			__atomic_store_n(&p->tx_irq_missed, 0,
					 __ATOMIC_SEQ_CST);
			if (p->tx_tail != p->tx_head) {
				uint32_t before = p->tx_by_irq;

				uart_tx_kick(p, &p->tx_by_irq);
				first = (before == 0 && p->tx_by_irq != 0);
			}
			(void)pthread_mutex_unlock(&p->tx_lock);
		}

		if (first)
			printf("uart: the THRE interrupt refilled the "
			       "transmitter — TX is interrupt-driven on this "
			       "target; %u bytes had left by writers' own "
			       "kicks before it fired (#497)\n",
			       (unsigned)p->tx_by_kick);
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
		natural_t released = 0, klog_from = 0;

		kr = device_io_port_claim(char_core_device_port(), UART_BASE,
					  8u, &released, &klog_from);
		p->wire_is_ours = (kr == KERN_SUCCESS && released != 0);
		if (kr == KERN_SUCCESS)
			char_core_set_wire_owned(p->wire_is_ours,
						 (unsigned int)klog_from);
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

	/*
	 * RX-data and line-status interrupts on; THRE follows the ring, which
	 * is empty here.  This used to say "leave THRE off: we drive TX
	 * polled, so a THRE IRQ would just be noise" -- the console's
	 * technique, written into a driver.
	 */
	p->tx_head = p->tx_tail = 0;
	uart_ier_write(p);

	p->attached = 1;
	printf("uart: COM1 attached @ 115200 8N1 (IRQ %u)\n", UART_IRQ);
	return 0;
}

static void
uart_detach(void *priv)
{
	struct uart_priv *p = priv;
	unsigned int i;

	(void)pthread_mutex_lock(&p->tx_lock);
	uart_out(UART_IER, 0x00);
	p->tx_head = p->tx_tail = 0;	/* what was queued is not going out */
	p->thre_armed = 0;
	p->tx_irq_missed = 0;		/* nothing left for an edge to move */
	(void)pthread_mutex_unlock(&p->tx_lock);
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
 * Release the ring, then carry any THRE edge that fired while it was held
 * (#538): see tx_irq_missed.  A loop, because the refill holds the lock too
 * and an edge can fire during it.
 */
static void
uart_tx_unlock(struct uart_priv *p)
{
	uint32_t before = p->tx_by_rescue;

	(void)pthread_mutex_unlock(&p->tx_lock);
	while (__atomic_exchange_n(&p->tx_irq_missed, 0, __ATOMIC_SEQ_CST)) {
		(void)pthread_mutex_lock(&p->tx_lock);
		uart_tx_kick(p, &p->tx_by_rescue);
		(void)pthread_mutex_unlock(&p->tx_lock);
	}
	if (before == 0 && p->tx_by_rescue != 0)
		printf("uart: a THRE edge fired while a writer held the ring "
		       "and the writer refilled the transmitter on release — "
		       "the edge that used to be lost (#538)\n");
}

static int
uart_tty_write(void *priv, const char *buf, size_t len)
{
	struct uart_priv *p = priv;
	size_t i;
	int dropped = 0;

	if (!p->tx_lock_ready)
		return -1;
	(void)pthread_mutex_lock(&p->tx_lock);

	for (i = 0; i < len; i++) {
		if (((p->tx_head + 1u) & UART_TX_RING_MASK) == p->tx_tail
		    && !uart_tx_wait_ring(p)) {
			/*
			 * Counted and REPORTED to the caller, not absorbed:
			 * a write that returns success for bytes it threw
			 * away is #570's shape from the other side.
			 */
			if (p->tx_drops == 0)
				printf("uart: dropping %u bytes — the ring "
				       "stayed full for %u yields, the chip "
				       "has stopped taking them (#497)\n",
				       (unsigned)(len - i), UART_TX_RING_WAITS);
			p->tx_drops += (uint32_t)(len - i);
			dropped = 1;
			break;
		}
		p->tx_ring[p->tx_head] = (uint8_t)buf[i];
		p->tx_head = (p->tx_head + 1u) & UART_TX_RING_MASK;
	}

	uart_tx_kick(p, &p->tx_by_kick);

	uart_tx_unlock(p);
	return dropped ? -1 : 0;
}

static int
uart_tty_set_attr(void *priv, uint32_t baud, uint32_t data_bits,
		  uint32_t parity, uint32_t stop_bits)
{
	struct uart_priv *p = priv;
	uint32_t divisor;
	uint8_t  lcr = LCR_8N1;

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

	/* Under the lock: the pump may be mid-FIFO on the other thread, and
	 * a divisor change under a byte in flight corrupts that byte. */
	(void)pthread_mutex_lock(&p->tx_lock);
	uart_out(UART_IER, 0x00);
	uart_out(UART_LCR, lcr);
	uart_set_divisor((uint16_t)divisor);
	uart_ier_write(p);	/* THRE back on if the ring is not empty */
	uart_tx_unlock(p);

	return 0;
}

/* ============================================================
 * tty_subscribe — register a wake-up port.
 * ============================================================ */

static int
uart_tty_subscribe(void *priv, mach_port_t notify_port)
{
	struct uart_priv *p = priv;
	unsigned int i, slot = UART_MAX_SUBSCRIBERS;

	/*
	 * #583: a slot freed by the notify loop -- the subscriber's port died
	 * -- is taken again, and a port that is already subscribed is not added
	 * twice.  It used to append at n_subscribers and refuse at the limit,
	 * so the freed slots were never reused: after eight subscriptions in the
	 * life of the boot, nobody could subscribe again.
	 */
	for (i = 0; i < p->n_subscribers; i++) {
		if (p->subscribers[i] == notify_port) {
			/* the same port again: the right it brought is one too many */
			(void)mach_port_deallocate(mach_task_self(), notify_port);
			return 0;
		}
		if (p->subscribers[i] == MACH_PORT_NULL && slot == UART_MAX_SUBSCRIBERS)
			slot = i;
	}
	if (slot == UART_MAX_SUBSCRIBERS) {
		if (p->n_subscribers >= UART_MAX_SUBSCRIBERS)
			return -1;
		slot = p->n_subscribers++;
	}
	p->subscribers[slot] = notify_port;
	printf("uart: subscriber added (port=0x%x, slot %u of %u)\n",
	       (unsigned)notify_port, slot, UART_MAX_SUBSCRIBERS);
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
