/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The HPET's main counter, as a ruler (#508).
 *
 * The timer block's counter counts up at a period its own capability register
 * states in femtoseconds (IA-PC HPET 1.0a, 2.3.4), so, like the PM timer, it
 * is a ruler that needs no ruler.  This file reads it, and the only thing it
 * ever writes is the overall enable bit, when the firmware left the counter
 * halted: the specification's initial state is "halted and zeroed" (3.1).
 * The comparators and their interrupts are #593's.
 */

#ifndef _X86_64_TIME_HPET_H_
#define _X86_64_TIME_HPET_H_

#include <stdint.h>

/*
 * Find the block through the ACPI table, map it, check what its capability
 * register says, and start the counter if nobody has.  Returns 1 if there is
 * a usable counter.
 */
int hpet_init(void);

int hpet_present(void);
uint64_t hpet_address(void);		/* physical */
uint32_t hpet_period_fs(void);		/* COUNTER_CLK_PERIOD */
uint64_t hpet_hz(void);			/* 10^15 / period, rounded down */
int hpet_counter_64(void);		/* COUNT_SIZE_CAP */
unsigned hpet_comparators(void);	/* NUM_TIM_CAP + 1 */
uint16_t hpet_vendor(void);
int hpet_started_here(void);		/* the counter was halted and this started it */

/* The counter.  A 32-bit counter reads as its low half, zero-extended. */
uint64_t hpet_read(void);

#endif	/* _X86_64_TIME_HPET_H_ */
