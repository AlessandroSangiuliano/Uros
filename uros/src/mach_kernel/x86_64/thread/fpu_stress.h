/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Does vector state survive an involuntary switch? (#408)
 *
 * Its own boot, like -D, -C and -P before it, and for the same reason: the
 * ordinary boot stops at bootstrap_create() (#422) within a few milliseconds
 * of starting its first thread, and this needs a second of wall clock.
 *
 * ⚠️ Thread context, after the application processors are in the scheduler --
 * it binds threads to one of them.  Called from machine_processors_ready(),
 * which is the first moment that is true.  Does not return.
 */

#ifndef _X86_64_THREAD_FPU_STRESS_H_
#define _X86_64_THREAD_FPU_STRESS_H_

void	fpu_stress_run(void);

#endif	/* _X86_64_THREAD_FPU_STRESS_H_ */
