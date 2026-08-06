/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Is a thread that never yields actually taken away? (#459, #461)
 *
 * Two entries, because the question has two forms and only the second one is
 * the interesting one now.
 *
 * ⚠️ Declared in a header rather than as an `extern' at the call site, which
 * is how x86_64/thread/machine.c reached preempt_test_run() until #461.  A
 * declaration that matches its caller and nothing else is invisible to the
 * compiler and to the linker both (#448).
 */

#ifndef _X86_64_TIME_PREEMPT_TEST_H_
#define _X86_64_TIME_PREEMPT_TEST_H_

#include <kern/thread.h>

/*
 * The uniprocessor form.  Creates the three threads on THIS processor and
 * answers with the reporter, for load_context() to start in place of the
 * first thread -- the boot has to be given to the test, because the ordinary
 * first thread reaches bootstrap_create() and panics there (#422) in less
 * time than the test needs.
 */
thread_t	preempt_test_run(void);

/*
 * The form that matters on a machine with more than one processor: the three
 * threads on an APPLICATION processor, and this processor waiting for the
 * verdict.
 *
 * It is a stronger claim than the uniprocessor one, and it is the claim #461
 * exists to make.  An application processor that reaches the scheduler and
 * runs an idle thread has been shown to arrive; being preempted there shows
 * that its own clock, its own quantum accounting and its own AST all work --
 * every one of which is per-processor state that has only ever run on the
 * processor the firmware started.
 */
void		preempt_test_run_remote(void);

#endif	/* _X86_64_TIME_PREEMPT_TEST_H_ */
