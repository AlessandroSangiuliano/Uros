/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A census of every thread, printed once the machine has stopped (#476).
 * See kern/quiet_census.c for why this exists and why gdb cannot do it.
 */

#ifndef _KERN_QUIET_CENSUS_H_
#define _KERN_QUIET_CENSUS_H_

/*
 * One pass of an idle processor, and one processor finding work again.
 *
 * ⚠️ Both take the processor number, and that is a correction.  The first
 * version counted on the boot processor and let ANY processor reset the
 * count, so with four of them waking on every tick the counter could not
 * accumulate at all -- and the failure was silence, which is what an absence
 * always looks like.  One processor owns the count on both sides.
 */
extern void	quiet_census_pass(int mycpu);
extern void	quiet_census_busy(int mycpu);

#endif	/* _KERN_QUIET_CENSUS_H_ */
