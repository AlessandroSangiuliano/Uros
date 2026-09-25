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
 *
 * The comparators below are #593's, and only time/hpet_event.c drives them:
 * the clock-event backend for when the local APIC's timer cannot be used.
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

/*
 * The counter's low 32 bits, in one access.  For an interval shorter than a
 * wrap of them -- 43 s at 100 MHz, five minutes at 14.318 MHz -- this is all
 * a ruler needs, and it is one exit to the host under an emulator where
 * hpet_read() is three.
 */
uint32_t hpet_read32(void);

/* ------------------------------------------------------------------ */
/*  The comparators (#593)                                              */
/* ------------------------------------------------------------------ */

/* LEG_RT_CAP: timer 0 can take the 8254's line and timer 1 the RTC's. */
int hpet_legacy_capable(void);

/* Whether this kernel has switched LegacyReplacement on (2.3.5). */
int hpet_legacy_routed(void);

struct hpet_comparator_caps {
	int		fsb;		/* Tn_FSB_INT_DEL_CAP */
	int		periodic;	/* Tn_PER_INT_CAP */
	int		size_64;	/* Tn_SIZE_CAP */
	uint32_t	route_cap;	/* Tn_INT_ROUTE_CAP: I/O APIC inputs */
};

void hpet_comparator_caps(unsigned n, struct hpet_comparator_caps *out);

/*
 * Configure comparator n one-shot, edge-triggered and 32 bits wide, parked a
 * wrap away, and let it interrupt: as an FSB message of `data' written to
 * `addr', or through LegacyReplacement (n must be 0 or 1, and the block must
 * be legacy-capable; returns 0 otherwise).
 */
void hpet_comparator_fsb(unsigned n, uint32_t addr, uint32_t data);
int hpet_comparator_legacy(unsigned n);

/* Stop comparator n from interrupting. */
void hpet_comparator_off(unsigned n);

/* The match value, against the counter's low 32 bits. */
void hpet_comparator_set(unsigned n, uint32_t value);

#endif	/* _X86_64_TIME_HPET_H_ */
