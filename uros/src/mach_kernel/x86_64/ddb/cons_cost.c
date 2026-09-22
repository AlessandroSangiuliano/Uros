/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * cons_cost.c — what one line on the console costs, said once per boot (#551)
 *
 * Nothing in the tree recorded what a kernel printf costs, which is how 9.6
 * million cycles inside a trap were read as a defect in the trap: the whole
 * of it was a 55-byte diagnostic leaving a polled UART at the DEVICE's pace,
 * under printf_lock with interrupts masked.  So the cost is measured on every
 * boot and written into the log beside the numbers it explains, on the
 * accelerator and the machine they were taken on -- which is the only place a
 * figure like this means anything (#516).
 *
 * Five lines of one length, the median, and two numbers the line alone would
 * hide:
 *
 * ⚠️ The DEVICE's pace, and not the wire's: under an accelerator there is no
 * wire, and the two are not even the same order -- 83 us a byte under KVM
 * against the 87 us that 8N1 at the 115200 baud boot.S now programs takes on
 * a real line (#567; it was 260 at 38400, and 115200 is a decision made in
 * boot.S and repeated in grub.cfg).  Which of them a boot measured is decided
 * by what it booted on, which is why the conditions block beside this line is
 * part of the number (#516).
 *
 *   - the most polls a healthy byte spent waiting for the transmitter, beside
 *     the bound cons_putc() gives up at.  The bound is in polls, and a poll is
 *     a different length of time on every accelerator and every machine; this
 *     is how one that brings the two close is seen before it starts losing
 *     bytes, and how the number in cons.h was chosen in the first place.
 *
 *   - how many bytes that bound has dropped so far.  Zero on a healthy port;
 *     and on a log with holes in it, the first fact its reader needs.
 *
 * NOT ASKED (#563) for the time when the TSC is not calibrated: the cycles are
 * still a number and are still printed, but a microsecond figure without a
 * ruler would be a guess wearing a unit.
 */

#include <stdint.h>

#include <kern/misc_protos.h>
#include <ddb/cons.h>
#include <ddb/cons_cost.h>
#include <time/tsc.h>

#define COST_LINES	5

void
cons_cost_report(void)
{
	/*
	 * 47 bytes with the newline: the length #551 was measured at, so the
	 * figure here and the figure there are the same unit.
	 */
	static const char line[] =
		"console: this line is timed, five times (#551)\n";
	uint64_t	took[COST_LINES];
	uint64_t	t0, hz, med, us, ns_byte;
	unsigned	i, j, len, peak, dropped;

	len = sizeof line - 1;

	cons_tx_spins_reset();
	for (i = 0; i < COST_LINES; i++) {
		t0 = rdtsc_ordered();
		printf("%s", line);
		took[i] = rdtsc_ordered() - t0;
	}

	/* The median of five: insertion sort, then the middle one. */
	for (i = 1; i < COST_LINES; i++)
		for (j = i; j > 0 && took[j - 1] > took[j]; j--) {
			uint64_t t = took[j];

			took[j] = took[j - 1];
			took[j - 1] = t;
		}
	med = took[COST_LINES / 2];

	peak = cons_tx_spins_high();
	dropped = cons_tx_dropped();
	hz = tsc_hz();

	if (hz == 0) {
		printf("UrMach x86-64: console: a %u-byte line costs %llu "
		       "cycles — NOT ASKED for the time, no calibrated TSC; "
		       "the slowest byte polled %u times of %u allowed, "
		       "%u bytes dropped so far (#551)\n",
		       len, (unsigned long long) med, peak, CONS_THRE_SPINS,
		       dropped);
		return;
	}

	us = med * 1000000ULL / hz;
	ns_byte = med * 1000000000ULL / hz / len;
	printf("UrMach x86-64: console: a %u-byte line costs %llu cycles = "
	       "%llu us, %llu ns a byte; the slowest byte polled %u times of "
	       "%u allowed, %u bytes dropped so far (#551)\n",
	       len, (unsigned long long) med, (unsigned long long) us,
	       (unsigned long long) ns_byte, peak, CONS_THRE_SPINS, dropped);
}
