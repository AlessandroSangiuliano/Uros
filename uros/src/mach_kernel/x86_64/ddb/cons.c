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

unsigned cons_tx_dropped(void)
{
	return cons_tx_dropped_count;
}

unsigned cons_tx_spins_high(void)
{
	return cons_tx_spins_peak;
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

void cons_putc_wire(char c)
{
	/*
	 * ABLATE_551_UNBOUNDED puts the original loop back, so that
	 * scripts/uart-stall.py can show the machine it stops -- a bound
	 * nothing has ever hit is untested code, and this is how it is hit.
	 */
	unsigned spins = 0;

	if (cons_fifo_depth == 0)
		cons_fifo_probe();

	/*
	 * A look at the transmitter buys a whole FIFO (#567).  When the room
	 * from the last look is used up -- or when the port has no FIFO, where
	 * the depth is one and this is every byte, exactly as before -- look
	 * again.
	 */
	if (cons_fifo_room == 0) {
		while (!(inb(COM1 + UART_LSR) & LSR_THR_EMPTY)) {
#if	!ABLATE_551_UNBOUNDED
			if (cons_tx_stuck || ++spins >= CONS_THRE_SPINS) {
				cons_tx_stuck = 1;
				cons_tx_dropped_count++;
				return;
			}
#endif
			cpu_pause();
		}
		cons_tx_stuck = 0;
		if (spins > cons_tx_spins_peak)
			cons_tx_spins_peak = spins;
		cons_fifo_room = cons_fifo_depth;
	}

	outb(COM1 + UART_DATA, (uint8_t)c);
	cons_fifo_room--;
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
	uint8_t saved_mcr = inb(COM1 + UART_MCR);
	int result = -1;

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
	return result;
}

void cons_puthex8(uint8_t v)
{
	static const char hex[] = "0123456789abcdef";

	cons_putc(hex[(v >> 4) & 0xF]);
	cons_putc(hex[v & 0xF]);
}
