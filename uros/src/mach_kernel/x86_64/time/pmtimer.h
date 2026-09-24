/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The ACPI power-management timer, as a ruler (#508).
 *
 * A free-running up-counter at 3.579545 MHz, 24 or 32 bits wide, that
 * software can only read (ACPI 6.5, 4.8.3.3).  Nothing about it is
 * programmed, and nothing can be, which is what makes it worth having next
 * to the 8254: nobody, this kernel included, can leave it in a wrong state.
 * Its frequency is stated by the specification rather than measured, so it
 * is a ruler that needs no ruler.
 *
 * ⚠️ Its starting value is undefined, and so is anything but a difference
 * between two reads taken less than one wrap apart: 4.7 s at 24 bits.
 */

#ifndef _X86_64_TIME_PMTIMER_H_
#define _X86_64_TIME_PMTIMER_H_

#include <stdint.h>

#define PMTIMER_HZ	3579545u

/*
 * Find the timer through the FADT and make it readable.  Returns 1 if there
 * is one.  A hardware-reduced platform has none, and so does a FADT that
 * states no block; both are answers, reported by whoever asks.
 */
int pmtimer_init(void);

int pmtimer_present(void);
unsigned pmtimer_width(void);		/* 24 or 32 */
int pmtimer_is_io(void);		/* a port, or else memory */
uint64_t pmtimer_address(void);		/* the port or the physical address */

/* The count, masked to the timer's width.  Only valid if present. */
uint32_t pmtimer_read(void);

/* Counts from `from' to `to', across at most one wrap. */
uint32_t pmtimer_delta(uint32_t from, uint32_t to);

#endif	/* _X86_64_TIME_PMTIMER_H_ */
