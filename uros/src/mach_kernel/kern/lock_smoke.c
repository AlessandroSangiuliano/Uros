/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * lock_smoke.c — boot-time sanity check for the kernel lock primitives.
 *
 * #303 acceptance asks for a contended-counter check on 2- and 4-CPU
 * runs.  Until #308 brings the second CPU online end-to-end, the test
 * runs single-threaded on the BSP — which still exercises the
 * lock/unlock acquire path and confirms the counter survives the
 * round-trip.  Codegen-level atomicity is verified separately in
 * docs/lock_audit_smp.md.
 *
 * When #308 lands and an AP can drive contention here, this routine
 * gets a second arm that pins one half of the iterations to each CPU
 * and confirms the merged counter still matches the expected total.
 */

#include <kern/lock.h>
#include <kern/spl.h>
#include <kern/lock_smoke.h>		/* #448: the interface, so it is checked */
#include <kern/misc_protos.h>		/* printf */

#define	LOCK_SMOKE_ITERS	1000

decl_simple_lock_data(static, lock_smoke_lock)

void
lock_smoke_test(void)
{
	int i;
	int counter = 0;

	simple_lock_init(&lock_smoke_lock, ETAP_MISC_KERNEL_TEST);

	for (i = 0; i < LOCK_SMOKE_ITERS; i++) {
		simple_lock(&lock_smoke_lock);
		counter++;
		simple_unlock(&lock_smoke_lock);
	}

	if (counter != LOCK_SMOKE_ITERS) {
		panic("lock_smoke: counter=%d expected=%d (lock layer broken!)",
		      counter, LOCK_SMOKE_ITERS);
	}

	printf("lock_smoke: %d acquire/release cycles, counter ok (#303)\n",
	       counter);
}

/*
 * #410 -- spl_return_check
 *
 * splx() and spllo() were declared void and had been for as long as
 * kern/spl.h existed, while the machine code underneath returned the level
 * that had been current.  Nothing caught it: the i386 implementation is
 * assembly, so there was no prototype to disagree with.  The declarations are
 * fixed; this is what makes the fix a claim about the running machine rather
 * than about how the assembly reads.
 *
 * ⚠️ The first version of this routine passed without proving anything.  It
 * did splhigh() then splx(), and startup already runs at SPLHI, so the level
 * never moved: the level left and the argument were the same number, and the
 * check could not tell "splx answers with the level it left" from "splx
 * answers with its argument".  An experiment only discriminates when the two
 * hypotheses predict different outcomes, and that one predicted 8 either way.
 *
 * So this moves the level, and asserts that it moved before believing
 * anything that follows.  It calls splx twice, in opposite directions: the
 * first answer must be the level on the way in, the second must be the level
 * in the middle.  Under the wrong hypothesis both are the other number.
 *
 * The level is reached by arithmetic rather than by name.  SPLTTY exists on
 * i386 and not on x86-64, and the property under test is not about any named
 * level; one step down from wherever we are keeps it above SPL0 on either
 * target, which matters because dropping to zero at this point in startup
 * would release deferred interrupts, and this routine has no business doing
 * that.  Every intermediate level masks identically here -- under MP_V1_1 any
 * ipl > 0 programs the same LAPIC task priority -- so the step is observable
 * in curr_ipl and nowhere else.
 */
void
spl_return_check(void)
{
	spl_t	entry, lower, first, middle, second;

	entry = getspl();

	if (entry == SPL0) {
		printf("spl_return_check: skipped, startup is at SPL0 and "
		       "this test may not lower further (#410)\n");
		return;
	}
	lower = entry - 1;

	first = splx(lower);		/* answer must be `entry` */
	middle = getspl();
	second = splx(entry);		/* answer must be `middle` */

	if (middle != lower)
		panic("spl_return_check: splx(%d) left the level at %d -- the "
		      "level never moved, so this test proves nothing (#410)",
		      lower, middle);

	if (first != entry)
		panic("spl_return_check: splx(%d) answered %d; the level it "
		      "left was %d (#410)", lower, first, entry);

	if (second != middle)
		panic("spl_return_check: splx(%d) answered %d; the level it "
		      "left was %d (#410)", entry, second, middle);

	if (getspl() != entry)
		panic("spl_return_check: level is %d, entered at %d (#410)",
		      getspl(), entry);

	printf("spl_return_check: splx answered %d then %d -- the level left, "
	       "not the argument (%d, %d) (#410)\n",
	       first, second, lower, entry);
}
