/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A read section held across clock ticks while a callback waits (#649, -U).
 */

#ifndef _X86_64_TIME_RCU_TICK_TEST_H_
#define _X86_64_TIME_RCU_TICK_TEST_H_

/*
 * A reader bound to one processor holds a read section open for about ten
 * ticks of that processor, round after round, while this thread queues a
 * callback with urmach_call_rcu() from another; the callback must not run
 * until the reader has left.  Prints "rcu_tick: PASS", "WRONG" or
 * "NOT ASKED", and returns: the boot goes on.
 */
void rcu_tick_reader_test(void);

#endif	/* _X86_64_TIME_RCU_TICK_TEST_H_ */
