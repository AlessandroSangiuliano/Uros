/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The debugger's console (#428).
 */

#include <stdarg.h>
#include <stdint.h>

#include <cpu/regs.h>
#include <ddb/cons.h>
#include <kern/lock.h>
#include <time/tsc.h>

#define COM1		0x3F8

#define UART_DATA	0		/* receive when read, transmit when written */
#define UART_LCR	3
#define UART_MCR	4
#define UART_IIR	2	/* read: interrupt id, and whether the FIFO is on */
#define UART_LSR	5

#define IIR_FIFO_ON	0xC0	/* both bits: a 16550 with a working FIFO */

/*
 * How many bytes may be handed over after ONE look at the transmitter (#567).
 *
 * 🔑 THRE MEANS THE FIFO IS EMPTY, NOT THAT ONE BYTE FITS.  boot.S turns the
 * FIFO on -- FCR 0xC7 -- and every writer here then waited for THRE before
 * EVERY byte, which is the worst of both: the sixteen-byte buffer is enabled
 * and the code uses one slot of it, paying a read of the line status register
 * for each byte.  Under KVM that read is an exit to the host, so it was half
 * the cost of the console.
 *
 * ⚠️ PROBED, NOT ASSUMED.  A 16450 has no FIFO and takes one byte at a time;
 * writing sixteen to it drops fifteen.  The 16550 says so in IIR's top two
 * bits after FCR has been written, which is the only way to tell them apart,
 * and a port that does not answer keeps the old depth of one.
 */
#define CONS_FIFO_DEPTH	16

#define LSR_DATA_READY	0x01
#define LSR_THR_EMPTY	0x20

#define MCR_DTR		0x01
#define MCR_RTS		0x02
#define MCR_LOOPBACK	0x10

/*
 * Capture (#415).
 *
 * The formatter below is what panic() prints through, and a formatter nobody
 * can check is a strange thing to put under the one message that gets read
 * when everything else has stopped working.  Checking it means being able to
 * read back what it wrote, so the console can be pointed at memory for the
 * length of a selftest.
 *
 * Deliberately a plain pointer and not a lock: this runs on one CPU during
 * boot, before there is anything to race with, and the alternative -- a
 * second write path used only by tests -- would verify a copy of the code
 * rather than the code.
 */
static void	cons_queue(char c);

static char	*cons_capture_buf;
static unsigned	 cons_capture_len;
static unsigned	 cons_capture_max;

/*
 * ── The transmitter may refuse, and a byte that waits for it forever is a
 *    machine that stops (#551) ──────────────────────────────────────────
 *
 * cons_putc() spun on LSR_THR_EMPTY with no bound.  Around it, printf() holds
 * printf_lock across every byte of a line, with preemption off and -- since
 * #528 -- interrupts masked for the hold; and an unprivileged trap can make
 * the kernel print: kernel_task_create() with an address the map refuses
 * printed a 55-byte diagnostic, 9.6 million cycles on the machine that found
 * it, and a loop over that trap was a program that kept a CPU inside a device
 * spin, under the lock the whole system prints through, for as long as it
 * liked.  If THRE never came at all -- a host whose stdout stopped draining, a
 * UART left mid-state by whoever owns it in userspace -- it never came back.
 *
 * Two bounds, and the second is the one that matters:
 *
 *   1. A byte waits at most CONS_THRE_SPINS polls.  i386's com_putc has done
 *      this since before this port existed.
 *
 *   2. 🔑 AND THE NEXT BYTE DOES NOT WAIT AGAIN.  With (1) alone a stuck
 *      transmitter turns a 55-byte line into 55 times the bound: longer than
 *      the unbounded wait ever was on a healthy port, and still under the
 *      lock.  So one timeout marks the port stuck, and while the mark stands
 *      every byte polls ONCE and is dropped if there is still no room.  The
 *      mark clears itself the first time a poll finds room -- which is the
 *      very next byte to try -- so a port that drains again resumes output at
 *      the byte after the last one lost, with nothing to reset and nobody to
 *      tell.
 *
 * What is lost is bytes on the wire and nothing else: printf() gives every
 * byte to the log ring (klog_putc) as well as to this, so host_get_log still
 * holds the whole line.  How many were lost is counted and said once per boot
 * by cons_cost_report(), because a console that has been dropping output is
 * the first thing a reader of that boot's log needs to know.
 *
 * ⚠️ The three words below are advisory and unlocked on purpose.  printf() and
 * consolewrite() reach here under printf_lock, but panic and the debugger do
 * not and must not; a lost update to a count of dropped bytes costs a number,
 * and a lock here would cost the one path that has to work when locks do not.
 */
