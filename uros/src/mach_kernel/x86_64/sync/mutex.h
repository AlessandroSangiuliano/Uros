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

/*
 * ⚠️ Included here rather than left to the caller: these macros are chosen by
 * MUTEX_OWNER_TRACK, and a translation unit that reached them without this
 * header would silently get the empty half -- which is the whole of what went
 * wrong before.
 */
#include <kern/mutex_track.h>

/*
 * ⚠️ And MACRO_BEGIN/MACRO_END, which the bodies below have always used and
 * this header has never included.  Nobody noticed because the bodies lived
 * under MACH_LDEBUG and MACH_LDEBUG is 0: the branch was never compiled, so
 * its missing dependency was never checked.  Code that is not built does not
 * know it is broken.
 */
#include <kern/macro_help.h>

#define	MUTEX_FREE	0	/* nobody holds it                        */
#define	MUTEX_HELD	1	/* held, and no thread has announced itself */
#define	MUTEX_WAIT	2	/* held, and at least one thread will sleep */

/*
 * Owner tracking.
 *
 * This is not decoration.  The machine-independent tree asserts on
 * mutex_held() in a hundred places, and kern/lock.h builds that macro out of
 * hw_lock_held(&m->locked) *and* m->thread -- a mutex that cannot say which
 * thread holds it turns every one of those assertions into a check of
 * "somebody holds it", which is nearly always true and therefore says
 * nothing.  Silently weakening an assertion is worse than not having it.
 *
 * The notes are stores to a line this thread already owns exclusively, so
 * they cost nothing a release build would notice.
 *
 * 🔥 TWO SWITCHES, AND THIS FILE OBEYED THE WRONG ONE (#476).
 *
 * <kern/mutex_track.h> sets MUTEX_OWNER_TRACK to 1.  That is what puts
 * own_thr/own_pc/ilk_thr into mutex_t, what i386_lock.S writes at its fixed
 * offsets 4/8/12, what kern/lock.h documents as "identify who took it", and
 * what mutex_lock_wait() in kern/lock.c maintains -- it clears ilk_thr there
 * "so a stale ilk_thr with waiters>0 would not point at this window", which
 * is a sentence that only makes sense if something is writing the others.
 *
 * On this machine nothing was.  These macros were spelled under MACH_LDEBUG,
 * which is 0, so every one of them compiled to nothing: the tree paid twelve
 * bytes per mutex for three fields, said in a header that they name the
 * holder, kept one of them up to date from machine-independent code -- and
 * the holder was never recorded at all.  A deadlock report reading own_thr
 * here would have named whatever the heap happened to contain.
 *
 * Found while trying to name the thread holding vm_page_queue_lock in a boot
 * that had stopped, which is exactly the use the header describes.
 *
 * ⚠️ So they follow MUTEX_OWNER_TRACK, the switch that decides whether the
 * fields exist, and MACH_LDEBUG keeps only the pair that is its own --
 * m->thread and m->pc, which live in mutex_t under MACH_LDEBUG and are what
 * mutex_held() reads.  One switch, one set of fields, and neither able to be
 * on while the other is off.
 */
#if	MUTEX_OWNER_TRACK
#define	MUTEX_NOTE_OWNER(m)						\
MACRO_BEGIN								\
	(m)->own_thr = (vm_offset_t) current_thread();			\
	(m)->own_pc  = (vm_offset_t) __builtin_return_address(0);	\
MACRO_END
#define	MUTEX_NOTE_NO_OWNER(m)						\
MACRO_BEGIN								\
	(m)->own_thr = 0;						\
MACRO_END
#else	/* MUTEX_OWNER_TRACK */
#define	MUTEX_NOTE_OWNER(m)
#define	MUTEX_NOTE_NO_OWNER(m)
#endif	/* MUTEX_OWNER_TRACK */

#if	MACH_LDEBUG
#define	MUTEX_NOTE_HELD(m)						\
MACRO_BEGIN								\
	(m)->thread = (vm_offset_t) current_thread();			\
	(m)->pc     = (vm_offset_t) __builtin_return_address(0);	\
MACRO_END
#define	MUTEX_NOTE_UNHELD(m)						\
MACRO_BEGIN								\
	(m)->thread = 0;						\
MACRO_END
#else	/* MACH_LDEBUG */
#define	MUTEX_NOTE_HELD(m)
#define	MUTEX_NOTE_UNHELD(m)
#endif	/* MACH_LDEBUG */

#define	MUTEX_NOTE_ACQUIRED(m)						\
MACRO_BEGIN								\
	MUTEX_NOTE_OWNER(m);						\
	MUTEX_NOTE_HELD(m);						\
MACRO_END

#define	MUTEX_NOTE_RELEASED(m)						\
MACRO_BEGIN								\
	MUTEX_NOTE_NO_OWNER(m);						\
	MUTEX_NOTE_UNHELD(m);						\
MACRO_END

/*
 * About to sleep: the holder stays recorded, because that is precisely the
 * thread a deadlock report needs to name.
 */
#define	MUTEX_NOTE_BLOCKING(m)	/* holder unchanged, on purpose */

#endif /* _X86_64_SYNC_MUTEX_H_ */
