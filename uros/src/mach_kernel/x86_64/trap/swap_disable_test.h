/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * #642: thread_swap_disable() on an activation the thread swapper is moving.
 * See the .c for the three arms and what each one asks.
 */

#ifndef	_X86_64_TRAP_SWAP_DISABLE_TEST_H_
#define	_X86_64_TRAP_SWAP_DISABLE_TEST_H_

/*
 * Runs the three arms from the calling thread, which must be able to block, and
 * returns so the boot goes on.  Needs no second processor.  Leaves three
 * waiters and two helper threads waiting for ever.
 */
extern void	swap_disable_test(void);

#endif	/* _X86_64_TRAP_SWAP_DISABLE_TEST_H_ */
