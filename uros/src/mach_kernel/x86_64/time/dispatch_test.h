/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -d: a second wakeup on a dispatching processor does not take the place of
 * a better thread (#787, section 7).
 */

#ifndef _X86_64_TIME_DISPATCH_TEST_H_
#define _X86_64_TIME_DISPATCH_TEST_H_

void dispatch_test(void);

/* Called from the master's tick by the widen's call site only (#787). */
void dispatch_test_tick_probe(void);

#endif	/* _X86_64_TIME_DISPATCH_TEST_H_ */
