/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Is a kernel-mode trap return taking ASTs it has no business taking? (#463)
 *
 * The reasoning is in ast_test.c.  In one line: the fault is a window a few
 * instructions wide, so waiting for it is hopeless -- measured, a build that
 * merely LINKS a new object file without executing it moves the rate from
 * zero in ten to one in ten -- so the test constructs the state instead of
 * hoping for it.
 *
 * ⚠️ Behind `-A' and not on the ordinary boot: it costs a thread that never
 * comes back, and on a kernel with the defect it does not report a failure,
 * it panics.
 */

#ifndef _X86_64_TRAP_AST_TEST_H_
#define _X86_64_TRAP_AST_TEST_H_

void	kernel_ast_test(void);

#endif	/* _X86_64_TRAP_AST_TEST_H_ */
