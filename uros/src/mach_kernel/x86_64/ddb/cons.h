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
 * The port itself is already set up — boot.S programmes it to 115200 8N1 with
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
/*
 * How many polls one byte may spend waiting for the transmitter (#551).
 *
 * In polls and not in time, because the first bytes go out before the TSC is
 * calibrated, and because the boot narration's kputc (boot_c.c) shares this
 * number so that the two polled writers cannot drift apart.  A poll costs
 * whatever an `inb' costs on the accelerator or the machine, so this is a
 * different length of time everywhere it runs -- which is why the number was
 * measured against a healthy byte and not chosen, and why cons_cost_report()
 * prints the slowest healthy byte of every boot beside it.
 *
 * Measured (victus, qemu 11.1.1, entry 6): under KVM and under TCG alike the
 * slowest healthy byte polled ZERO times -- qemu's 16550 has room again
 * before the `outb' that filled it returns, and the 80 µs a byte KVM shows
 * is paid inside that outb, not in this loop.  So on either accelerator the
 * bound is never approached by a working port, and only a stuck one reaches
 * it: 4000 polls is some 6 ms under KVM, paid once per stall (cons_putc
 * remembers).  On the metal a byte at 115200 baud takes 87 µs and a port
 * poll about one, so a healthy byte would poll near 90 times and this bound
 * is forty of them -- reasoning, not a measurement, and the per-boot line is
 * what will turn it into one on the first bare-metal boot.
 *
 * The number itself lives in cons_bound.h, where boot.S can read it too.
 */
#include <ddb/cons_bound.h>

void cons_putc(char c);
void cons_puts(const char *s);

/*
 * The same write, straight to the wire, past the capture buffer (#551).  For
 * the trap path's reporting, whose one message must reach a reader when
 * everything else has stopped -- a selftest's capture included.
 */
void cons_putc_wire(char c);

/* What the bound has done on this boot, for cons_cost_report() (#551). */
unsigned cons_tx_dropped(void);
unsigned cons_tx_spins_high(void);
void cons_tx_spins_reset(void);

/*
 * ── The console that does not make the caller wait for the wire (#567) ──
 *
 * printf() rendered a line into the port one byte at a time, under a lock
 * held with preemption off and interrupts masked, and a byte costs what the
 * DEVICE costs.  Now it renders into a ring under that lock and the port is
 * written outside it, in ring order, so a line still arrives whole.  Why a
 * ring answers the interleaving objection, and what this does NOT claim, is
 * written above the ring in cons.c.
 *
 * ⚠️ The constraint that makes this delicate: on this target the serial port
 * is the only output there is (#497), so a line that is in the ring and not
 * on the wire is a line lost if the machine stops.  Every way down therefore
 * disarms the ring, and disarming flushes it.
 */

/*
 * Arm the ring, or take it away -- and taking it away flushes it first.
 * Armed by printf_init(), because that is the moment the kernel's own printf
 * becomes the writer; taken away by panic(), by the debugger and by
 * halt_all_cpus(), which are the named ways down.
 */
void cons_async_set(int on);

/*
 * Push what the port will take right now and return; never waits.  The clock
 * tick and the idle loop call this, which is what gets out the bytes left
 * behind by a writer that could not get the port.
 */
void cons_drain(void);

/*
 * Push everything that is queued, waiting for the transmitter within the
 * bound of #551.  printf() calls this once it has let its lock go, and a
 * self-test run calls it before the harness reads what it printed.
 */
void cons_flush(void);

/*
 * Where the bytes of this boot were actually handed to the port, counted so
 * that a drain nobody ever reached is visible as the zero it is rather than
 * passing for support.  cons_cost_report() prints them.
 */
enum {
	CONS_DRAIN_WRITER = 0,	/* the thread that printed, after unlocking */
	CONS_DRAIN_DEFERRED,	/* the clock tick, or an idle processor */
	CONS_DRAIN_DOWN,	/* panic, the debugger, halt_all_cpus */
	CONS_DRAIN_SITES
};

unsigned cons_drains(unsigned site);
unsigned cons_backpressure(void);	/* writers that had to pay for room */
unsigned cons_queued(void);		/* bytes in the ring right now */
uint64_t cons_wire_cycles(void);	/* cycles spent handing bytes over */

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
