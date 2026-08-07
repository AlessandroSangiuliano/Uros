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

#include <sync/atomic.h>	/* atomic_add64, under the counters below */

typedef volatile uint8_t	hw_lock_data_t;
typedef hw_lock_data_t		*hw_lock_t;

void         hw_lock_init(hw_lock_t l);
void         hw_lock_lock(hw_lock_t l);
void         hw_lock_unlock(hw_lock_t l);
unsigned int hw_lock_try(hw_lock_t l);
unsigned int hw_lock_held(hw_lock_t l);


/*
 * The address of a hardware lock's word, for code that wants to read the
 * value rather than operate on the lock.
 *
 * It exists because a machine is free to make hw_lock_data_t a struct: the
 * machine-independent tree writes *hw_lock_addr(l) and gets the word either
 * way.  Here the type is already the word, so this is the identity -- but
 * the indirection is the contract, and kern/lock.c uses it in three places
 * to print lock state under DDB.
 */
#define	hw_lock_addr(hwl)	(&(hwl))

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

/*
 * Counters the machine-independent tree bumps without a lock (#453).
 *
 * Typed on `long` and not on a fixed width, which is the whole point: the
 * machine-independent callers used to cast whatever they had to `long *`,
 * and a cast is exactly what let a 32-bit field be handed to a 64-bit
 * read-modify-write.  The casts are gone and the counters are declared as
 * long, so a mismatch is now a compiler error rather than four neighbouring
 * bytes changing value.
 *
 * atomic_incl() answers nothing; atomic_add_fetchl() answers the value
 * *after* the addition, which is what a release needs -- it has to know
 * whether the reference it just dropped was the last one, and the value
 * before the drop cannot say that without a second read that another
 * processor could slip between.
 */
static inline void atomic_incl(long *p, long delta)
{
	(void) atomic_add64((volatile uint64_t *) p, (uint64_t) delta);
}

static inline void atomic_decl(long *p, long delta)
{
	(void) atomic_add64((volatile uint64_t *) p, (uint64_t) -delta);
}

static inline long atomic_add_fetchl(long *p, long delta)
{
	return (long) atomic_add64((volatile uint64_t *) p, (uint64_t) delta)
	       + delta;
}

#endif	/* _X86_64_SYNC_LOCK_H_ */
