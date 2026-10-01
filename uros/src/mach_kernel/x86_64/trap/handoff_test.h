/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * #607: the futex hand-off must wake a waiter it does not switch to the way
 * every other wakeup does.  See the .c for the two arms and why each is built
 * rather than waited for.
 */

#ifndef	_X86_64_TRAP_HANDOFF_TEST_H_
#define	_X86_64_TRAP_HANDOFF_TEST_H_

/*
 * Runs both arms from the calling thread, which must be able to block, and
 * returns so the boot goes on.  Needs no second processor.  Leaves two threads
 * waiting for ever on an event nobody signals.
 */
extern void	handoff_wake_test(void);

#endif	/* _X86_64_TRAP_HANDOFF_TEST_H_ */
