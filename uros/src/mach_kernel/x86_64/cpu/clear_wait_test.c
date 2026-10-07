/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -V: a thread clears its own wait while another processor wakes it (#599).
 *
 * clear_wait_locked() lets go of the thread's lock to take the hash bucket's
 * first.  A waker that takes the thread off the hash in that gap holds its
 * wait at WAKING_EVENT, TH_WAIT still set, until it gets the thread's lock,
 * and a thread clearing its own wait must not carry on before then: the RCU
 * drain thread asserted its next wait in that state, and assert_wait()
 * panicked.  Here this thread, bound where it is, asserts a wait and clears it
 * CLEAR_WAIT_TEST_ROUNDS times while a thread bound to another processor wakes
 * the same event, and after every clear it looks, under its own lock, for a
 * wait left behind.  A round that finds one lets the waker finish before the
 * next, so that the test counts what assert_wait() would have panicked on.
 */

#include <stdint.h>

#include <kern/cpu_number.h>
#include <kern/lock.h>			/* mutex_pause */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/spl.h>
#include <kern/task.h>
#include <kern/thread.h>

#include <cpu/clear_wait_test.h>
#include <cpu/regs.h>			/* cpu_pause */
#include <time/tsc.h>

#define	CLEAR_WAIT_TEST_ROUNDS	20000
#define	CLEAR_WAIT_TEST_WAIT_S	10

static int		clear_wait_test_event;	/* the event both sides use */
static volatile int	clear_wait_test_cpu;	/* where the waker goes */
static volatile int	clear_wait_test_bound;	/* it is there */
static volatile int	clear_wait_test_go;	/* both start */
static volatile int	clear_wait_test_stop;	/* the rounds are over */
static volatile int	clear_wait_test_done;	/* the waker has stopped */
static volatile unsigned int clear_wait_test_wakeups;

static void
clear_wait_test_waker(void)
{
	thread_bind(current_thread(), cpu_to_processor(clear_wait_test_cpu));
	thread_block((void (*)(void)) 0);	/* runs next where it is bound */
	clear_wait_test_bound = 1;
	while (!clear_wait_test_go)
		cpu_pause();
	while (!clear_wait_test_stop) {
		thread_wakeup((event_t) &clear_wait_test_event);
		clear_wait_test_wakeups++;
		cpu_pause();
	}
	clear_wait_test_done = 1;
	thread_terminate_self();
}

static boolean_t
clear_wait_test_wait(volatile int *v, uint64_t hz)
{
	uint64_t	t0 = rdtsc();

	while (*v == 0) {
		if (hz != 0 && rdtsc() - t0 >= hz * CLEAR_WAIT_TEST_WAIT_S)
			return FALSE;
		mutex_pause();
	}
	return TRUE;
}

void
clear_wait_test(void)
{
	uint64_t	hz = tsc_hz();
	thread_t	self = current_thread();
	int		me, i, r = -1, ncpu = 0, state = 0;
	unsigned int	left = 0, met;
	event_t		event = NO_EVENT;
	spl_t		s;

	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p == PROCESSOR_NULL || p->state == PROCESSOR_OFF_LINE)
			continue;
		ncpu++;
	}
	if (ncpu < 2) {
		printf("clear_wait_test: NOT ASKED — %d processor: a waker there "
		       "runs both its halves at splsched, so a thread never sees "
		       "its own wait in between (#599)\n", ncpu);
		return;
	}

	me = thread_bind_here();
	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p != PROCESSOR_NULL && p->state != PROCESSOR_OFF_LINE &&
		    i != me)
			r = i;
	}
	clear_wait_test_cpu = r;
	if (r < 0 || kernel_thread(kernel_task, clear_wait_test_waker,
				   (char *) 0) == THREAD_NULL) {
		printf("clear_wait_test: NOT ASKED — no waker on another "
		       "processor (#599)\n");
		thread_bind(self, PROCESSOR_NULL);
		return;
	}
	if (!clear_wait_test_wait(&clear_wait_test_bound, hz)) {
		printf("clear_wait_test: NOT ASKED — the waker did not reach "
		       "processor %d in %u s (#599)\n", r,
		       CLEAR_WAIT_TEST_WAIT_S);
		clear_wait_test_stop = 1;
		clear_wait_test_go = 1;
		thread_bind(self, PROCESSOR_NULL);
		return;
	}

	printf("clear_wait_test: starting: processor %d asserts and clears its "
	       "own wait %u times while processor %d wakes the same event "
	       "(#599)\n", me, CLEAR_WAIT_TEST_ROUNDS, r);
	met = clear_wait_relock_waits;
	clear_wait_test_go = 1;
	for (i = 0; i < CLEAR_WAIT_TEST_ROUNDS; i++) {
		assert_wait((event_t) &clear_wait_test_event, FALSE);
		clear_wait(self, THREAD_AWAKENED, FALSE);
		s = splsched();
		thread_lock(self);
		if (self->wait_event != NO_EVENT || (self->state & TH_WAIT)) {
			if (left++ == 0) {
				event = self->wait_event;
				state = self->state;
			}
			while (self->wait_event == (event_t) WAKING_EVENT) {
				thread_unlock(self);
				cpu_pause();
				thread_lock(self);
			}
		}
		thread_unlock(self);
		splx(s);
	}
	met = clear_wait_relock_waits - met;
	clear_wait_test_stop = 1;
	(void) clear_wait_test_wait(&clear_wait_test_done, hz);
	thread_bind(self, PROCESSOR_NULL);

	if (!clear_wait_test_done)
		printf("clear_wait_test: WRONG — the waker did not stop in %u s "
		       "(#599)\n", CLEAR_WAIT_TEST_WAIT_S);
	else if (left != 0)
		printf("clear_wait_test: WRONG — %u of %u rounds came back from "
		       "clearing this thread's own wait with a wait left behind, "
		       "the first with wait_event %p and state 0x%x: a wakeup "
		       "from processor %d was still in flight, and the next "
		       "assert_wait() would have panicked (#599)\n", left,
		       CLEAR_WAIT_TEST_ROUNDS, event, state, r);
	else
		printf("clear_wait_test: PASS — %u rounds against %u wakeups "
		       "from processor %d, and every clear came back with no "
		       "wait left; clear_wait_locked() found a wakeup in flight "
		       "after a relock and waited for it %u times (#599)\n",
		       CLEAR_WAIT_TEST_ROUNDS, clear_wait_test_wakeups, r, met);
}
