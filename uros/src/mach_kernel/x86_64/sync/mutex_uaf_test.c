/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -u: the x86-64 mutex's release is its last access to the mutex (#753).
 *
 * #753 was a write after free: mutex_unlock() swapped the word free and then
 * went back to the mutex for the interlock and the count of sleepers.  A lock
 * decides the life of the object around it -- task_deallocate() drops a
 * reference under the task's lock and frees the task when it was the last --
 * so another processor, taking the lock on the fast path the instant the word
 * read free, could free the object before the release's second access, which
 * then wrote freed memory (the interlock byte over the zone's free-list
 * pointer).  The fix makes the release's swap its last access, and wakes by
 * the event's address without reading the mutex again.
 *
 * This checks that property directly, and without racing the 1-in-1800 window.
 * It stands in for the freeing processor deterministically: a probe, called
 * from inside mutex_unlock() right after the swap, scribbles a sentinel over
 * the whole mutex -- exactly what a free and reuse would leave there -- and
 * records that it fired.  Whatever the release does after that point is then
 * visible in the mutex: the fix touches it no more, so the sentinel stands;
 * the #753 defect takes the interlock and decrements the count, so the
 * sentinel is clobbered.
 *
 * ⚠️ The probe's call site is NOT in the shipped kernel.  It is added by
 * ablations/753-widen-release-window.patch, so that the ablation that reverts
 * the fix is caught at will; a build without the widen never calls the probe,
 * and the test says NOT ASKED there -- the property holds by construction in
 * the shipped release, and the A/B is what proves the ablation is caught
 * (#691: NOT ASKED is a verdict, not a miss).
 */

#include <stdint.h>

#include <kern/lock.h>
#include <kern/misc_protos.h>		/* printf */
#include <kern/thread.h>		/* current_thread */

#include <sync/mutex.h>			/* MUTEX_FREE/HELD/WAIT, the field layout */
#include <sync/mutex_uaf_test.h>

/* A marker the release cannot produce on its own: no count and no address. */
#define MUTEX_UAF_SENTINEL	((short) 0x5EED)

volatile int		mutex_uaf_armed;	/* the test sets this */
static mutex_t		*mutex_uaf_target;	/* and names the mutex */
static volatile int	mutex_uaf_fired;	/* the probe set this */

void
mutex_uaf_probe(mutex_t *m)
{
	if (!mutex_uaf_armed || m != mutex_uaf_target)
		return;

	/*
	 * The free, standing in for the processor that took the lock the
	 * instant the word read free.  The interlock is left clear so the
	 * defect's hw_lock_lock() does not spin on it -- it takes it, which is
	 * the clobbering write -- and the count is set to the sentinel, which
	 * the defect's mutex_unlock_wakeup() then decrements.  A release that
	 * touches the mutex no more leaves both as they are here.
	 */
	m->interlock = 0;
	m->waiters   = MUTEX_UAF_SENTINEL;
	mutex_uaf_fired = 1;
}

void
mutex_uaf_test(void)
{
	static mutex_t	tm;
	short		after;

	printf("mutex_uaf_test: starting (#753)\n");

	mutex_init(&tm, ETAP_NO_TRACE);
	mutex_lock(&tm);

	/*
	 * Stand in for an announced waiter, so the release takes the slow path
	 * -- the one that, before the fix, read and wrote the mutex after the
	 * swap.  No thread is queued, so the wakeup the slow path does finds
	 * nobody; the point is the access, not the wakeup.
	 */
	tm.locked   = MUTEX_WAIT;
	tm.waiters  = 1;

	mutex_uaf_target = &tm;
	mutex_uaf_fired  = 0;
	mutex_uaf_armed  = 1;

	mutex_unlock(&tm);

	mutex_uaf_armed  = 0;
	mutex_uaf_target = (mutex_t *) 0;
	after = tm.waiters;

	if (!mutex_uaf_fired) {
		printf("mutex_uaf_test: NOT ASKED -- the release-window probe is "
		       "not in this build (no widen); the shipped release makes "
		       "its swap its last access by construction (#753)\n");
		return;
	}

	if (after != MUTEX_UAF_SENTINEL)
		printf("mutex_uaf_test: WRONG -- the release touched the mutex "
		       "after its swap freed the word: a processor could have "
		       "freed the object in between, as #753's write after free "
		       "(waiters 0x%x, not the 0x%x left at the free)\n",
		       (unsigned short) after, (unsigned short) MUTEX_UAF_SENTINEL);
	else
		printf("mutex_uaf_test: PASS -- after its swap freed the word the "
		       "release did not touch the mutex again, so a free in that "
		       "window is not written over (#753)\n");
}
