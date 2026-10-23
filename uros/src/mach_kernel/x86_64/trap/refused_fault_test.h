/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -j: a fault from ring 0 on a task's present page that the processor's
 * protection refused is reported, not retried for ever (#750).
 */

#ifndef _X86_64_TRAP_REFUSED_FAULT_TEST_H_
#define _X86_64_TRAP_REFUSED_FAULT_TEST_H_

void refused_fault_test(void);

#endif	/* _X86_64_TRAP_REFUSED_FAULT_TEST_H_ */
