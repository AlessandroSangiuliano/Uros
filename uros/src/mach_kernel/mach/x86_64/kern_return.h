/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Machine-dependent kernel return definitions, for x86-64 (#413).
 *
 * `int`, thirty-two bits.  A kern_return_t is what every RPC in the system
 * answers with, so it sits in every reply message; and the values it takes
 * are a small enumeration that has never needed more room.
 */

#ifndef	_MACH_X86_64_KERN_RETURN_H_
#define _MACH_X86_64_KERN_RETURN_H_

#ifndef	ASSEMBLER
typedef	int		kern_return_t;
#endif	/* ASSEMBLER */

#endif	/* _MACH_X86_64_KERN_RETURN_H_ */
