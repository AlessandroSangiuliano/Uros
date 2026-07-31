/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <machine/xpr.h> for x86-64 (#450).
 *
 * The timestamp the XPR trace buffer stamps each entry with.  Zero until
 * there is a clock worth reading from every core -- which is exactly the
 * question #318 exists to answer.  A plausible-looking counter here would
 * make every trace carry a number nobody had checked.
 */

#ifndef _X86_64_XPR_H_
#define _X86_64_XPR_H_

#define	XPR_TIMESTAMP	(0)

#endif /* _X86_64_XPR_H_ */
