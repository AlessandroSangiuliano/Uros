/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <machine/lock.h> for x86-64 (#450).
 *
 * The machine-independent tree reaches the MD layer through flat names under
 * machine/, while x86_64/ is one directory per MD contract.  This forwards
 * rather than moving the file, so #410 keeps its own directory and the
 * MI->MD contract surface stays something you can enumerate: every header
 * at this level names exactly what satisfies it.
 */

#ifndef _X86_64_LOCK_H_
#define _X86_64_LOCK_H_

#include <sync/lock.h>

#endif /* _X86_64_LOCK_H_ */
