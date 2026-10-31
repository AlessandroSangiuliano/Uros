/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -o: a thread whose wakeup meets its timeout is destroyed (#787).
 *
 * softclock() makes a thread's fired timer TELT_PENDING and lets go of
 * timer_lock before it calls thread_timeout().  A wakeup in between -- a
 * message for a receiver whose timeout has just run out -- cancels the timer
 * through reset_timeout(), which finds it PENDING, makes it UNSET and has no
 * reference to give back: the reference set_timeout() took is the coming
 * thread_timeout()'s to release, whatever that call finds.
 *
 * Here a sleeper parks with a one-tick timeout, as an IPC receiver does, on a
 * queue of the test's, and this thread, on another processor, watches the
 * sleeper's timer and the moment it reads PENDING takes the sleeper off the
 * queue under the queue's lock and wakes it with thread_go(), as a sender
 * does.  A sleeper that comes back with THREAD_AWAKENED was woken before
 * thread_timeout() ran: its round met the window.  Every sleeper takes a
 * reference for the test before it parks, and once it has ended and the
 * reaper is done with it, that reference must be the only one left.  One
 * more is the fired timer's, never released, and the thread would never
 * have been destroyed.  The rounds woken by their timeout are the control:
 * no window there, in either kernel.
 */

#include <kern/ipc_sched.h>		/* thread_go, thread_will_wait_with_timeout */
#include <kern/lock.h>
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/spl.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/time_out.h>
#include <kern/zalloc.h>

#include <cpu/regs.h>			/* cpu_pause */
#include <time/timer_ref_test.h>

#define	TIMER_REF_TEST_ROUNDS	200
#define	TIMER_REF_TEST_WAIT	200	/* ticks: two seconds at 100 Hz */

extern struct zone	*thread_shuttle_zone;

decl_simple_lock_data(static, timer_ref_test_lock)	/* the queue's */
static volatile int	timer_ref_test_parked;	/* on the queue, under it */
static volatile int	timer_ref_test_armed;	/* parked, this round */
static volatile int	timer_ref_test_woke;	/* back from its block */
static volatile int	timer_ref_test_round;
static thread_t		timer_ref_test_threads[TIMER_REF_TEST_ROUNDS];
static int		timer_ref_test_results[TIMER_REF_TEST_ROUNDS];

static void
timer_ref_test_sleeper(void)
{
	thread_t	self = current_thread();
	int		i = timer_ref_test_round;
	spl_t		s;

	thread_reference(self);			/* the test's */
	timer_ref_test_threads[i] = self;

	thread_will_wait_with_timeout(self, 1);	/* a millisecond: one tick */
	s = splsched();
	simple_lock(&timer_ref_test_lock);
	timer_ref_test_parked = 1;
	simple_unlock(&timer_ref_test_lock);
	splx(s);
	timer_ref_test_armed = 1;
	thread_block((void (*)(void)) 0);

	/* What a receiver does when it comes back: off the queue, then its
	 * timer (ipc/ipc_mqueue.c). */
	s = splsched();
	simple_lock(&timer_ref_test_lock);
	timer_ref_test_parked = 0;
	simple_unlock(&timer_ref_test_lock);
	splx(s);
	reset_timeout_check(&self->timer);

	timer_ref_test_results[i] = self->wait_result;
	timer_ref_test_woke = 1;
	thread_terminate_self();
}

/* Wait for *v, for TIMER_REF_TEST_WAIT ticks at most. */
static boolean_t
timer_ref_test_wait(volatile int *v)
{
	natural_t	t0 = timeout_ticks;

	while (*v == 0) {
		if (timeout_ticks - t0 >= TIMER_REF_TEST_WAIT)
			return FALSE;
		mutex_pause();
	}
	return TRUE;
}

void
timer_ref_test(void)
{
	thread_t	self = current_thread();
	int		me, i, n, ncpu = 0;
	unsigned int	met = 0, timed_out = 0, missed = 0, other = 0;
	unsigned int	kept = 0, control_off = 0, destroyed = 0;
	long		kept_count = 0, control_count = 0;
	int		zone_before, zone_after, set;
	const char	*stopped = NULL;
	natural_t	t0;
	spl_t		s;

	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p != PROCESSOR_NULL && p->state != PROCESSOR_OFF_LINE)
			ncpu++;
	}
	if (ncpu < 2) {
		printf("timer_ref_test: NOT ASKED — %d processor: the wakeup "
		       "has to come from another processor while softclock() "
		       "is between the timer and thread_timeout() (#787)\n",
		       ncpu);
		return;
	}

	simple_lock_init(&timer_ref_test_lock, ETAP_MISC_TIMER);
	me = thread_bind_here();
	zone_before = thread_shuttle_zone->count;
	printf("timer_ref_test: starting: %u sleepers park with a one-tick "
	       "timeout, and processor %d wakes each with thread_go() as soon "
	       "as it reads its timer fired (#787)\n",
	       TIMER_REF_TEST_ROUNDS, me);

	for (n = 0; n < TIMER_REF_TEST_ROUNDS; n++) {
		thread_t	sleeper;

		timer_ref_test_round = n;
		timer_ref_test_armed = 0;
		timer_ref_test_woke = 0;
		timer_ref_test_threads[n] = THREAD_NULL;
		timer_ref_test_results[n] = -1;	/* THREAD_AWAKENED is 0 */
		if (kernel_thread(kernel_task, timer_ref_test_sleeper,
				  (char *) 0) == THREAD_NULL) {
			stopped = "a sleeper could not be made";
			break;
		}
		if (!timer_ref_test_wait(&timer_ref_test_armed)) {
			stopped = "a sleeper did not park";
			break;
		}
		sleeper = timer_ref_test_threads[n];

		t0 = timeout_ticks;
		while ((set = sleeper->timer.set) == TELT_SET &&
		       timeout_ticks - t0 < TIMER_REF_TEST_WAIT)
			cpu_pause();
		if (set == TELT_SET) {
			stopped = "a one-tick timer did not fire";
			break;
		}
		if (set == TELT_PENDING) {
			s = splsched();
			simple_lock(&timer_ref_test_lock);
			if (timer_ref_test_parked) {
				timer_ref_test_parked = 0;
				thread_go(sleeper);
			}
			simple_unlock(&timer_ref_test_lock);
			splx(s);
		} else
			missed++;	/* thread_timeout() was done already */

		if (!timer_ref_test_wait(&timer_ref_test_woke)) {
			stopped = "a sleeper did not come back";
			n++;		/* counted below, with the rest */
			break;
		}
	}

	/*
	 * The reaper's work: each sleeper's count comes down to the test's
	 * one reference, or stops one above it.
	 */
	t0 = timeout_ticks;
	for (i = 0; i < n; i++) {
		thread_t th = timer_ref_test_threads[i];

		if (th == THREAD_NULL)
			continue;
		while (th->ref_count > 1 &&
		       timeout_ticks - t0 < TIMER_REF_TEST_WAIT)
			mutex_pause();
	}
	for (i = 0; i < n; i++) {
		thread_t	th = timer_ref_test_threads[i];
		long		left;

		if (th == THREAD_NULL)
			continue;
		left = th->ref_count;
		if (timer_ref_test_results[i] == THREAD_AWAKENED) {
			met++;
			if (left != 1) {
				kept++;
				kept_count = left;
			}
		} else if (timer_ref_test_results[i] == THREAD_TIMED_OUT) {
			timed_out++;
			if (left != 1) {
				control_off++;
				control_count = left;
			}
		} else
			other++;
		if (left == 1)
			destroyed++;	/* the deallocation below is its last */
		thread_deallocate(th);
	}
	zone_after = thread_shuttle_zone->count;
	thread_bind(self, PROCESSOR_NULL);

	if (stopped != NULL)
		printf("timer_ref_test: WRONG — %s within %u ticks, at round "
		       "%d of %u (#787)\n", stopped, TIMER_REF_TEST_WAIT, n,
		       TIMER_REF_TEST_ROUNDS);
	else if (kept != 0)
		printf("timer_ref_test: WRONG — %u of the %u sleepers woken "
		       "while their fired timer was pending kept a reference "
		       "after the reaper, the count %ld where the test's own is "
		       "1: the timer's, never released, and the thread never "
		       "destroyed; %u of %u threads destroyed, the shuttle zone "
		       "%d above where it began (#787)\n", kept, met,
		       kept_count, destroyed, n, zone_after - zone_before);
	else if (control_off != 0 || other != 0)
		printf("timer_ref_test: WRONG — %u of the %u sleepers woken by "
		       "their timeout ended with a count of %ld, not the test's "
		       "1, and %u came back with another result: the control "
		       "does not hold (#787)\n", control_off, timed_out,
		       control_count, other);
	else if (met == 0)
		printf("timer_ref_test: NOT ASKED — in %u rounds the wakeup "
		       "never came before thread_timeout() (%u woken by their "
		       "timeout, %u already done when read) (#787)\n", n,
		       timed_out, missed);
	else
		printf("timer_ref_test: PASS — %u of %u sleepers were woken by "
		       "thread_go() while their fired timer was pending, and "
		       "%u by their timeout; every count came back to the "
		       "test's 1 reference, and %u of %u threads were destroyed, "
		       "the shuttle zone %d from where it began (#787)\n", met,
		       n, timed_out, destroyed, n, zone_after - zone_before);
}
