/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -z: thread_doassign()'s retry does not lock the default set's mutex twice
 * (#787, section 4).
 */

#ifndef _X86_64_CPU_DOASSIGN_TEST_H_
#define _X86_64_CPU_DOASSIGN_TEST_H_

void doassign_test(void);

/* Called from thread_doassign() by the widen's call site only (#787). */
void doassign_probe(thread_t t);

#endif	/* _X86_64_CPU_DOASSIGN_TEST_H_ */
