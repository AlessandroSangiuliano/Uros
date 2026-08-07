/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <machine/setjmp.h> for x86-64 (#450).
 *
 * The buffer DDB unwinds through when a command faults: db_recover holds one
 * of these, and the fault handler longjmps back into the debugger instead of
 * letting a bad address in a debugger command take the machine down.
 *
 * What has to be saved is what the SysV AMD64 ABI makes the callee's
 * responsibility -- rbx, rbp and r12-r15 -- plus the stack and return
 * addresses.  i386 saves six 32-bit words for the same reason; x86-64 has
 * four more callee-saved registers and they are 64 bits wide, so the shape
 * is eight longs rather than six ints.  Getting this count wrong does not
 * fail to build: it corrupts registers on the way back out, which is the
 * worst possible behaviour in the one path whose job is to survive a fault.
 *
 * The saving and restoring live with #409, which owns the fault entry.
 */

#ifndef _X86_64_SETJMP_H_
#define _X86_64_SETJMP_H_

typedef struct jmp_buf {
	long	jmp_buf[8];	/* rbx, rbp, r12, r13, r14, r15, rsp, rip */
} jmp_buf_t;

#endif /* _X86_64_SETJMP_H_ */
