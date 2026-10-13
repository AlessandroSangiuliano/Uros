/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * delay(): spin for at least a number of microseconds (#624).
 *
 * <kern/misc_protos.h> declares it, and i386 has one: a loop counted out at
 * boot, whose length moves with the clock the core runs at.  x86-64 had none.
 * Here the time is read from a counter, never inferred from a count of
 * instructions.  The TSC, invariant and calibrated (time/tsc.c), is waited on
 * up to a deadline.  When tsc_hz() is zero -- before the calibration, or once
 * the watchdog has withdrawn the TSC -- the wait is on the HPET's main
 * counter, and failing that on the ACPI PM timer.  Both can only be read, by
 * any processor at once.  The 8254's channel 2 is not among them:
 * pit_delay_us() reprograms it, and the kernel keeps it as a ruler (#508).
 */

#include <stdint.h>

#include <kern/cpu_data.h>		/* disable_preemption */
#include <kern/cpu_number.h>
#include <kern/misc_protos.h>

#include <cpu/regs.h>			/* cpu_pause */
#include <time/delay.h>
#include <time/hpet.h>
#include <time/pmtimer.h>
#include <time/tsc.h>

/*
 * The ticks a counter at hz makes in us microseconds, rounded up, and in two
 * parts so that no product overflows: whole seconds, then what is left.
 */
static uint64_t
ticks_in(unsigned us, uint64_t hz)
{
	return (uint64_t)(us / 1000000) * hz +
	    ((uint64_t)(us % 1000000) * hz + 999999) / 1000000;
}

/*
 * The TSC, one processor at a time.  A thread can be moved between two of
 * its reads, and nothing promises that two processors' counters agree to the
 * cycle, so each read and the look at where it ran are made with preemption
 * off and the PAUSE between them with it on: a move closes one stretch and
 * starts the next from what the last one counted.  Linux's delay_tsc() does
 * the same, for the same reason.  Time spent preempted is not counted, which
 * only makes the wait longer.
 */
int
delay_tsc_us(unsigned us)
{
	uint64_t	hz = tsc_hz();
	uint64_t	cycles, start, now;
	int		cpu;

	if (hz == 0)
		return 0;
	cycles = ticks_in(us, hz);
	disable_preemption();
	cpu = cpu_number();
	start = rdtsc_ordered();
	while ((now = rdtsc_ordered()) - start < cycles) {
		enable_preemption();
		cpu_pause();
		disable_preemption();
		if (cpu_number() != cpu) {
			cycles -= now - start;
			cpu = cpu_number();
			start = rdtsc_ordered();
		}
	}
	enable_preemption();
	return 1;
}

/*
 * A counter that wraps, read a PAUSE apart with the differences added up, so
 * that no one difference comes near a wrap: 43 s for the HPET read 32 bits
 * wide at 100 MHz, 4.7 s for a 24-bit PM timer.  Every processor reads the
 * same counter, so a move changes nothing.
 */
static void
delay_on(uint64_t ticks, uint32_t (*read)(void),
	 uint32_t (*delta)(uint32_t from, uint32_t to))
{
	uint64_t	elapsed = 0;
	uint32_t	last = read();
	uint32_t	now;

	while (elapsed < ticks) {
		cpu_pause();
		now = read();
		elapsed += delta(last, now);
		last = now;
	}
}

static uint32_t
hpet_delta32(uint32_t from, uint32_t to)
{
	return to - from;
}

int
delay_hpet_us(unsigned us)
{
	if (!hpet_present())
		return 0;
	delay_on(ticks_in(us, hpet_hz()), hpet_read32, hpet_delta32);
	return 1;
}

int
delay_pmtimer_us(unsigned us)
{
	if (!pmtimer_present())
		return 0;
	delay_on(ticks_in(us, PMTIMER_HZ), pmtimer_read, pmtimer_delta);
	return 1;
}

/*
 * With none of the three there is nothing to count a microsecond on, and
 * returning would be a wait that never happened.
 */
void
delay(int n)
{
	if (n <= 0)
		return;
	if (!delay_tsc_us((unsigned)n) && !delay_hpet_us((unsigned)n) &&
	    !delay_pmtimer_us((unsigned)n))
		panic("delay(%d): no TSC, HPET or PM timer to count it on -- "
		      "called before the rulers were found (#624)", n);
}