static unsigned	cons_tx_stuck;		/* the last byte found no room in its bound */
static unsigned	cons_fifo_depth;	/* 0 until probed; then 16 or 1 */
static unsigned	cons_fifo_room;		/* bytes still writable without looking */
static unsigned	cons_tx_dropped_count;
static unsigned	cons_tx_spins_peak;	/* most polls one byte needed since reset */

/*
 * ── The wire leaves the critical section (#567) ───────────────────────
 *
 * printf() holds printf_lock across every byte of a line, with preemption off
 * and -- since #528 -- interrupts masked for the hold, and the device wait was
 * INSIDE that hold.  A byte costs what the device costs: 83 us under KVM, 87
 * us of wire on the metal at the 115200 baud boot.S now programs.  So a
 * 47-byte line was some four milliseconds of one processor with interrupts
 * off, and an entry-14 boot is 62 163 bytes of it.
 *
 * 🔑 THE LOCK PROTECTS NO DATA STRUCTURE.  It makes a line a line on a wire
 * that every processor shares, which is the whole of its job, and that is why
 * the obvious repair -- render under the lock, emit outside it -- was refused
 * when this was last looked at: two writers' bytes would mix on the wire,
 * which is the shape #544 chased through thirteen red runs.
 *
 * A ring answers that objection rather than arguing with it.  Bytes enter the
 * ring in the order the lock granted, and leave it in ring order, so a line is
 * still a line no matter which processor hands it to the port.  The lock now
 * covers a memory write per byte; the device is outside it.
 *
 * ⚠️ WHAT THIS DOES NOT CLAIM.  It does not make a byte cheaper.  Under an
 * accelerator the cost IS the `outb' -- an exit to the host and a chardev
 * write -- and somebody still pays it; the ring only decides who, and when.
 * What it changes is that the wire is no longer inside a window where this
 * processor cannot be rescheduled and cannot take an interrupt, and that on
 * the metal, where the cost is the wire itself, the transmitter now drains
 * while the kernel works instead of the kernel standing still while it
 * drains.  A boot's total is unchanged on an accelerator and must be: a
 * measurement that showed one getting faster would be measuring something
 * else.
 *
 * ── Two locks, and why that is not one too many ───────────────────────
 *
 * cons_ring_lock covers the two indices and nothing else, so a writer holds
 * it for one store.  cons_tx_lock covers the port and, more to the point, the
 * ORDER of bytes on it: a drainer holds it across taking a byte OUT of the
 * ring and writing that byte, so two drainers cannot have their bytes arrive
 * out of ring order.  A writer never takes cons_tx_lock, so a writer never
 * waits for the wire -- which is the entire point.  Lock order where both are
 * held is tx then ring, and only drainers hold both.
 *
 * 🔑 And the hold is ONE BYTE, not one line: hw_lock masks interrupts for as
 * long as it is held, so a line-long hold would have moved this change's
 * window from printf_lock onto this lock and changed nothing.  cons_tx_one()
 * says the rest.
 *
 * 🔑 hw_lock and not simple_lock, for the reason #566 found: hw_lock_init()
 * writes a zero and BSS is already zero, so these need nothing to have
 * happened first.  And hw_lock masks interrupts for the hold, which is what
 * makes the rest of this sound: a clock tick cannot land on a processor
 * inside either section, so the tick's own drain can never meet itself, and
 * a processor parked by the debugger's IPI is never parked holding the port.
 */
#define CONS_RING_SIZE	4096u	/* a power of two: the indices wrap by modulo */

/*
 * How many times a writer that found the ring full will pay for room before
 * it gives the byte up.  Each attempt either empties the ring -- the flush
 * below drains until the ring is empty -- or finds the port stuck and
 * discards the queue, so one is normally enough and four is room for other
 * processors refilling it underneath.  A byte given up here is still in klog,
 * and cons_cost_report() says how many there were.
 */
#define CONS_RING_FULL_TRIES	4

