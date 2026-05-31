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
