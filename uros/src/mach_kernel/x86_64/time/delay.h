/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The three waits delay() is made of (#624).
 *
 * delay(), declared in <kern/misc_protos.h>, spins for at least the
 * microseconds it is asked, on the first of these whose counter is there.
 * Each one returns 1 if it waited and 0 if it had nothing to count on, so
 * that the boot's check can time every one of them against another counter.
 */

#ifndef _X86_64_TIME_DELAY_H_
#define _X86_64_TIME_DELAY_H_

int delay_tsc_us(unsigned us);		/* the TSC, calibrated and trusted */
int delay_hpet_us(unsigned us);		/* the HPET's main counter */
int delay_pmtimer_us(unsigned us);	/* the ACPI PM timer */

#endif /* _X86_64_TIME_DELAY_H_ */
