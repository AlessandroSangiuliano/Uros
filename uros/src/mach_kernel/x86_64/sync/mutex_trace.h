/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Every transition one mutex goes through, kept so the interleaving can be
 * READ instead of imagined (#476).
 *
 * The state that stops the boot is known: a thread asleep on
 * vm_page_queue_lock, the word FREE, the owner clear, and waiters still 1.
 * Nobody will wake it.  What is NOT known is the order of operations that
 * arrives there -- and it has now resisted being derived from the algorithm
 * twice, which is two times more than reading a log would have cost.
 *
 * ⚠️ One mutex, chosen at run time by pointer, and that is what makes this
 * affordable.  Every mutex in the system would fill any ring in microseconds
 * and the interesting part would be gone before the boot reached userland;
 * this records only the lock the failure names, so the ring still holds the
 * beginning of the story when the machine stops.
 *
 * ⚠️ And it SATURATES rather than wrapping.  A wrapping ring is written by the
 * idle loop and by every page fault after the wedge, so by the time anything
 * reads it the entries that mattered have been overwritten by the entries
 * that did not.  Saturating keeps the first N, which is where a lost wakeup
 * happens.
 */

#ifndef _X86_64_SYNC_MUTEX_TRACE_H_
#define _X86_64_SYNC_MUTEX_TRACE_H_

#include <stdint.h>
#include <kern/mutex_track.h>

/*
 * What happened.  Named for the decision each one records rather than for the
 * instruction, because what is being reconstructed is who decided what with
 * which word in front of them.
 */
#define	MTR_LOCK_FAST	1	/* cmpxchg took it, uncontended            */
#define	MTR_ANNOUNCE	2	/* swap wrote WAIT; `arg' is what was there */
#define	MTR_ILK_FREE	3	/* under the interlock the word was FREE    */
#define	MTR_SLEEP	4	/* about to sleep; `arg' is waiters after ++ */
#define	MTR_WOKE		5	/* back from sleeping                       */
#define	MTR_UNLOCK	6	/* swap wrote FREE; `arg' is what was there */
#define	MTR_WAKEUP	7	/* woke one; `arg' is waiters after --       */
#define	MTR_UNLOCK_QUIET	8	/* released, nobody announced          */

/*
 * Off by default, and the reason is where the hooks are.
 *
 * The sequence #476 turned on is `lock-fast' followed by `unlock-quiet' --
 * the two branches that exist precisely so that an uncontended pair is two
 * atomics and nothing else.  A trace that could not see them would not have
 * found it, so the hooks have to be there, and there is exactly where 253
 * call sites in the machine-independent tree would pay for them: a load and
 * a compare on every mutex operation in the kernel.
 *
 * Worth paying while hunting, not worth shipping.  Set this to 1, rebuild,
 * and call mutex_trace_watch() on the lock in question.
 *
 * ⚠️ NOTHING IN THE TREE CALLS mutex_trace_watch(), and that is deliberate.
 * The lock worth watching is different every time, and the natural place to
 * arm it -- beside its mutex_init() -- is machine-independent code, which
 * must not reach a header that exists only under x86_64/.  It did once: a
 * line in vm/vm_resident.c broke the i386 build outright, and the i386 suites
 * went on "passing" because run-qemu.sh had a stale kernel to boot.  A build
 * that fails must not be able to look like a run that succeeded.
 *
 * So the hunter adds the call where the hunt needs it and takes it out again.
 *
 * ⚠️ AND BUILD IT after turning it on, before believing anything it says.
 * The bodies below are not compiled while this is 0, and code that is not
 * compiled does not know it is broken -- which is not a general worry but a
 * thing that happened, twice, in the week this was written: <sync/mutex.h>
 * had used MACRO_BEGIN without including <kern/macro_help.h> for as long as
 * it had existed, and nobody found out because the branch that used it lived
 * under a switch that was off.
 */
#ifndef	MUTEX_TRACE_ON
#define	MUTEX_TRACE_ON	0
#endif

#if	MUTEX_TRACE_ON

extern void	mutex_trace_watch(void *m);
extern void	mutex_trace(void *m, int what, uint64_t arg);
extern void	mutex_trace_report(void);

#define	MUTEX_TRACE(m, what, arg)	mutex_trace((m), (what), (uint64_t)(arg))

#else	/* MUTEX_TRACE_ON */

#define	mutex_trace_watch(m)		((void) (m))
#define	mutex_trace_report()
#define	MUTEX_TRACE(m, what, arg)

#endif	/* MUTEX_TRACE_ON */

#endif	/* _X86_64_SYNC_MUTEX_TRACE_H_ */
