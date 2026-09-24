/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The kernel's rulers, and their vote (#508, phase 4).
 *
 * Three clocks whose rates are known without measuring: the 8254 read back,
 * the ACPI PM timer, the HPET's counter.  The TSC is calibrated against every
 * one the machine has, by the rule in time/ruler.c, and then they vote: a
 * ruler that disagrees with the other two is named and not used.  With one
 * ruler, a ruler that is wrong is wrong twice (time/tsc.c said so of the
 * 8254); with two that disagree, nothing can say which; with three, two that
 * agree outvote the one that does not.
 *
 * ⚠️ UNDER A HYPERVISOR THEY ARE NOT INDEPENDENT WITNESSES.  #508 measured it:
 * QEMU drives all three from the host's clock, which the host's own kernel
 * disciplines with NTP, so they move together from one boot to the next and
 * agree with each other while all of them follow the host.  The vote still
 * catches a ruler that is broken in the guest -- a wrong period, a wrong
 * width, an emulation that programs itself inside its interval -- and that
 * is what it is for; it cannot catch a host whose clock is wrong for all
 * three at once.
 */

#ifndef _X86_64_TIME_RULERS_H_
#define _X86_64_TIME_RULERS_H_

#include <stdint.h>

#include <time/ruler.h>

enum { RULER_8254, RULER_PM, RULER_HPET, RULERS };

struct kernel_ruler {
	const char			*name;
	struct ruler			r;
	int				present;
	uint64_t			span;	/* thirty milliseconds of it */
	struct ruler_calibration	tsc;	/* the TSC against it */
	int				dissents;
};

/* Find them: the 8254 always, the others where the tables describe one. */
void rulers_find(void);

struct kernel_ruler *rulers_get(unsigned id);

/* The 8254 has to be set running to be read back; the others run anyway. */
void rulers_start(unsigned id);
void rulers_stop(unsigned id);

/*
 * The vote, over the TSC as each ruler measured it.
 *
 * Two rulers AGREE when they differ by no more than the mean of their widest
 * brackets plus 1000 ppm: each end can be off by half its bracket, and each
 * ruler's own rate by 500 ppm, the accuracy IA-PC HPET 1.0a requires over a
 * millisecond or more (2.4.1).  Derived, not chosen, so that it is as tight
 * as the rulers can promise and no tighter.
 *
 * Three that answer: the median when at least two pairs agree; the mean of
 * the one pair that agrees, with the third named, when only one does; and
 * when none does, the ruler whose brackets were narrowest is kept and the
 * boot line says there was no majority.  Two: their mean if they agree,
 * otherwise the narrower one's, said the same way.  One: its own value, and
 * no vote.  None: zero, and every consumer says NOT ASKED (#586).
 */
struct rulers_verdict {
	uint64_t	hz;
	unsigned	answered;	/* rulers that produced a median */
	int		dissenter;	/* named and not used, or -1 */
	int		no_majority;	/* answered >= 2 and no two agreed */
	int		kept;		/* whose value was kept without a majority */
};

void rulers_vote(struct rulers_verdict *v);
const struct rulers_verdict *rulers_verdict(void);

/* The ruler the LAPIC timer is calibrated against: the narrowest that was
 * not named, or the 8254 if none answered. */
unsigned rulers_elected(void);

/* A ruler that lasts a second -- the PM timer or the HPET -- that answered
 * and was not named, for the refinement; -1 if there is none. */
int rulers_long(void);

#endif	/* _X86_64_TIME_RULERS_H_ */
