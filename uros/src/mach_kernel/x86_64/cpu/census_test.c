/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -c: the quiet census walks the threads while other processors end them (#657).
 *
 * The census walked default_pset.threads with no lock, from the idle loop,
 * and on the campaign it read a thread freed under it.  Here
 * CENSUS_TEST_VICTIMS kernel threads wait on an event; this thread wakes them
 * all and at once walks the list through quiet_census_walk(), which is the
 * census's own walk under the processor set's lock.  The victims run on the
 * other processors and end, and their threads are freed after the lock is
 * given back.  The walk must come back.  The test says how many victims had
 * already run on to their end by then: that is the evidence that the deaths
 * met the walk.
 */

#include <stdint.h>

#include <kern/cpu_number.h>
#include <kern/lock.h>			/* mutex_pause */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>

#include <cpu/census_test.h>
#include <cpu/quiet_census.h>
#include <sync/atomic.h>
#include <time/tsc.h>

#define	CENSUS_TEST_VICTIMS	24
#define	CENSUS_TEST_WAIT_S	10

static int		census_test_event;	/* the victims sleep on it */
static volatile uint32_t census_test_waiting;	/* victims asleep, or about to be */
static volatile uint32_t census_test_ending;	/* victims woken and ending */

static void
census_test_victim(void)
{
	assert_wait((event_t) &census_test_event, FALSE);
	(void) atomic_add32(&census_test_waiting, 1);
	thread_block((void (*)(void)) 0);
	(void) atomic_add32(&census_test_ending, 1);
	thread_terminate_self();
}

void
census_test(void)
{
	uint64_t	hz = tsc_hz(), t0;
	int		i, me, ncpu = 0, n;
	unsigned int	ended, unreadable;

	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p == PROCESSOR_NULL || p->state == PROCESSOR_OFF_LINE)
			continue;
		ncpu++;
	}
	if (ncpu < 2) {
		printf("census_test: NOT ASKED — %d processor: no other "
		       "processor ends a thread while this one walks (#657)\n",
		       ncpu);
		return;
	}

	me = thread_bind_here();
	for (i = 0; i < CENSUS_TEST_VICTIMS; i++) {
		if (kernel_thread(kernel_task, census_test_victim,
				  (char *) 0) == THREAD_NULL) {
			printf("census_test: NOT ASKED — victim %d of %u could "
			       "not be made (#657)\n", i, CENSUS_TEST_VICTIMS);
			break;
		}
	}
	t0 = rdtsc();
	while (census_test_waiting < (uint32_t) i) {
		if (hz != 0 && rdtsc() - t0 >= hz * CENSUS_TEST_WAIT_S) {
			printf("census_test: NOT ASKED — %u of %d victims "
			       "asleep after %u s (#657)\n", census_test_waiting,
			       i, CENSUS_TEST_WAIT_S);
			thread_wakeup((event_t) &census_test_event);
			thread_bind(current_thread(), PROCESSOR_NULL);
			return;
		}
		mutex_pause();
	}
	if (i < CENSUS_TEST_VICTIMS) {
		thread_wakeup((event_t) &census_test_event);
		thread_bind(current_thread(), PROCESSOR_NULL);
		return;
	}

	printf("census_test: starting: %u threads asleep, woken now while "
	       "processor %d walks the census (#657)\n", CENSUS_TEST_VICTIMS,
	       me);
	thread_wakeup((event_t) &census_test_event);
	n = quiet_census_walk(&unreadable);
	ended = census_test_ending;
	thread_bind(current_thread(), PROCESSOR_NULL);

	printf("census_test: PASS — the census walked %d threads under the "
	       "processor set's lock while %u of the %u woken threads had "
	       "already run on to their end, and skipped %u pointers out of "
	       "them as not readable (#657)\n", n, ended, CENSUS_TEST_VICTIMS,
	       unreadable);
}
