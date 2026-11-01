/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -y: the priority-depression timer holds a reference on its thread (#787,
 * section 11).
 *
 * set_timeout() took a reference on the thread only for the wait timer, so
 * thread_depress_timeout() could run against a thread a termination had freed
 * as its depression ran out.  The fix makes the depression timer take one too,
 * released on a cancel and when it fires.
 *
 * A worker parks on an event of the test's.  This thread, from outside, arms
 * the worker's depression with a long timeout and reads its reference count:
 * one more than before arming.  It aborts the depression: back to what it was.
 * Then it arms a short depression and lets it fire: back again, once
 * thread_depress_timeout() has run.  A count that does not rise on arming is
 * the defect -- the timer held no reference, and the thread was the
 * termination's to free.  One processor is enough: the worker is parked and
 * only this thread touches it.
 */

#include <kern/ipc_sched.h>		/* thread_go */
#include <kern/lock.h>			/* mutex_pause */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched.h>			/* DEPRESSPRI */
#include <kern/sched_prim.h>
#include <kern/spl.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_act.h>
#include <kern/time_out.h>		/* timeout_ticks */

#include <cpu/regs.h>			/* cpu_pause */
#include <time/depress_ref_test.h>

/* Not in a header: the depression arming routine (#787). */
extern void		thread_depress_priority(thread_t, mach_msg_timeout_t);
extern kern_return_t	thread_depress_abort(thread_act_t);

#define	DEPRESS_REF_WAIT	200	/* ticks: two seconds at 100 Hz */
#define	DEPRESS_REF_LONG	100000	/* ms: longer than the test runs */
#define	DEPRESS_REF_SHORT	20	/* ms: two ticks, so it fires */

static thread_t			depress_ref_worker;
static int			depress_ref_event;
static volatile int		depress_ref_parked;
static volatile int		depress_ref_stop;
static volatile int		depress_ref_gone;

static void
depress_ref_worker_fn(void)
{
	thread_t	self = current_thread();

	depress_ref_worker = self;
	while (!depress_ref_stop) {
		assert_wait((event_t) &depress_ref_event, FALSE);
		if (depress_ref_stop) {
			clear_wait(self, THREAD_AWAKENED, FALSE);
			break;
		}
		depress_ref_parked = 1;
		thread_block((void (*)(void)) 0);
	}
	depress_ref_gone = 1;
	thread_terminate_self();
}

/* The worker blocked: waiting, and off its processor. */
static boolean_t
depress_ref_blocked(thread_t w)
{
	natural_t	t0 = timeout_ticks;

	while ((w->state & (TH_WAIT | TH_RUN)) != TH_WAIT) {
		if (timeout_ticks - t0 >= DEPRESS_REF_WAIT)
			return FALSE;
		cpu_pause();
	}
	return TRUE;
}

static long
depress_ref_count(thread_t w)
{
	long	n;
	spl_t	s;

	s = splsched();
	thread_lock(w);
	n = w->ref_count;
	thread_unlock(w);
	splx(s);
	return n;
}

void
depress_ref_test(void)
{
	thread_t	w;
	long		c0, c1, c2, c3;
	natural_t	t0;
	const char	*wrong = NULL;

	depress_ref_parked = 0;
	depress_ref_stop = 0;
	depress_ref_gone = 0;
	depress_ref_worker = THREAD_NULL;

	if (kernel_thread(kernel_task, depress_ref_worker_fn, (char *) 0)
							== THREAD_NULL) {
		printf("depress_ref_test: NOT ASKED — no worker (#787)\n");
		return;
	}
	t0 = timeout_ticks;
	while (depress_ref_worker == THREAD_NULL &&
	       timeout_ticks - t0 < DEPRESS_REF_WAIT)
		mutex_pause();
	w = depress_ref_worker;
	if (w == THREAD_NULL || !depress_ref_blocked(w)) {
		depress_ref_stop = 1;
		thread_wakeup((event_t) &depress_ref_event);
		printf("depress_ref_test: NOT ASKED — the worker did not park "
		       "(#787)\n");
		return;
	}
	thread_reference(w);		/* the test's own, dropped at the end */

	printf("depress_ref_test: starting: a parked worker's depression is "
	       "armed, aborted, and armed again to fire, and its reference "
	       "count is read at each step (#787)\n");

	c0 = depress_ref_count(w);

	/* Arm: a reference is taken while the timer is set. */
	thread_depress_priority(w, DEPRESS_REF_LONG);
	c1 = depress_ref_count(w);

	/* Abort: the reference is given back. */
	(void) thread_depress_abort(w->top_act);
	c2 = depress_ref_count(w);

	/* Arm short and let it fire: thread_depress_timeout() releases it. */
	(void) depress_ref_blocked(w);
	thread_depress_priority(w, DEPRESS_REF_SHORT);
	t0 = timeout_ticks;
	while (depress_ref_count(w) != c0 &&
	       timeout_ticks - t0 < DEPRESS_REF_WAIT)
		mutex_pause();
	c3 = depress_ref_count(w);

	if (c1 != c0 + 1)
		wrong = "arming the depression took no reference";
	else if (c2 != c0)
		wrong = "aborting the depression did not give the reference back";
	else if (c3 != c0)
		wrong = "the depression's firing did not give the reference back";

	/* Let the worker terminate, then drop the test's reference. */
	depress_ref_stop = 1;
	thread_wakeup((event_t) &depress_ref_event);
	t0 = timeout_ticks;
	while (!depress_ref_gone && timeout_ticks - t0 < DEPRESS_REF_WAIT)
		mutex_pause();
	thread_deallocate(w);

	if (wrong != NULL)
		printf("depress_ref_test: WRONG — %s: the count was %ld before, "
		       "%ld armed, %ld aborted, %ld after firing, where the "
		       "test holds one (#787)\n", wrong, c0, c1, c2, c3);
	else
		printf("depress_ref_test: PASS — the depression timer took a "
		       "reference on arming (%ld to %ld) and gave it back on "
		       "abort and on firing (back to %ld), the test's one "
		       "reference throughout (#787)\n", c0, c1, c0);
}
