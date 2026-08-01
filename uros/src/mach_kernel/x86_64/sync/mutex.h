/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The three states of the x86-64 kernel mutex (#452), and the debug notes
 * that ride along with them.  The routines themselves are declared by
 * <kern/lock.h>, which owns mutex_t; see sync/mutex.c for the design.
 */

#ifndef _X86_64_SYNC_MUTEX_H_
#define _X86_64_SYNC_MUTEX_H_

#define	MUTEX_FREE	0	/* nobody holds it                        */
#define	MUTEX_HELD	1	/* held, and no thread has announced itself */
#define	MUTEX_WAIT	2	/* held, and at least one thread will sleep */

/*
 * Owner tracking, under MACH_LDEBUG only.
 *
 * This is not decoration.  The machine-independent tree asserts on
 * mutex_held() in a hundred places, and kern/lock.h builds that macro out of
 * hw_lock_held(&m->locked) *and* m->thread -- a mutex that cannot say which
 * thread holds it turns every one of those assertions into a check of
 * "somebody holds it", which is nearly always true and therefore says
 * nothing.  Silently weakening an assertion is worse than not having it.
 *
 * The notes are stores to a line this thread already owns exclusively, so
 * they cost nothing a release build would notice -- and a release build does
 * not have them at all.
 */
#if	MACH_LDEBUG

#define	MUTEX_NOTE_ACQUIRED(m)						\
MACRO_BEGIN								\
	(m)->thread = (vm_offset_t) current_thread();			\
	(m)->pc     = (vm_offset_t) __builtin_return_address(0);	\
MACRO_END

#define	MUTEX_NOTE_RELEASED(m)						\
MACRO_BEGIN								\
	(m)->thread = 0;						\
MACRO_END

/*
 * About to sleep: the holder stays recorded, because that is precisely the
 * thread a deadlock report needs to name.
 */
#define	MUTEX_NOTE_BLOCKING(m)	/* holder unchanged, on purpose */

#else	/* MACH_LDEBUG */

#define	MUTEX_NOTE_ACQUIRED(m)
#define	MUTEX_NOTE_RELEASED(m)
#define	MUTEX_NOTE_BLOCKING(m)

#endif	/* MACH_LDEBUG */

#endif /* _X86_64_SYNC_MUTEX_H_ */