static char		cons_ring[CONS_RING_SIZE];
static unsigned		cons_ring_head;		/* bytes queued,  modular */
static unsigned		cons_ring_tail;		/* bytes emitted, modular */
static hw_lock_data_t	cons_ring_lock;		/* the two indices */
static hw_lock_data_t	cons_tx_lock;		/* the port, and the order on it */
static int		cons_async_on;		/* armed: cons_putc queues */

/*
 * ⚠️ Advisory and unlocked, like the three counters above and for the same
 * reason: a lost update costs a number, and a lock here would cost the path
 * that has to work when locks do not.
 */
static unsigned		cons_drain_counts[CONS_DRAIN_SITES];
static unsigned		cons_backpressure_count;
static uint64_t		cons_wire_cycles_count;

unsigned cons_tx_dropped(void)
{
	return cons_tx_dropped_count;
}

unsigned cons_tx_spins_high(void)
{
	return cons_tx_spins_peak;
}

unsigned cons_drains(unsigned site)
{
	return site < CONS_DRAIN_SITES ? cons_drain_counts[site] : 0;
}

unsigned cons_backpressure(void)
{
	return cons_backpressure_count;
}

uint64_t cons_wire_cycles(void)
{
	return cons_wire_cycles_count;
}

unsigned cons_queued(void)
{
	return cons_ring_head - cons_ring_tail;
}

void cons_tx_spins_reset(void)
{
	cons_tx_spins_peak = 0;
}

void cons_capture_begin(char *buf, unsigned max)
{
	cons_capture_len = 0;
	cons_capture_max = max;
	cons_capture_buf = buf;
}

unsigned cons_capture_end(void)
{
	unsigned len = cons_capture_len;

	cons_capture_buf = 0;
	return len;
}

void cons_putc(char c)
{
	if (cons_capture_buf != 0) {
		/*
		 * One short of the maximum, so the caller always has room for
		 * a terminator without having to remember to leave it.
		 */
		if (cons_capture_len + 1 < cons_capture_max)
			cons_capture_buf[cons_capture_len++] = c;
		cons_capture_buf[cons_capture_len] = '\0';
		return;
	}

	if (cons_async_on) {
		cons_queue(c);
		return;
	}

	cons_putc_wire(c);
}

/*
 * Ask the port whether it has a FIFO, once (#567).
 *
 * Lazily, on the first byte, because there is no other moment that is both
 * after boot.S has written FCR and before anything prints -- this file has no
 * init of its own, deliberately: it is the writer of last resort and the fewer
 * things that must have happened before it works, the better.
 */
static void cons_fifo_probe(void)
{
	/*
	 * ABLATE_567_NO_FIFO keeps the old depth of one, which is what this
	 * file did before: the FIFO enabled and one slot of it used.  It is
	 * the other arm of the measurement, in a binary that differs in
	 * nothing else.
	 */
#if	ABLATE_567_NO_FIFO
	cons_fifo_depth = 1;
#else
	cons_fifo_depth = ((inb(COM1 + UART_IIR) & IIR_FIFO_ON) == IIR_FIFO_ON)
			  ? CONS_FIFO_DEPTH : 1;
#endif
	cons_fifo_room = 0;
}

/*
 * How many bytes the port will take right now, for a writer that may wait for
 * room and for one that may not (#567).
 *
 * One place, because there are two callers -- the direct writer below and the
 * ring's drain -- and the bound of #551 has to be the same thing for both.
 * Zero means the caller may not write: either there is no room and it said it
 * would not wait, or the port is stuck.  What that means for the byte in hand
 * is the caller's decision.
 *
 * ABLATE_551_UNBOUNDED puts the original loop back, so that
 * scripts/uart-stall.py can show the machine it stops -- a bound nothing has
 * ever hit is untested code, and this is how it is hit.  The `may_wait' exit
 * is outside the ablation: it is about who is asking, not about the bound.
 */
