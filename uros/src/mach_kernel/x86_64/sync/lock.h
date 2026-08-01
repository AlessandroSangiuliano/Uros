/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 hardware lock (#410, MD contract 5/6).
 *
 * The lowest level the machine-independent tree names: kern/lock.h builds
 * usimple, simple and mutex on top of exactly these five operations, so the
 * names and the return conventions are its, not ours — try answers non-zero
 * when it took the lock, held answers non-zero when the lock is set.
 *
 * The lock word keeps i386's shape, a single byte, because the
 * machine-independent structures that embed one take their size from this
 * typedef and there is nothing to gain from widening it.
 */

#ifndef _X86_64_SYNC_LOCK_H_
#define _X86_64_SYNC_LOCK_H_

#include <stdint.h>

typedef volatile uint8_t	hw_lock_data_t;
typedef hw_lock_data_t		*hw_lock_t;

void         hw_lock_init(hw_lock_t l);
void         hw_lock_lock(hw_lock_t l);
void         hw_lock_unlock(hw_lock_t l);
unsigned int hw_lock_try(hw_lock_t l);
unsigned int hw_lock_held(hw_lock_t l);


/*
 * The mutex, as the machine-independent tree spells it (#452).
 *
 * kern/lock.h declares _mutex_lock / _mutex_try / mutex_unlock and owns
 * mutex_t; what a machine supplies is the outer names and whatever fast path
 * it wants behind them.  i386 puts an inline xchg here and falls back to the
 * function; on x86-64 the function *is* the fast path -- one cmpxchg and a
 * return -- so a second copy inlined at 88 call sites would buy nothing and
 * cost the instruction cache.
 *
 * mutex_lock_assert_safe() is the machine-independent check that this thread
 * is allowed to block here at all; it lives in kern/lock.c and is declared
 * rather than included because <kern/thread.h> would close a cycle back
 * through <kern/lock.h>.
 */
extern void	mutex_lock_assert_safe(void);

#define	mutex_try(m)		_mutex_try(m)
#define	mutex_lock(m)							\
MACRO_BEGIN								\
	mutex_lock_assert_safe();					\
	_mutex_lock((m));						\
MACRO_END

#endif	/* _X86_64_SYNC_LOCK_H_ */
