/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Boolean type, for x86-64 (#413).
 *
 * `int`, which is thirty-two bits here as it was there.  A boolean_t is a
 * field in messages the kernel and its servers exchange, so its width is
 * part of the wire format and not a matter of taste — widening it to the
 * register size would move every field that follows one in a structure, to
 * carry one bit.
 */

#ifndef	_MACH_X86_64_BOOLEAN_H_
#define _MACH_X86_64_BOOLEAN_H_

typedef int		boolean_t;

#endif	/* _MACH_X86_64_BOOLEAN_H_ */