static unsigned cons_tx_room(int may_wait)
{
	unsigned spins = 0;

	if (cons_fifo_depth == 0)
		cons_fifo_probe();

	/*
	 * A look at the transmitter buys a whole FIFO (#567).  When the room
	 * from the last look is used up -- or when the port has no FIFO, where
	 * the depth is one and this is every byte, exactly as before -- look
	 * again.
	 */
	if (cons_fifo_room != 0)
		return cons_fifo_room;

	while (!(inb(COM1 + UART_LSR) & LSR_THR_EMPTY)) {
		if (!may_wait)
			return 0;
#if	!ABLATE_551_UNBOUNDED
		if (cons_tx_stuck || ++spins >= CONS_THRE_SPINS) {
			cons_tx_stuck = 1;
			return 0;
		}
#endif
		cpu_pause();
	}

	cons_tx_stuck = 0;
	if (spins > cons_tx_spins_peak)
		cons_tx_spins_peak = spins;
	cons_fifo_room = cons_fifo_depth;
	return cons_fifo_room;
}

/*
 * Straight to the port, taking no lock (#551, #567).
 *
 * ⚠️ AND IT TAKES NO LOCK ON PURPOSE.  trap.c's reporter writes through here
 * because its one message has to reach a reader when everything else has
 * stopped, which a lock cannot promise.  The price is that a fault report
 * that lands while another processor is draining the ring shares the port
 * with it: the bytes interleave, as that report's bytes already did, and the
 * two of them decrement the same count of FIFO room with no lock between,
 * so a lost update leaves the count too high and a few bytes of an already-
 * garbled report reach a FIFO with no space for them.  That is the whole of
 * the race, it is bounded to the report, and it is the cheaper half of the
 * trade.
 *
 * 🔴 WHAT IS NOT LEFT TO THAT RACE IS THE UNDERFLOW.  cons_fifo_room is
 * unsigned, and a decrement of zero is four billion -- which cons_tx_room()
 * would then hand out as room, and the console would write thousands of bytes
 * to a port it had never looked at.  The check above cannot prevent it: the
 * other writer may reach zero between the check and the decrement.  So the
 * decrement is guarded here and in cons_tx_one(), which costs a compare and
 * closes the one outcome of this race that is not merely untidy.
 *
 * When the ring is not armed -- early boot, and everything after the way down
 * has turned it off -- this is also cons_putc()'s path, and then there is no
 * drainer in existence and no race at all.
 */
void cons_putc_wire(char c)
{
	if (cons_tx_room(1) == 0) {
		cons_tx_dropped_count++;
		return;
	}

	outb(COM1 + UART_DATA, (uint8_t)c);
	if (cons_fifo_room != 0)
		cons_fifo_room--;
}

/*
 * Hand ONE byte from the ring to the port.
 *
 * 🔴 ONE BYTE PER HOLD, AND THAT IS THE WHOLE POINT.  hw_lock masks
 * interrupts and holds preemption off for as long as it is held, so the
 * length of this section is the length of the window this change exists to
 * shorten.  A whole line under one hold would have moved that window off
 * printf_lock and onto this lock, unchanged; taking the port for one byte
 * makes the window ONE BYTE -- a constant the kernel chooses, where it used
 * to be the length of a line that ring 3 chooses, up to CONSOLE_CHUNK bytes
 * through consolewrite().
 *
 * ⚠️ Chunking would not have bought the FIFO back: cons_fifo_room outlives a
 * hold, so a look at the transmitter still buys sixteen bytes (#567) across
 * sixteen holds.  What the hold costs on top is one exchange per byte,
 * against a device that costs 83 us -- three orders of magnitude apart.
 *
 * ⚠️ And the honest caveat, which is about the metal and not about this
 * code.  Under both accelerators the wait inside cons_tx_room() is nothing:
 * the slowest healthy byte of every boot so far polled ZERO times, because
 * qemu's transmitter has room again before the `outb' that filled it
 * returns.  On a real wire the sixteenth byte waits for the FIFO to empty --
 * some 1.4 ms at 115200 -- and that wait is inside this hold.  It is a
 * property of the port rather than of the caller's line, which is the
 * difference that matters, but it is not zero and saying otherwise would be
 * a claim no boot has checked.
 *
 * Returns 1 if a byte went, 0 if the port would not take one or the ring
 * turned out to be empty.
 */
