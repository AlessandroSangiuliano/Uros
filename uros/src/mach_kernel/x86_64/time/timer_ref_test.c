/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -o: a thread whose wakeup meets its timeout is destroyed (#787).
 *
 * softclock() makes a thread's fired timer TELT_PENDING and lets go of
 * timer_lock before it calls thread_timeout().  A wakeup in between --
 * thread_go(), for a receiver whose timeout has just run out -- cancels the
 * timer through reset_timeout(), which finds it PENDING, makes it UNSET and
 * has no reference to give back: the reference set_timeout() took is the
 * coming thread_timeout()'s to release, whatever that call finds.
 *
 * The window is a few instructions wide, and a wakeup that watches for it
 * from outside does not meet it: thread_timeout() takes the thread's lock
 * first (#787: 1 of 200 rounds saw PENDING under TCG, and lost).  So here the
 * test takes that lock first, as a thread_go() that won the race has: a
 * sleeper parks with a two-tick timeout, as an IPC receiver does, and this
 * thread, on a processor that is not the master -- the master counts the
 * ticks, and this one waits at splsched -- holds the sleeper's lock until
 * softclock() has fired the timer, then does what thread_go() does under it:
 * reset_timeout_check(), and the wakeup.  thread_timeout(), held on the lock
 * meanwhile, then finds the timer UNSET.  Every sleeper takes a reference for
 * the test before it parks, and once it has ended and the reaper is done
 * with it, that reference must be the only one left.  One more is the fired
 * timer's, never released, and the thread would never have been destroyed.
 * The rounds whose timer had fired and been handled before this thread had
 * the lock are the control: no window there, in either kernel.
 */

#include <kern/cpu_number.h>
#include <kern/ipc_sched.h>		/* thread_will_wait_with_timeout */
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
#define	TIMER_REF_TEST_MS	20	/* two ticks: the lock is had before the first */
#define	TIMER_REF_TEST_WAIT	200	/* ticks: two seconds at 100 Hz */

extern struct zone	*thread_shuttle_zone;

static volatile int	timer_ref_test_armed;	/* about to block, this round */
static volatile int	timer_ref_test_woke;	/* back from its block */
static volatile int	timer_ref_test_round;
static volatile int	timer_ref_test_cpu;	/* where this thread waits */
static volatile int	timer_ref_test_bound;	/* it is there */
static thread_t		timer_ref_test_threads[TIMER_REF_TEST_ROUNDS];
static int		timer_ref_test_results[TIMER_REF_TEST_ROUNDS];

static void
timer_ref_test_sleeper(void)
{
	thread_t	self = current_thread();
	int		i = timer_ref_test_round;

	thread_reference(self);			/* the test's */
	timer_ref_test_threads[i] = self;

	thread_will_wait_with_timeout(self, TIMER_REF_TEST_MS);
	timer_ref_test_armed = 1;
	thread_block((void (*)(void)) 0);

	reset_timeout_check(&self->timer);	/* as a receiver does, back */
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

/*
 * The same, without blocking: mutex_pause() sleeps a tick, and a sleeper's
 * timer has two -- the wait for it to park has to leave this thread the
 * whole first tick to take the lock in (#787: with the sleeping wait, 0 of
 * 200 rounds under KVM had the lock before the timer fired).
 */
static boolean_t
timer_ref_test_spin(volatile int *v)
{
	natural_t	t0 = timeout_ticks;

	while (*v == 0) {
		if (timeout_ticks - t0 >= TIMER_REF_TEST_WAIT)
			return FALSE;
		cpu_pause();
	}
	return TRUE;
}

/*
 * One round's wakeup, under the sleeper's lock: TRUE when the timer was
 * fired and not yet handled, which is the window.
 */
static boolean_t
timer_ref_test_wake(thread_t sleeper, const char **stopped)
{
	natural_t	t0;
	boolean_t	met = FALSE;
	int		set;
	spl_t		s;

	/* Blocked: waiting, and off its processor. */
	t0 = timeout_ticks;
	while ((sleeper->state & (TH_WAIT | TH_RUN)) != TH_WAIT &&
	       (sleeper->state & TH_WAIT) != 0 &&
	       timeout_ticks - t0 < TIMER_REF_TEST_WAIT)
		cpu_pause();

	s = splsched();
	thread_lock(sleeper);
	t0 = timeout_ticks;
	while ((set = sleeper->timer.set) == TELT_SET &&
	       timeout_ticks - t0 < TIMER_REF_TEST_WAIT)
		cpu_pause();
	if (set == TELT_PENDING) {
		reset_timeout_check(&sleeper->timer);
		clear_wait_locked(sleeper, THREAD_AWAKENED, FALSE);
		met = TRUE;
	} else if (set == TELT_SET)
		*stopped = "a two-tick timer did not fire";
	thread_unlock(sleeper);
	splx(s);
	return met;
}

void
timer_ref_test(void)
{
	thread_t	self = current_thread();
	int		i, n, r = -1, ncpu = 0;
	unsigned int	met = 0, before = 0, other = 0;
	unsigned int	kept = 0, control_off = 0, destroyed = 0;
	long		kept_count = 0, control_count = 0;
	int		zone_before, zone_after;
	const char	*stopped = NULL;
	natural_t	t0;

	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p == PROCESSOR_NULL || p->state == PROCESSOR_OFF_LINE)
			continue;
		ncpu++;
		if (i != master_cpu)
			r = i;
	}
	if (ncpu < 2 || r < 0) {
		printf("timer_ref_test: NOT ASKED — %d processor: the wakeup "
		       "has to hold the thread on a processor that is not the "
		       "master while the master's tick fires its timer (#787)\n",
		       ncpu);
		return;
	}

	thread_bind(self, cpu_to_processor(r));
	thread_block((void (*)(void)) 0);	/* runs next where it is bound */
	if (cpu_number() != r) {
		printf("timer_ref_test: NOT ASKED — bound to processor %d, it "
		       "runs on %d (#787)\n", r, cpu_number());
		thread_bind(self, PROCESSOR_NULL);
		return;
	}

	zone_before = thread_shuttle_zone->count;
	printf("timer_ref_test: starting: %u sleepers park with a two-tick "
	       "timeout, and processor %d holds each one's lock until the "
	       "master's tick has fired the timer, then cancels it and wakes "
	       "the sleeper as thread_go() does (#787)\n",
	       TIMER_REF_TEST_ROUNDS, r);

	for (n = 0; n < TIMER_REF_TEST_ROUNDS; n++) {
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
		if (!timer_ref_test_spin(&timer_ref_test_armed)) {
			stopped = "a sleeper did not park";
			break;
		}
		if (timer_ref_test_wake(timer_ref_test_threads[n], &stopped))
			met++;
		else
			before++;	/* handled before the lock was had */
		if (stopped != NULL)
			break;
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
			if (left != 1) {
				kept++;
				kept_count = left;
			}
		} else if (timer_ref_test_results[i] == THREAD_TIMED_OUT) {
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
		printf("timer_ref_test: WRONG — %u of the %u sleepers whose "
		       "timer was handled first ended with a count of %ld, not "
		       "the test's 1, and %u came back with neither result: "
		       "the control does not hold (#787)\n", control_off,
		       before, control_count, other);
	else if (met == 0)
		printf("timer_ref_test: NOT ASKED — in %u rounds the timer was "
		       "always handled before this thread had the lock (#787)\n",
		       n);
	else
		printf("timer_ref_test: PASS — %u of %u sleepers were woken "
		       "while their fired timer was pending, %u had theirs "
		       "handled first; every count came back to the test's 1 "
		       "reference, and %u of %u threads were destroyed, the "
		       "shuttle zone %d from where it began (#787)\n", met, n,
		       before, destroyed, n, zone_after - zone_before);
}
