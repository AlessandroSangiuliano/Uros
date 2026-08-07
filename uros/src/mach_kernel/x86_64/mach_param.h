/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <machine/mach_param.h> for x86-64 (#450).
 *
 * The clock tick the machine-independent scheduler assumes.  100 Hz is not
 * a property of the instruction set; it is the rate the timer is programmed
 * to, and #318 is where that decision actually gets made for this target.
 * Stated here because kern/ reads it from the machine.
 */

#ifndef _X86_64_MACH_PARAM_H_
#define _X86_64_MACH_PARAM_H_

#define	HZ	(100)

#endif /* _X86_64_MACH_PARAM_H_ */