static int cons_tx_one(int may_wait)
{
	char	c = 0;
	int	went = 0;

	if (may_wait)
		hw_lock_lock(&cons_tx_lock);
	else if (!hw_lock_try(&cons_tx_lock))
		return 0;

	if (cons_tx_room(may_wait) == 0) {
		/*
		 * No room, and this caller was allowed to wait for it: the
		 * port is stuck, which #551 treats as a condition and not an
		 * event.  So the queue goes rather than standing between the
		 * machine and everything it prints from here on -- klog still
		 * holds every byte of it, and cons_cost_report() says how many
		 * were lost.  A caller that would not wait simply leaves them
		 * for the next tick.
		 */
		if (may_wait) {
			hw_lock_lock(&cons_ring_lock);
			cons_tx_dropped_count += cons_ring_head - cons_ring_tail;
			cons_ring_tail = cons_ring_head;
			hw_lock_unlock(&cons_ring_lock);
		}
		hw_lock_unlock(&cons_tx_lock);
		return 0;
	}

	hw_lock_lock(&cons_ring_lock);
	if (cons_ring_head != cons_ring_tail) {
		c = cons_ring[cons_ring_tail++ % CONS_RING_SIZE];
		went = 1;
	}
	hw_lock_unlock(&cons_ring_lock);

	if (went) {
		outb(COM1 + UART_DATA, (uint8_t)c);
		/* Guarded against the unlocked writer: see cons_putc_wire(). */
		if (cons_fifo_room != 0)
			cons_fifo_room--;
	}

	hw_lock_unlock(&cons_tx_lock);
	return went;
}

/*
 * Hand over what is queued, and count where it was done from.
 *
 * 🔑 A DRAINER OWES THE RING WHAT IT FOUND, not what arrives while it works.
 * Without that, a processor that started draining while others kept printing
 * would stay here for as long as they cared to print -- the unbounded wait of
 * #551 rebuilt out of other people's lines.  The snapshot is taken unlocked,
 * which is right: it is a budget and not an invariant.
 */
static unsigned cons_drain_run(int may_wait, unsigned site)
{
	unsigned	owed, sent;
	uint64_t	t0;

	owed = cons_ring_head - cons_ring_tail;
	if (owed == 0)
		return 0;

	cons_drain_counts[site]++;
	t0 = rdtsc_ordered();
	for (sent = 0; sent < owed; sent++)
		if (!cons_tx_one(may_wait))
			break;
	cons_wire_cycles_count += rdtsc_ordered() - t0;
	return sent;
}

/*
 * Push what the port will take right now and return.  Never waits -- neither
 * for the port's lock nor for the transmitter -- so it is safe from the clock
 * tick; what it cannot place stays in the ring for the next visit.
 *
 * The unlocked look at the two indices inside cons_drain_run() is what keeps
 * this off the port entirely when there is nothing to send, and that is also
 * what makes the way down safe: once the ring is empty and cons_async_set(0)
 * has run, no drainer will ever take cons_tx_lock again, so nothing can be
 * stopped or parked holding it.
 */
void cons_drain(void)
{
	(void) cons_drain_run(0, CONS_DRAIN_DEFERRED);
}

/*
 * Get what is queued onto the wire, waiting for the transmitter within the
 * bound of #551.
 *
 * This is what printf() calls once it has let printf_lock go, so the device
 * time is paid by the thread that printed -- as it always was -- but outside
 * the window where that thread cannot be rescheduled and takes no interrupt.
 */
void cons_flush(void)
{
	(void) cons_drain_run(1, CONS_DRAIN_WRITER);
}

/*
 * Append one byte, and pay for room if the ring has none.
 *
 * A full ring means this writer is outrunning the wire, and then it drains the
 * ring itself: back pressure, and the price is exactly what the console used
 * to charge every writer for every byte.  It is counted, because a boot that
 * pays it often has a ring that is too small and that is a fact about this
 * ring rather than a guess about it.
 */
static void cons_queue(char c)
{
	unsigned tries;

	for (tries = 0; ; tries++) {
		hw_lock_lock(&cons_ring_lock);
		if (cons_ring_head - cons_ring_tail < CONS_RING_SIZE) {
			cons_ring[cons_ring_head++ % CONS_RING_SIZE] = c;
			hw_lock_unlock(&cons_ring_lock);
			return;
		}
		hw_lock_unlock(&cons_ring_lock);

		if (tries >= CONS_RING_FULL_TRIES) {
			cons_tx_dropped_count++;
			return;
		}
		cons_backpressure_count++;
		cons_flush();
	}
}

