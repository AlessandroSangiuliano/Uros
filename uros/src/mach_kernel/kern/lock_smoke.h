/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The boot-time acceptance checks for the lock primitives and for spl.
 *
 * Both are machine-independent -- they test what <kern/lock.h> and
 * <kern/spl.h> promise, on whatever machine is underneath -- and both are
 * called once from setup_main().
 *
 * ⚠️ This header exists so that they are declared somewhere both the caller
 * and kern/lock_smoke.c can see.  They used to be `extern' declarations
 * inside setup_main()'s body, which is the shape #448 was: two halves that
 * agree with themselves and are never compared with each other.  The C
 * language will not check a declaration against a definition it never sees
 * in the same translation unit, so the compare has to be arranged (#453).
 */

#ifndef	_KERN_LOCK_SMOKE_H_
#define	_KERN_LOCK_SMOKE_H_

/* #303 acceptance: a contended counter through simple_lock/simple_unlock. */
extern void	lock_smoke_test(void);

/* #410 acceptance: splx() returns, and returns to the level it was given. */
extern void	spl_return_check(void);

#endif	/* _KERN_LOCK_SMOKE_H_ */
