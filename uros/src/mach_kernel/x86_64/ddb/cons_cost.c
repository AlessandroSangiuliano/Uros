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

/* The median of five: insertion sort, then the middle one. */
static uint64_t
cost_median(uint64_t v[COST_LINES])
{
	unsigned i, j;

	for (i = 1; i < COST_LINES; i++)
		for (j = i; j > 0 && v[j - 1] > v[j]; j--) {
			uint64_t t = v[j];

			v[j] = v[j - 1];
			v[j - 1] = t;
		}
	return v[COST_LINES / 2];
}

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
	uint64_t	hold[COST_LINES];
	uint64_t	t0, w0, hz, med, med_hold, us, ns_byte, ns_hold;
	unsigned	i, len, peak, dropped;

	len = sizeof line - 1;

	cons_tx_spins_reset();
	for (i = 0; i < COST_LINES; i++) {
		w0 = cons_wire_cycles();
		t0 = rdtsc_ordered();
		printf("%s", line);
		took[i] = rdtsc_ordered() - t0;

		/*
		 * What this line spent handing bytes to the port, which since
		 * #567 is outside printf_lock.  The rest is the WINDOW: the
		 * part of a printf in which this processor cannot be
		 * rescheduled and, on this target, takes no interrupt (#528).
		 *
		 * ⚠️ Subtracted and not measured directly, so it carries
		 * whatever the drain's own two rdtsc's cost; that is tens of
		 * cycles against a device that costs tens of thousands.  And
		 * with the ring not armed -- the #567 ablation, or i386 --
		 * cons_wire_cycles() never moves and the window is the whole
		 * line, which is exactly what it was.
		 */
		hold[i] = took[i] - (cons_wire_cycles() - w0);
	}

	med = cost_median(took);
	med_hold = cost_median(hold);

	peak = cons_tx_spins_high();
	dropped = cons_tx_dropped();
	hz = tsc_hz();

	if (hz == 0) {
		printf("UrMach x86-64: console: a %u-byte line costs %llu "
		       "cycles, of which %llu is the window — NOT ASKED for "
		       "the time, no calibrated TSC; the slowest byte polled "
		       "%u times of %u allowed, %u bytes dropped so far "
		       "(#551, #567)\n",
		       len, (unsigned long long) med,
		       (unsigned long long) med_hold, peak, CONS_THRE_SPINS,
		       dropped);
	} else {
		us = med * 1000000ULL / hz;
		ns_byte = med * 1000000000ULL / hz / len;
		ns_hold = med_hold * 1000000000ULL / hz / len;
		printf("UrMach x86-64: console: a %u-byte line costs %llu "
		       "cycles = %llu us, %llu ns a byte, of which %llu ns a "
		       "byte is the WINDOW where this processor cannot be "
		       "rescheduled; the slowest byte polled %u times of %u "
		       "allowed, %u bytes dropped so far (#551, #567)\n",
		       len, (unsigned long long) med, (unsigned long long) us,
		       (unsigned long long) ns_byte,
		       (unsigned long long) ns_hold, peak, CONS_THRE_SPINS,
		       dropped);
	}

}

/*
 * Where the bytes of this boot were actually handed to the port (#567).
 *
 * 🔴 Printed rather than assumed, because a drain nobody ever reaches is not
 * support.  The tick's and the idle loop's is the one with no other caller to
 * prove it: it exists for the bytes a writer had to leave behind because
 * another processor held the port, so a boot in which it never fires is a
 * boot in which that never happened -- which is a fact a reader should be
 * shown rather than have to go looking for.
 *
 * ⚠️ At the END of the run and not beside the cost line, which is printed a
 * few lines into setup_main and would report a boot that has barely started
 * -- and in particular before there has been a single tick to drain anything.
 *
 * 🔑 AND A RUN ON THIS TARGET HAS TWO ENDS, so it is said from both and said
 * ONCE.  halt_cpu() is one: a panic, a self-test that halts because the boot
 * WAS the test, an operator's halt; it says this before disarming the ring,
 * so the line itself still goes out.  The other is the machine going quiet
 * with everything finished, which is how an ordinary run ends -- the harness
 * stops a kernel that has nothing left to do rather than waiting for it to
 * halt, so a report that only halt_cpu() made would never be printed by the
 * runs that matter most.
 *
 * ⚠️ A plain word and no atomic, so two processors arriving together can both
 * print it.  That is the right way round for a report: a line said twice is a
 * reader's mild confusion, a line lost to a failed exchange is a boot with no
 * answer at all.
 */
static int	cons_ring_said;

void
cons_ring_report(void)
{
	if (cons_ring_said)
		return;
	cons_ring_said = 1;

	printf("UrMach x86-64: console: over this boot the ring was handed "
	       "over %u times by the thread that printed, %u by a tick or an "
	       "idle processor, %u on the way down; %u writers paid for room, "
	       "%u bytes are still queued (#567)\n",
	       cons_drains(CONS_DRAIN_WRITER), cons_drains(CONS_DRAIN_DEFERRED),
	       cons_drains(CONS_DRAIN_DOWN), cons_backpressure(),
	       cons_queued());
}
