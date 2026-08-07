/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Does the flavour dispatch do what kern/thread_act.c assumes? (#408)
 *
 * The reasoning is in state_test.c.  In one line: kern/thread_act.c checks
 * neither the flavour nor the count before handing both to this machine, so
 * every check there is has to be here — and none of it had ever run, because
 * no thread on this target has been to ring 3 for it to be asked about.
 *
 * Runs on every boot rather than behind a flag: it needs no scheduler, no
 * second processor and no user space, and a test that runs only when asked
 * for is a test that mostly does not run.
 */

#ifndef _X86_64_THREAD_STATE_TEST_H_
#define _X86_64_THREAD_STATE_TEST_H_

void	thread_state_dispatch_test(void);

/*
 * The machine-independent entry points on top of it: thread_get_state() and
 * thread_set_state() themselves, against a thread that is genuinely stopped.
 *
 * ⚠️ Behind `-G' and not on the ordinary boot, because it costs a thread
 * parked in a wait nobody will signal: thread_stop_wait() waits for TH_RUN to
 * clear, and a kernel thread that spins never clears it.
 */
void	thread_state_entry_test(void);

#endif	/* _X86_64_THREAD_STATE_TEST_H_ */
