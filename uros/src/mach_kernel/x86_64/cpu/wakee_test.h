/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -w: the hand-off on block reaches the thread it names, and that thread does
 * not wait behind another one when its waker leaves (#777).
 */

#ifndef	_CPU_WAKEE_TEST_H_
#define	_CPU_WAKEE_TEST_H_

void wakee_test(void);

#endif	/* _CPU_WAKEE_TEST_H_ */
