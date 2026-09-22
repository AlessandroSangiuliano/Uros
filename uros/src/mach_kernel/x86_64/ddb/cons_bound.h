/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The one number every polled UART writer on this target shares (#551).
 *
 * A file of its own, with nothing but the number in it, because boot.S needs
 * it too: the two assembly writers that narrate the hand-off run before there
 * is a C environment, and a constant they could not include would be a second
 * copy that drifts.  What the number means, and how it was measured, is
 * written above the C-side use in ddb/cons.h.
 */

#ifndef _X86_64_DDB_CONS_BOUND_H_
#define _X86_64_DDB_CONS_BOUND_H_

#define CONS_THRE_SPINS	4000

#endif	/* _X86_64_DDB_CONS_BOUND_H_ */
