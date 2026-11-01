/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -z: thread_doassign()'s retry does not lock the default set's mutex twice
 * (#787, section 4).
 *
 * A thread of the default set assigned to a set destroyed in the window
 * before thread_doassign() checks it reaches the retry, which substitutes the
 * default set for the one gone -- and then the set the thread is already in
 * equals the set it is being moved to.  The branch that locks them in order
 * then locked the one mutex twice and held it for good, and with it every
 * thread and task creation.  The fix returns early when the two are the same.
 *
 * doassign_probe(), called inside thread_doassign() after the thread is
 * stopped and before the lock loop, lets the test destroy the set in exactly
 * that window: it signals the test and waits.  Its call site is not in the
 * kernel; a widen adds it, and without the widen the probe never fires and the
 * test says NOT ASKED.  With the widen and the fix, the assignment comes back
 * (the thread stays in the default set); with the fix taken out it deadlocks,
 * and the test, watching from another thread, says so.
 */

#include <kern/host.h>			/* realhost */
#include <kern/lock.h>			/* mutex_pause */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/spl.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_act.h>
#include <kern/time_out.h>		/* timeout_ticks */

#include <mach/mach_host_server.h>	/* thread_assign, processor_set_* */

#include <cpu/doassign_test.h>

#define	DOASSIGN_WAIT	300		/* ticks: three seconds at 100 Hz */

static processor_set_t		doassign_set;
static thread_t			doassign_victim;
static volatile int		doassign_armed;
static volatile int		doassign_at_window;
static volatile int		doassign_release;
static volatile int		doassign_driver_done;
static volatile int		doassign_parked;
static volatile int		doassign_stop;
static volatile int		doassign_gone;
static int			doassign_event;
static kern_return_t		doassign_assign_kr;

/*
 * Called from thread_doassign() by the widen's call site only (#787).  For
 * the victim's assignment, hold it in the window the test needs -- between
 * the stop and the lock loop, with no set locked -- so the test can destroy
 * the set there.  A no-op for every other thread_doassign().
 */
void
doassign_probe(thread_t t)
{
	natural_t	t0;

	if (!doassign_armed || t != doassign_victim)
		return;
	doassign_armed = 0;
	doassign_at_window = 1;
	t0 = timeout_ticks;
	while (!doassign_release && timeout_ticks - t0 < DOASSIGN_WAIT)
		cpu_pause();
}

static void
doassign_victim_fn(void)
{
	thread_t	self = current_thread();

	doassign_victim = self;
	while (!doassign_stop) {
		assert_wait((event_t) &doassign_event, FALSE);
		if (doassign_stop) {
			clear_wait(self, THREAD_AWAKENED, FALSE);
			break;
		}
		doassign_parked = 1;
		thread_block((void (*)(void)) 0);
	}
	doassign_gone = 1;
	thread_terminate_self();
}

static void
doassign_driver_fn(void)
{
	doassign_assign_kr = thread_assign(doassign_victim->top_act,
					   doassign_set);
	doassign_driver_done = 1;
	thread_terminate_self();
}

static boolean_t
doassign_wait(volatile int *v)
{
	natural_t	t0 = timeout_ticks;

	while (*v == 0) {
		if (timeout_ticks - t0 >= DOASSIGN_WAIT)
			return FALSE;
		mutex_pause();
	}
	return TRUE;
}

void
doassign_test(void)
{
	processor_set_t	name;
	thread_t	v, driver;
	kern_return_t	kr;

	doassign_armed = 0;
	doassign_at_window = 0;
	doassign_release = 0;
	doassign_driver_done = 0;
	doassign_parked = 0;
	doassign_stop = 0;
	doassign_gone = 0;
	doassign_victim = THREAD_NULL;

	kr = processor_set_create(&realhost, &doassign_set, &name);
	if (kr != KERN_SUCCESS) {
		printf("doassign_test: NOT ASKED — processor_set_create "
		       "returned 0x%x (#787)\n", kr);
		return;
	}
	pset_deallocate(name);		/* the test keeps the set reference */

	v = kernel_thread(kernel_task, doassign_victim_fn, (char *) 0);
	if (v == THREAD_NULL || !doassign_wait(&doassign_parked)) {
		printf("doassign_test: NOT ASKED — the victim did not park "
		       "(#787)\n");
		doassign_stop = 1;
		thread_wakeup((event_t) &doassign_event);
		processor_set_destroy(doassign_set);
		pset_deallocate(doassign_set);
		return;
	}

	printf("doassign_test: starting: a default-set thread is assigned to a "
	       "set destroyed in thread_doassign()'s window, and the "
	       "assignment must come back (#787)\n");

	doassign_armed = 1;
	driver = kernel_thread(kernel_task, doassign_driver_fn, (char *) 0);
	if (driver == THREAD_NULL) {
		printf("doassign_test: NOT ASKED — no driver (#787)\n");
		goto cleanup;
	}

	if (!doassign_wait(&doassign_at_window)) {
		/* The probe never fired: a build without the widen. */
		printf("doassign_test: NOT ASKED — thread_doassign()'s window "
		       "was never reached: a build without the widen that adds "
		       "the probe (#787)\n");
		(void) doassign_wait(&doassign_driver_done);
		(void) processor_set_destroy(doassign_set);	/* V moved in */
		goto cleanup;
	}

	/*
	 * In the window now, no set locked: destroy the set so the lock loop
	 * finds it inactive and retries with the default set -- the set the
	 * victim is already in.  The test's reference keeps the set's memory
	 * alive, inactive, for thread_doassign() to lock and reject.
	 */
	(void) processor_set_destroy(doassign_set);
	doassign_release = 1;

	if (!doassign_wait(&doassign_driver_done))
		printf("doassign_test: WRONG — thread_doassign() did not come "
		       "back in %u ticks: the retry locked the default set's "
		       "mutex a second time and slept holding it (#787)\n",
		       DOASSIGN_WAIT);
	else
		printf("doassign_test: PASS — the assignment came back (0x%x), "
		       "the retry found the thread already in the default set "
		       "and moved it no further (#787)\n", doassign_assign_kr);

cleanup:
	doassign_stop = 1;
	thread_wakeup((event_t) &doassign_event);
	(void) doassign_wait(&doassign_gone);
	pset_deallocate(doassign_set);	/* the last reference: frees it */
}
