/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -h: a thread woken onto another processor's queue, better than what runs
 * there, does not wait for that processor's tick (#787, section 7).
 */

#ifndef _X86_64_TIME_BOUND_IPI_TEST_H_
#define _X86_64_TIME_BOUND_IPI_TEST_H_

void bound_ipi_test(void);

#endif	/* _X86_64_TIME_BOUND_IPI_TEST_H_ */
