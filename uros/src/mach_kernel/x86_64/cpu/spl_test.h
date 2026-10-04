/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A raise from level zero, preempted half-way (#526, -E).
 */

#ifndef _X86_64_CPU_SPL_TEST_H_
#define _X86_64_CPU_SPL_TEST_H_

/*
 * Threads that are free to move between processors raise their level from
 * zero to SPLHI and back, as fast as they can, for two seconds, and check
 * after every raise that the processor they are on is the one at SPLHI.
 * Prints "spl_test: PASS", "WRONG" or "NOT ASKED", and returns: the boot goes
 * on.
 */
void spl_raise_split_test(void);

#endif	/* _X86_64_CPU_SPL_TEST_H_ */
