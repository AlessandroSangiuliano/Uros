/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Where the fields of a trap frame sit, for code that cannot see the struct
 * (#474).
 *
 * The syscall entry has to WRITE that frame rather than build one by pushing:
 * it arrives with %rsp at the frame's base and fills five words in place.  So
 * it needs the offsets as constants, and a constant in a .S file is the
 * classic second half that nothing compares with the first.
 *
 * ⚠️ Which is why this header holds ONLY numbers.  <trap/trap.h> defines the
 * structure and asserts every one of them against __builtin_offsetof, so the
 * two cannot drift: adding a field to that structure moves the real offsets
 * and the build stops here rather than in a boot.  #448 is the reason the
 * assertion is the point and the numbers are an implementation detail of it.
 *
 * Safe to include from assembly: nothing below is C.
 */

#ifndef _X86_64_TRAP_FRAME_H_
#define _X86_64_TRAP_FRAME_H_

#define	TF_RIP		136
#define	TF_CS		144
#define	TF_RFLAGS	152
#define	TF_RSP		160
#define	TF_SS		168

#endif	/* _X86_64_TRAP_FRAME_H_ */
