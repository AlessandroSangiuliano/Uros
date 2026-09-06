/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * #490: a thread must not be put to sleep between declaring its wait and
 * releasing the lock it still holds.  See the .c for the window and for why
 * this constructs the interleaving rather than waiting for it.
 */

#ifndef	_X86_64_TRAP_WAIT_PREEMPT_TEST_H_
#define	_X86_64_TRAP_WAIT_PREEMPT_TEST_H_

/*
 * Runs a bound probe on another processor and returns, so the boot goes on.
 * Needs more than one processor running and a calibrated TSC; says so and
 * returns if it has neither.
 */
extern void	kernel_wait_preempt_test(void);

#endif	/* _X86_64_TRAP_WAIT_PREEMPT_TEST_H_ */
