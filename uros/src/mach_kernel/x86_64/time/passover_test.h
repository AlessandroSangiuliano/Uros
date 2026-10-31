/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -g: a thread woken before it blocks is not passed over (#787, section 3).
 */

#ifndef _X86_64_TIME_PASSOVER_TEST_H_
#define _X86_64_TIME_PASSOVER_TEST_H_

void passover_test(void);

#endif	/* _X86_64_TIME_PASSOVER_TEST_H_ */
