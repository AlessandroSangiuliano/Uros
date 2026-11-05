/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -w: the hand-off on block reaches the thread it names, and that thread does
 * not wait behind another one when its waker leaves (#777).
 *
 * A send names the thread it wakes, and thread_setrun() hands off to that
 * thread alone: any other wakeup on the way goes the ordinary way.  And the
 * waker's leaving settles the bet: if the thread it queued is not the one that
 * gets the processor, it goes back the ordinary way, because no other processor
 * reads this queue.
 *
 * Both are asked here because a thread can be bound here, which a task cannot
 * do.  This thread, bound where it is, stands for the sender, and names the
 * wakee as ipc_mqueue_deliver() does.  A thread bound to the same processor at
 * a higher priority is what thread_select() takes instead of the wakee; it
 * runs until the wakee has run or two seconds are up, so a good kernel spends
 * microseconds in it and a bad one comes out saying so.  The wakee has to run
 * somewhere else meanwhile, which takes a second processor.
 */

#include <stdint.h>

#include <kern/cpu_number.h>
#include <kern/machine.h>		/* machine_info */
#include <kern/lock.h>			/* mutex_pause */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched.h>
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>

#include <cpu/wakee_test.h>
#include <cpu/regs.h>			/* cpu_pause */
#include <cpu/smp.h>			/* real_ncpus */
#include <time/tsc.h>

#define	WAKEE_TEST_HELD_S	2	/* how long the wakee may be held */
#define	WAKEE_TEST_WAIT_S	10	/* a thread's start, or its end */
#define	WAKEE_TEST_HIGH	(BASEPRI_KERNEL + 1)
#define	WAKEE_TEST_LOW	(BASEPRI_SYSTEM + 2)

/* The events: one thread waits on each, and each is woken once. */
static int		wakee_test_wakee_event;
static int		wakee_test_other_event;
static int		wakee_test_high_event;
static int		wakee_test_done_event;

static volatile int	wakee_test_cpu;	/* where this test runs */
static volatile int	wakee_test_wakee_on = -1; /* where the wakee ran */
static volatile int	wakee_test_wakee_ran;
static volatile int	wakee_test_other_ran;
static volatile int	wakee_test_high_done;
static volatile int	wakee_test_high_held;	/* the two seconds ran out */
static volatile uint64_t wakee_test_high_ticks;

static void
wakee_test_wakee(void)
{
	thread_set_own_priority(WAKEE_TEST_LOW);
	assert_wait((event_t) &wakee_test_wakee_event, FALSE);
	thread_block((void (*)(void)) 0);
	wakee_test_wakee_on = cpu_number();
	wakee_test_wakee_ran = 1;
	thread_terminate_self();
}

static void
wakee_test_other(void)
{
	assert_wait((event_t) &wakee_test_other_event, FALSE);
	thread_block((void (*)(void)) 0);
	wakee_test_other_ran = 1;
	thread_terminate_self();
}

static void
wakee_test_high(void)
{
	uint64_t	hz = tsc_hz(), t0;

	thread_bind(current_thread(), cpu_to_processor(wakee_test_cpu));
	thread_block((void (*)(void)) 0);	/* runs next where it is bound */
	thread_set_own_priority(WAKEE_TEST_HIGH);
	assert_wait((event_t) &wakee_test_high_event, FALSE);
	thread_block((void (*)(void)) 0);

	t0 = rdtsc();
	while (!wakee_test_wakee_ran &&
	       rdtsc() - t0 < hz * WAKEE_TEST_HELD_S)
		cpu_pause();
	wakee_test_high_ticks = rdtsc() - t0;
	wakee_test_high_held = !wakee_test_wakee_ran;
	wakee_test_high_done = 1;
	thread_wakeup((event_t) &wakee_test_done_event);
	thread_terminate_self();
}

/* Off every processor, and waiting: what a wakeup hands to thread_setrun(). */
static boolean_t
wakee_test_parked(thread_t th, uint64_t hz)
{
	uint64_t	t0 = rdtsc();

	while ((th->state & (TH_WAIT | TH_RUN)) != TH_WAIT) {
		if (rdtsc() - t0 >= hz * WAKEE_TEST_WAIT_S)
			return FALSE;
		mutex_pause();
	}
	return TRUE;
}

static boolean_t
wakee_test_wait(volatile int *v, uint64_t hz)
{
	uint64_t	t0 = rdtsc();

	while (*v == 0) {
		if (rdtsc() - t0 >= hz * WAKEE_TEST_WAIT_S)
			return FALSE;
		mutex_pause();
	}
	return TRUE;
}

void
wakee_test(void)
{
	uint64_t	hz = tsc_hz();
	thread_t	self = current_thread();
	thread_t	wakee, other, high, took_first, took_named;
	unsigned long	undone[SCHED_HANDOFF_UNDO_WHERE], left;
	int		me, i, ncpu = 0;

	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p != PROCESSOR_NULL && p->state != PROCESSOR_OFF_LINE)
			ncpu++;
	}
	if (ncpu < 2 || hz == 0 || sched_rpc_handoff == 0 ||
	    machine_info.avail_cpus < real_ncpus) {
		printf("wakee_test: NOT ASKED — %d processor(s), TSC at %llu "
		       "Hz, hand-off reach %d, %d of %d processors in: a wakee "
		       "needs a second one to go to, and the hand-off to be on "
		       "(#777)\n", ncpu, (unsigned long long) hz,
		       sched_rpc_handoff, machine_info.avail_cpus, real_ncpus);
		return;
	}

	me = thread_bind_here();
	wakee_test_cpu = me;
	wakee = kernel_thread(kernel_task, wakee_test_wakee, (char *) 0);
	other = kernel_thread(kernel_task, wakee_test_other, (char *) 0);
	high = kernel_thread(kernel_task, wakee_test_high, (char *) 0);
	if (wakee == THREAD_NULL || other == THREAD_NULL || high == THREAD_NULL ||
	    !wakee_test_parked(wakee, hz) ||
	    !wakee_test_parked(other, hz) ||
	    !wakee_test_parked(high, hz)) {
		printf("wakee_test: NOT ASKED — its threads did not all come "
		       "to wait in %u s (#777)\n", WAKEE_TEST_WAIT_S);
		thread_bind(self, PROCESSOR_NULL);
		return;
	}

	printf("wakee_test: starting: processor %d names a wakee, wakes another "
	       "thread, wakes the wakee, and leaves for a thread of higher "
	       "priority bound there (#777)\n", me);

	sched_rpc_handoff_undone(undone);
	left = undone[URMACH_HANDOFF_UNDO_LEFT];

	/* What ipc_mqueue_deliver() does, around another thread's wakeup. */
	self->handoff_hint = TRUE;
	self->handoff_target = wakee;
	thread_wakeup((event_t) &wakee_test_other_event);
	took_first = self->handoff_wakee;
	thread_wakeup((event_t) &wakee_test_wakee_event);
	took_named = self->handoff_wakee;
	self->handoff_target = THREAD_NULL;
	self->handoff_hint = FALSE;

	/* And this thread leaves, with something better than the wakee here. */
	thread_wakeup((event_t) &wakee_test_high_event);
	assert_wait((event_t) &wakee_test_done_event, FALSE);
	if (wakee_test_high_done)
		clear_wait(self, THREAD_AWAKENED, FALSE);
	thread_block((void (*)(void)) 0);

	(void) wakee_test_wait(&wakee_test_high_done, hz);
	(void) wakee_test_wait(&wakee_test_wakee_ran, hz);
	(void) wakee_test_wait(&wakee_test_other_ran, hz);
	thread_bind(self, PROCESSOR_NULL);

	sched_rpc_handoff_undone(undone);
	left = undone[URMACH_HANDOFF_UNDO_LEFT] - left;

	if (took_first != THREAD_NULL)
		printf("wakee_test: WRONG — a wakeup of another thread took "
		       "the hand-off meant for the wakee (#777)\n");
	else if (took_named != wakee)
		printf("wakee_test: WRONG — the thread named was not handed "
		       "off, queued thread %p (#777)\n", took_named);
	else if (!wakee_test_high_done || !wakee_test_wakee_ran ||
		 !wakee_test_other_ran)
		printf("wakee_test: WRONG — a thread never ran: the wakee %d, "
		       "the other %d, the high one %d (#777)\n",
		       wakee_test_wakee_ran, wakee_test_other_ran,
		       wakee_test_high_done);
	else if (wakee_test_high_held)
		printf("wakee_test: WRONG — the wakee waited behind a thread "
		       "of higher priority on processor %d for all of %u s, on a "
		       "queue no other processor reads (#777)\n", me,
		       WAKEE_TEST_HELD_S);
	else
		printf("wakee_test: PASS — the other wakeup went the ordinary "
		       "way, the wakee named was queued on processor %d, and when "
		       "another thread took that processor it ran on processor %d "
		       "after %llu TSC ticks; %lu undone as the waker left (#777)\n",
		       me, wakee_test_wakee_on,
		       (unsigned long long) wakee_test_high_ticks, left);
}