/*
 * Arm the ring, or take it away.
 *
 * 🔴 TAKING IT AWAY IS ALSO A FLUSH, and in that order: nothing new may be
 * queued from the moment this is called, and what is already queued goes out
 * before it returns.  A console that stopped buffering with bytes still
 * buffered would be precisely the failure this has to avoid -- on this target
 * the serial port is the only output there is (#497), so a line that is in a
 * ring and not on the wire is a line lost if the machine stops.
 *
 * The named places that take it away are panic(), the debugger and
 * halt_all_cpus(); the end of a self-test run flushes without disarming,
 * because the machine goes on afterwards.
 *
 * ABLATE_567_NO_RING refuses to arm, which leaves every byte on the synchronous
 * path this file had before -- the other arm of the measurement, in a binary
 * that differs in nothing else.
 */
void cons_async_set(int on)
{
#if	ABLATE_567_NO_RING
	on = 0;
#endif
	if (!on) {
		cons_async_on = 0;
		while (cons_drain_run(1, CONS_DRAIN_DOWN) != 0)
			;
		return;
	}
	cons_async_on = 1;
}

void cons_puts(const char *s)
{
	for (; *s; s++)
		cons_putc(*s);
}

void cons_puthex64(uint64_t v)
{
	cons_puts("0x");
	for (int i = 60; i >= 0; i -= 4)
		cons_putc("0123456789abcdef"[(v >> i) & 0xF]);
}

void cons_putdec(uint64_t v)
{
	char buf[20];
	int i = 0;

	if (v == 0) {
		cons_putc('0');
		return;
	}

	while (v > 0) {
		buf[i++] = '0' + (char)(v % 10);
		v /= 10;
	}
	while (i > 0)
		cons_putc(buf[--i]);
}

/*
 * Formatted output (#415).
 *
 * This exists because panic() has to be variadic.  The machine-independent
 * tree declares `void panic(const char *, ...)` and the x86-64 tree defined
 * `void panic(const char *)`, so every panic in MI code was going to hand its
 * arguments to a function that had no way to take them -- and print the
 * format string with the values missing, at the one moment when the values
 * are the whole message.
 *
 * The conversions are the ones the tree actually uses, counted rather than
 * guessed: across every panic() in kern/ ipc/ vm/ device/ intel/ i386/ there
 * are exactly three, %x (54), %s (50) and %d (31).  %u, %c, %p and %% come
 * along because they cost a line each and their absence would be a surprise.
 *
 * The length modifiers are not padding of the set: on this target an address
 * is sixty-four bits and %x prints thirty-two, so without %lx a panic can
 * report half a pointer, which is worse than reporting none.  That is the
 * same defect this audit is looking for, and it would be odd to build it into
 * the tool doing the looking.
 *
 * No field widths, no precision, no padding.  Nothing in the tree asks for
 * them, and a formatter that quietly accepts more than it implements is how
 * you end up trusting output that was never produced.
 */
static void cons_putdec_signed(int64_t v)
{
	if (v < 0) {
		cons_putc('-');
		/* Negated as unsigned: -INT64_MIN does not fit in int64_t. */
		cons_putdec(-(uint64_t)v);
		return;
	}
	cons_putdec((uint64_t)v);
}

static void cons_puthex(uint64_t v, unsigned digits)
{
	for (int i = (int)(digits - 1) * 4; i >= 0; i -= 4)
		cons_putc("0123456789abcdef"[(v >> i) & 0xF]);
}

void cons_vprintf(const char *fmt, va_list ap)
{
	for (; *fmt; fmt++) {
		unsigned longs = 0;
		const char *spec;

		if (*fmt != '%') {
			cons_putc(*fmt);
			continue;
		}

		spec = fmt++;
		while (*fmt == 'l') {
			longs++;
			fmt++;
		}

		switch (*fmt) {
		case 's': {
			const char *s = va_arg(ap, const char *);

			cons_puts(s != 0 ? s : "(null)");
			break;
		}
		case 'c':
			cons_putc((char)va_arg(ap, int));
			break;
		case 'd':
			cons_putdec_signed(longs ? va_arg(ap, int64_t)
					         : va_arg(ap, int));
			break;
		case 'u':
			cons_putdec(longs ? va_arg(ap, uint64_t)
					  : va_arg(ap, unsigned int));
			break;
		case 'x':
			if (longs)
				cons_puthex(va_arg(ap, uint64_t), 16);
			else
				cons_puthex(va_arg(ap, unsigned int), 8);
			break;
		case 'p':
			cons_puthex64((uint64_t)(uintptr_t)
				      va_arg(ap, void *));
			break;
		case '%':
			cons_putc('%');
			break;
		case '\0':
			/* Trailing '%': print it and stop, rather than run off. */
			cons_putc('%');
			return;
		default:
			/*
			 * Unknown conversion.  Its argument cannot be consumed
			 * without knowing its width, so everything after this
			 * is reading the wrong slot -- which is exactly the
			 * kind of quietly wrong output panic must not produce.
			 * The specifier is echoed verbatim so the reader can
			 * see what was asked for, and the rest is abandoned
			 * rather than invented.
			 */
			while (spec <= fmt)
				cons_putc(*spec++);
			cons_puts(" <unsupported conversion, rest dropped>");
			return;
		}
	}
}

