/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef	_I386_FPU_STRESS_H_
#define	_I386_FPU_STRESS_H_

/*
 * Ask whether the switch carries a thread's floating-point state (#560).
 *
 * Armed with the `-F' boot argument, which is the name this test has on both
 * targets.  Prints one PASS line or one WRONG line per thread that came back
 * with something other than what it was holding.
 */
extern void	fpu_stress_run(void);

#endif	/* _I386_FPU_STRESS_H_ */
