/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The debugger's console (#428).
 */

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

void cons_putc(char c)
{
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
