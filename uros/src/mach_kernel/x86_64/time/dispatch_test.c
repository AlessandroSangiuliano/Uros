/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -d: a second wakeup on a dispatching processor does not take the place of
 * a better thread (#787, section 7).
 *
 * An idle processor that a wakeup has given a thread is DISPATCHING until it
 * switches to it, and a wakeup made on it meanwhile -- from the interrupt that
 * ended its halt -- reached thread_setrun()'s preemption check, which swapped
 * next_thread for the new thread whatever the two priorities.  The better
 * thread then waited behind the worse one, and on one processor a wakeup of
 * two sleepers, the better first, inverted them every time.
 *
 * H, then L below it, both bound to the master, wait on one event.  The
 * master's tick wakes it, after timeout_tick(), through
 * dispatch_test_tick_probe(), whose call site a widen adds; a build without
 * the widen never calls it, and the test says NOT ASKED.  The probe also
 * notes whether the master was idle when it ran: only then does the first
 * wakeup make it DISPATCHING, and the question is asked.  H must run first.
 */

#include <kern/lock.h>			/* mutex_pause */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched.h>			/* BASEPRI_SYSTEM */
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/time_out.h>		/* timeout_ticks */

#include <time/dispatch_test.h>

#define	DISPATCH_TEST_WAIT	200	/* ticks: two seconds at 100 Hz */
#define	DISPATCH_TEST_L_PRI	(BASEPRI_SYSTEM + 8)

static int		dispatch_test_event;	/* H and L wait on it */
static volatile int	dispatch_test_armed;	/* the probe wakes it once */
static volatile int	dispatch_test_fired;
static volatile int	dispatch_test_was_idle;	/* the master, when it did */
static volatile int	dispatch_test_parked[2];	/* H, L */
static volatile int	dispatch_test_seq;
static volatile int	dispatch_test_ran[2];	/* their place in the order */

#define	DISPATCH_H	0
#define	DISPATCH_L	1

/*
 * Called from the master's tick, after timeout_tick(), by the widen's call
 * site; never otherwise.  Interrupt context: wake the event once.
 */
void
dispatch_test_tick_probe(void)
{
	if (!dispatch_test_armed)
		return;
	dispatch_test_armed = 0;
	dispatch_test_was_idle =
	    (current_thread() == current_processor()->idle_thread);
	dispatch_test_fired = 1;
	thread_wakeup((event_t) &dispatch_test_event);
}

static void
dispatch_test_sleeper(int me)
{
	thread_bind(current_thread(), master_processor);
	thread_block((void (*)(void)) 0);	/* runs next on the master */
	if (me == DISPATCH_L)
		thread_set_own_priority(DISPATCH_TEST_L_PRI);

	assert_wait((event_t) &dispatch_test_event, FALSE);
	dispatch_test_parked[me] = 1;
	thread_block((void (*)(void)) 0);

	dispatch_test_ran[me] = ++dispatch_test_seq;
	thread_terminate_self();
}

static void
dispatch_test_h(void)
{
	dispatch_test_sleeper(DISPATCH_H);
}

static void
dispatch_test_l(void)
{
	dispatch_test_sleeper(DISPATCH_L);
}

static boolean_t
dispatch_test_wait(volatile int *v)
{
	natural_t	t0 = timeout_ticks;

	while (*v == 0) {
		if (timeout_ticks - t0 >= DISPATCH_TEST_WAIT)
			return FALSE;
		mutex_pause();
	}
	return TRUE;
}

void
dispatch_test(void)
{
	thread_t	self = current_thread();
	int		i, ncpu = 0, other = -1;
	natural_t	t0;

	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p == PROCESSOR_NULL || p->state == PROCESSOR_OFF_LINE)
			continue;
		ncpu++;
		if (i != master_cpu)
			other = i;
	}
	/* Off the master, so that it idles while H and L wait. */
	if (other >= 0) {
		thread_bind(self, cpu_to_processor(other));
		thread_block((void (*)(void)) 0);
	}

	dispatch_test_armed = 0;
	dispatch_test_fired = 0;
	dispatch_test_was_idle = 0;
	dispatch_test_seq = 0;
	for (i = 0; i < 2; i++)
		dispatch_test_parked[i] = dispatch_test_ran[i] = 0;

	if (kernel_thread(kernel_task, dispatch_test_h, (char *) 0)
							== THREAD_NULL ||
	    !dispatch_test_wait(&dispatch_test_parked[DISPATCH_H]) ||
	    kernel_thread(kernel_task, dispatch_test_l, (char *) 0)
							== THREAD_NULL ||
	    !dispatch_test_wait(&dispatch_test_parked[DISPATCH_L])) {
		printf("dispatch_test: NOT ASKED — H and L did not both park "
		       "in %u ticks (#787)\n", DISPATCH_TEST_WAIT);
		if (other >= 0)
			thread_bind(self, PROCESSOR_NULL);
		return;
	}

	printf("dispatch_test: starting: H at %d, then L at %d, bound to the "
	       "master and waiting on one event, woken by the master's tick "
	       "(#787)\n", BASEPRI_SYSTEM, DISPATCH_TEST_L_PRI);
	dispatch_test_armed = 1;

	t0 = timeout_ticks;
	while ((!dispatch_test_ran[DISPATCH_H] ||
		!dispatch_test_ran[DISPATCH_L]) &&
	       timeout_ticks - t0 < DISPATCH_TEST_WAIT)
		mutex_pause();
	if (other >= 0)
		thread_bind(self, PROCESSOR_NULL);

	if (!dispatch_test_fired) {
		dispatch_test_armed = 0;
		printf("dispatch_test: NOT ASKED — the tick probe was never "
		       "called: a build without the widen that adds its call "
		       "site (#787)\n");
		/* Let H and L go, so they do not wait for good. */
		thread_wakeup((event_t) &dispatch_test_event);
	} else if (!dispatch_test_ran[DISPATCH_H] ||
		   !dispatch_test_ran[DISPATCH_L])
		printf("dispatch_test: WRONG — woken by the tick, H %s and L "
		       "%s in %u ticks (#787)\n",
		       dispatch_test_ran[DISPATCH_H] ? "ran" : "did not run",
		       dispatch_test_ran[DISPATCH_L] ? "ran" : "did not run",
		       DISPATCH_TEST_WAIT);
	else if (!dispatch_test_was_idle)
		printf("dispatch_test: NOT ASKED — the master was not idle "
		       "when its tick woke H and L, so no dispatch was in "
		       "progress for the second wakeup to meet; H ran %s "
		       "(#787)\n", dispatch_test_ran[DISPATCH_H] <
		       dispatch_test_ran[DISPATCH_L] ? "first" : "second");
	else if (dispatch_test_ran[DISPATCH_L] < dispatch_test_ran[DISPATCH_H])
		printf("dispatch_test: WRONG — the master was idle, its tick "
		       "woke H and then L, and L ran first: the second wakeup "
		       "took the place of the better thread the dispatch had "
		       "been given (#787)\n");
	else
		printf("dispatch_test: PASS — the master was idle, its tick "
		       "woke H and then L, and H ran first (#787)\n");
}
