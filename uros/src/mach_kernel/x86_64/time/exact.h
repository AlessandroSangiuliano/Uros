/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Which of a clock's exact sources is believed, against what the rulers
 * measured (#508, #594).
 *
 * One rule for every clock that has exact sources: the TSC (time/tsc.c) and
 * the LAPIC timer (cpu/lapic.c).  exact.c states the rule and says where each
 * of its tolerances is argued.
 */

#ifndef _X86_64_TIME_EXACT_H_
#define _X86_64_TIME_EXACT_H_

#include <stdint.h>

#include <time/freq_source.h>

enum {
	EXACT_ABSENT,		/* the source states no rate */
	EXACT_ADOPTED,		/* the first that agrees with the rulers */
	EXACT_AGREES,		/* within 10 ppm of the adopted one */
	EXACT_NOT_USED,		/* agrees with the rulers, not with the adopted one */
	EXACT_CONTRADICTS,	/* further from the rulers than they can be: WRONG */
	EXACT_UNCHECKED,	/* nothing was measured to check it against */
};

struct exact_choice {
	uint64_t	measured;		/* the rulers' verdict, or 0 */
	uint64_t	bound_ppm;		/* how far a source may be from it */
	uint64_t	phase_ppm;		/* the part of it that is a host's
						   NTP slew: 0 on bare metal */
	int		adopted;		/* FREQ_* id, or -1 */
	uint64_t	hz[FREQ_EXACT];
	uint64_t	ppm[FREQ_EXACT];	/* from the measurement */
	uint64_t	apart_ppm[FREQ_EXACT];	/* from the adopted source */
	int		verdict[FREQ_EXACT];
};

/*
 * `hz' is what each source states, in the order <time/freq_source.h> believes
 * them, zero where a source states nothing; `measured' is the rulers' answer
 * for the same clock, zero if they gave none; `bracket_ppm' is the widest
 * bracket behind that answer.  Returns the adopted rate, or zero, and fills
 * `out' with every source's verdict for whoever prints the boot line.
 */
uint64_t exact_choose(const uint64_t hz[FREQ_EXACT], uint64_t measured,
		      uint64_t bracket_ppm, struct exact_choice *out);

#endif	/* _X86_64_TIME_EXACT_H_ */
