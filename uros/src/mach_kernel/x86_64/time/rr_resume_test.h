/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -k: a round-robin thread preempted by a wakeup resumes before its peer
 * (#787, section 6).
 */

#ifndef _X86_64_TIME_RR_RESUME_TEST_H_
#define _X86_64_TIME_RR_RESUME_TEST_H_

void rr_resume_test(void);

#endif	/* _X86_64_TIME_RR_RESUME_TEST_H_ */