void cons_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	cons_vprintf(fmt, ap);
	va_end(ap);
}

int cons_getc_nowait(void)
{
	if (!(inb(COM1 + UART_LSR) & LSR_DATA_READY))
		return -1;

	return inb(COM1 + UART_DATA);
}

int cons_getc(void)
{
	int c;

	while ((c = cons_getc_nowait()) < 0)
		cpu_pause();

	return c;
}

/*
 * How long to wait for the loopback byte before giving up.
 *
 * Bounded, unlike the command prompt, and for the opposite reason: nobody is
 * going to type this one. If the byte does not come back the port is not
 * what this code thinks it is, and a probe that hung would turn a diagnosis
 * into a boot that stops with no explanation — which is the failure this
 * whole file exists to end.
 */
#define LOOPBACK_SPINS	100000u

int cons_loopback_probe(uint8_t byte)
{
	uint8_t saved_mcr;
	int was_async = cons_async_on;
	int result = -1;

	/*
	 * Nothing may be in flight while the port is disconnected from the
	 * outside (#567): a queued byte handed to a loopback transmitter is a
	 * byte that never leaves the machine, and it would come back as this
	 * probe's answer.  So the ring is emptied and disarmed first -- which
	 * also waits for any drain already running, since the flush takes the
	 * port's lock -- and with an empty ring no drainer will take that lock
	 * again for as long as this runs.
	 */
	cons_async_set(0);
	saved_mcr = inb(COM1 + UART_MCR);

	/*
	 * Anything already waiting is drained first: a byte that arrived from
	 * outside before this started would come back as the answer, and the
	 * probe would report a working receiver on the strength of somebody
	 * having pressed a key.
	 */
	for (unsigned i = 0; i < 16 && cons_getc_nowait() >= 0; i++)
		;

	outb(COM1 + UART_MCR, MCR_DTR | MCR_RTS | MCR_LOOPBACK);

	/*
	 * Bounded like every other wait on this register (#551).  A byte that
	 * cannot be handed to the transmitter is a byte that will not come
	 * back, and the loop below then answers -1 for the right reason.
	 */
	for (unsigned i = 0; i < CONS_THRE_SPINS; i++) {
		if (inb(COM1 + UART_LSR) & LSR_THR_EMPTY)
			break;
		cpu_pause();
	}
	if (inb(COM1 + UART_LSR) & LSR_THR_EMPTY)
		outb(COM1 + UART_DATA, byte);

	for (unsigned i = 0; i < LOOPBACK_SPINS; i++) {
		if (inb(COM1 + UART_LSR) & LSR_DATA_READY) {
			result = inb(COM1 + UART_DATA);
			break;
		}
		cpu_pause();
	}

	/*
	 * The port goes back to whatever it was before anything is said about
	 * the result — including on the path where nothing came back, which is
	 * exactly the path where leaving the port in loopback would silence
	 * the report explaining why.
	 */
	outb(COM1 + UART_MCR, saved_mcr);

	/*
	 * Whatever this did to the port, the room counted before it is not
	 * counted any more (#567): the next byte looks at the transmitter
	 * again rather than trusting a number taken across a loopback.
	 */
	cons_fifo_room = 0;
	if (was_async)
		cons_async_set(1);
	return result;
}

void cons_puthex8(uint8_t v)
{
	static const char hex[] = "0123456789abcdef";

	cons_putc(hex[(v >> 4) & 0xF]);
	cons_putc(hex[v & 0xF]);
}
