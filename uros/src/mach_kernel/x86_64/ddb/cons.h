/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The debugger's console (#428).
 *
 * Until now the kernel could only talk. Every line it has ever produced went
 * out of COM1 and nothing ever came back, which is enough to narrate a boot
 * and not enough for a debugger: a debugger is a conversation, and half of
 * one is a log.
 *
 * The port itself is already set up — boot.S programmes it to 38400 8N1 with
 * the FIFO on before the first announcement — so what is missing is only the
 * receiving half.
 *
 * ── Polled, and staying polled ────────────────────────────────────────
 *
 * No interrupts, deliberately. The moment this is most needed is the moment
 * the machine is least trustworthy: after a fault, possibly with interrupts
 * disabled, possibly on a processor whose interrupt controller is part of
 * what went wrong. A console that needs an interrupt to deliver a keystroke
 * is a console that stops working exactly when it is wanted.
 *
 * It costs a spin per character, which is free at human typing speed.
 */

#ifndef _X86_64_DDB_CONS_H_
#define _X86_64_DDB_CONS_H_

#include <stdarg.h>
#include <stdint.h>

/* One character out, waiting for room. */
void cons_putc(char c);
void cons_puts(const char *s);

/*
 * Formatted output (#415), for panic() and anything else that has a value to
 * report rather than a sentence.  %s %c %d %u %x %p %%, with l/ll; no field
 * widths and no precision.  See cons.c for why that set and not another.
 */
void cons_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void cons_vprintf(const char *fmt, va_list ap)
	__attribute__((format(printf, 1, 0)));

/*
 * Point the console at memory instead of the port, so a selftest can read
 * back what the formatter produced. Always terminated; returns the length
 * written, which is short of max only because the terminator has room.
 */
void cons_capture_begin(char *buf, unsigned max);
unsigned cons_capture_end(void);

/*
 * Numbers, because three copies of this had accumulated — one in the boot
 * narration, one in the fault reporter, and the debugger would have been a
 * fourth. Formatting belongs with the thing that does the writing.
 */
void cons_puthex64(uint64_t v);
void cons_putdec(uint64_t v);

/*
 * One character in, or -1 if none has arrived.
 *
 * The non-blocking form is the one a break-key check wants: asking "has
 * anybody interrupted us" must not become "wait until somebody does".
 */
int cons_getc_nowait(void);

/*
 * One character in, waiting for it.
 *
 * Unbounded, and that is right here: this is the command prompt of a
 * debugger, and a debugger with a timeout is a debugger that walks away
 * while you are reading.
 */
int cons_getc(void);

/*
 * Send a byte to ourselves through the receiver and return what came back,
 * or -1 if nothing did.
 *
 * The 16550 can connect its transmitter to its own receiver — a bit in the
 * modem control register — which makes the receiving path testable without a
 * human at the other end and without anything leaving the machine. That
 * matters more than convenience: the receive path is otherwise only
 * exercised by somebody typing, so a kernel whose console cannot hear would
 * look exactly like a kernel nobody had typed at.
 *
 * ⚠️ The port is disconnected from the outside while this runs, so nothing
 * may be printed from within it, and the previous state is put back before
 * it returns.
 */
int cons_loopback_probe(uint8_t byte);

/* One byte, two digits — for instruction bytes the decoder declined. */
void cons_puthex8(uint8_t v);

#endif	/* _X86_64_DDB_CONS_H_ */
