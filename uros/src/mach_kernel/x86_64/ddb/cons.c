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
#define UART_LSR	5

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

	while (!(inb(COM1 + UART_LSR) & LSR_THR_EMPTY))
		;
	outb(COM1 + UART_DATA, (uint8_t)c);
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

	while (!(inb(COM1 + UART_LSR) & LSR_THR_EMPTY))
		;
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
	return result;
}
