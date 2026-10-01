/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef	_I386_AT386_COM_DIVISOR_TEST_H_
#define	_I386_AT386_COM_DIVISOR_TEST_H_

/*
 * May a byte the console sends land in COM1's divisor latch? (#599)
 *
 * Armed with the `-U' boot argument.  Starts a thread and returns: the
 * question needs a second processor, and on this target none exists until
 * the end of start_kernel_threads().  Prints one PASS, WRONG or NOT ASKED
 * line when it has an answer.
 */
extern int	com_divisor_test_wanted;
extern void	com_divisor_test_start(void);

#endif	/* _I386_AT386_COM_DIVISOR_TEST_H_ */
