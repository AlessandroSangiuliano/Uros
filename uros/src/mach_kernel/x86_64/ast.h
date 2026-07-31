/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <machine/ast.h> for x86-64 (#450).
 *
 * The one AST bit the machine owns: a thread that has used the FPU carries
 * it so the context switch knows to save state.  The bit's value is the
 * architecture's to choose -- it lives in the same word as the MI reasons --
 * and it is named for this architecture rather than inherited, because a bit
 * called AST_I386_FP on x86-64 would be a lie the compiler cannot catch.
 */

#ifndef _X86_64_AST_H_
#define _X86_64_AST_H_

#define	AST_X86_64_FP		0x80000000

#define	MACHINE_AST_PER_THREAD	AST_X86_64_FP

#endif /* _X86_64_AST_H_ */
