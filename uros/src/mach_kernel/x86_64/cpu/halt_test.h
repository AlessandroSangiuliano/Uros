/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Two processors panic at the same instant (#599, -Z).
 */

#ifndef _X86_64_CPU_HALT_TEST_H_
#define _X86_64_CPU_HALT_TEST_H_

/*
 * Makes this processor and another panic together, from thread context with
 * interrupts on.  Does not return when it runs; prints "double_panic: NOT
 * ASKED" or "WRONG" and returns when it cannot pose the question.  The verdict
 * is the log's: scripts/double-panic-check.sh.
 */
void double_panic_test(void);

#endif	/* _X86_64_CPU_HALT_TEST_H_ */
