/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -q: a processor set's thread-changing calls come back (#787, section 9).
 *
 * processor_set_policy_disable() and processor_set_limit(), asked to change
 * the threads of a set, hold each thread's activation lock (thread_lock_act())
 * and called thread_policy() or thread_max_priority(), which take that mutex
 * again (act_lock_thread()).  The x86-64 mutex has no owner check, so the
 * caller slept for good holding the activation's lock and the set's lock, and
 * with the set's lock every thread and task creation.  It is reachable without
 * privileges on a set of the task's own.
 *
 * Here the test makes a set of its own, enables FIFO on it, and puts a worker
 * in it under FIFO.  A driver thread then calls processor_set_limit() and
 * processor_set_policy_disable() on the set, each of which walks the set's
 * threads and reaches the worker; after each it advances a phase.  This
 * thread watches the phase with a bound of PSET_TEST_WAIT ticks: a call that
 * does not come back is asleep on the lock it holds, and the test says which.
 * With the fix the two calls use the _locked variants and come back, and the
 * worker, FIFO before, is TIMESHARE after the disable.  One processor is
 * enough: the deadlock is a thread sleeping on a mutex it holds, so the driver
 * yields to this thread, which sees the phase stalled.
 */

#include <kern/host.h>			/* realhost */
#include <kern/lock.h>			/* mutex_pause */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched.h>			/* BASEPRI_USER */
#include <kern/sched_prim.h>
#include <kern/spl.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_act.h>
#include <kern/time_out.h>		/* timeout_ticks */

#include <mach/policy.h>
#include <mach/mach_host_server.h>	/* processor_set_* , thread_assign,
					   thread_set_policy */

#include <cpu/pset_test.h>

/* Internal, no MIG route of its own (#787): declared here to call it. */
extern kern_return_t	processor_set_limit(processor_set_t pset,
					    policy_t policy,
					    policy_limit_t limit,
					    boolean_t change);

#define	PSET_TEST_WAIT	300		/* ticks: three seconds at 100 Hz */

static processor_set_t		pset_test_set;
static thread_t			pset_test_worker;
static volatile int		pset_test_phase;	/* driver advances it */
static volatile int		pset_test_worker_parked;
static volatile int		pset_test_worker_stop;
static volatile int		pset_test_worker_gone;
static int			pset_test_event;	/* the worker blocks on */
static kern_return_t		pset_test_limit_kr;
static kern_return_t		pset_test_disable_kr;

static void
pset_test_worker_fn(void)
{
	thread_t	self = current_thread();
	spl_t		s;

	pset_test_worker_parked = 1;
	for (;;) {
		s = splsched();
		thread_lock(self);
		if (pset_test_worker_stop) {
			thread_unlock(self);
			splx(s);
			break;
		}
		thread_unlock(self);
		splx(s);
		assert_wait((event_t) &pset_test_event, FALSE);
		if (pset_test_worker_stop) {
			clear_wait(self, THREAD_AWAKENED, FALSE);
			break;
		}
		thread_block((void (*)(void)) 0);
	}
	pset_test_worker_gone = 1;
	thread_terminate_self();
}

static void
pset_test_driver_fn(void)
{
	policy_fifo_limit_data_t	fifo_limit;

	fifo_limit.max_priority = BASEPRI_USER;

	pset_test_limit_kr = processor_set_limit(pset_test_set, POLICY_FIFO,
						 (policy_limit_t) &fifo_limit,
						 TRUE);
	pset_test_phase = 2;

	pset_test_disable_kr = processor_set_policy_disable(pset_test_set,
							    POLICY_FIFO, TRUE);
	pset_test_phase = 3;

	thread_terminate_self();
}

/* Wait for pset_test_phase to reach `want', PSET_TEST_WAIT ticks at most. */
static boolean_t
pset_test_reach(int want)
{
	natural_t	t0 = timeout_ticks;

	while (pset_test_phase < want) {
		if (timeout_ticks - t0 >= PSET_TEST_WAIT)
			return FALSE;
		mutex_pause();
	}
	return TRUE;
}

void
pset_test(void)
{
	processor_set_t			name;
	thread_t			worker, driver;
	thread_act_t			worker_act;
	policy_fifo_base_data_t		fifo_base;
	policy_fifo_limit_data_t	fifo_limit;
	kern_return_t			kr;
	int				worker_policy = -1;
	const char			*stuck = NULL;
	natural_t			t0;
	spl_t				s;

	/*
	 * A set of the test's own, FIFO enabled: the policy this test disables,
	 * and the one a task may give a thread of its own (#274, #786).
	 */
	kr = processor_set_create(&realhost, &pset_test_set, &name);
	if (kr != KERN_SUCCESS) {
		printf("pset_test: NOT ASKED — processor_set_create returned "
		       "0x%x (#787)\n", kr);
		return;
	}
	pset_deallocate(name);		/* the name right: unused here */

	kr = processor_set_policy_enable(pset_test_set, POLICY_FIFO);
	if (kr != KERN_SUCCESS) {
		printf("pset_test: NOT ASKED — FIFO could not be enabled on "
		       "the set: 0x%x (#787)\n", kr);
		processor_set_destroy(pset_test_set);
		return;
	}

	pset_test_phase = 1;
	pset_test_worker_parked = 0;
	pset_test_worker_stop = 0;
	pset_test_worker_gone = 0;

	worker = kernel_thread(kernel_task, pset_test_worker_fn, (char *) 0);
	if (worker == THREAD_NULL) {
		printf("pset_test: NOT ASKED — no worker thread (#787)\n");
		processor_set_destroy(pset_test_set);
		return;
	}
	pset_test_worker = worker;

	t0 = timeout_ticks;
	while (!pset_test_worker_parked) {
		if (timeout_ticks - t0 >= PSET_TEST_WAIT) {
			printf("pset_test: NOT ASKED — the worker did not park "
			       "(#787)\n");
			pset_test_worker_stop = 1;
			thread_wakeup((event_t) &pset_test_event);
			return;
		}
		mutex_pause();
	}

	/* Into the set, then FIFO there: now it is what the two calls walk. */
	worker_act = worker->top_act;
	kr = thread_assign(worker_act, pset_test_set);
	if (kr != KERN_SUCCESS) {
		printf("pset_test: NOT ASKED — thread_assign to the set "
		       "returned 0x%x (#787)\n", kr);
		goto stop_worker;
	}
	fifo_base.base_priority = BASEPRI_USER;
	fifo_limit.max_priority = BASEPRI_USER;
	kr = thread_set_policy(worker_act, pset_test_set, POLICY_FIFO,
			       (policy_base_t) &fifo_base, POLICY_FIFO_BASE_COUNT,
			       (policy_limit_t) &fifo_limit,
			       POLICY_FIFO_LIMIT_COUNT);
	if (kr != KERN_SUCCESS) {
		printf("pset_test: NOT ASKED — the worker could not be made "
		       "FIFO: 0x%x (#787)\n", kr);
		goto stop_worker;
	}

	printf("pset_test: starting: processor_set_limit() then "
	       "processor_set_policy_disable() on a set with a FIFO worker, "
	       "each must come back (#787)\n");

	driver = kernel_thread(kernel_task, pset_test_driver_fn, (char *) 0);
	if (driver == THREAD_NULL) {
		printf("pset_test: NOT ASKED — no driver thread (#787)\n");
		goto stop_worker;
	}

	if (!pset_test_reach(2))
		stuck = "processor_set_limit";
	else if (!pset_test_reach(3))
		stuck = "processor_set_policy_disable";

	if (stuck == NULL) {
		s = splsched();
		thread_lock(worker);
		worker_policy = worker->policy;
		thread_unlock(worker);
		splx(s);
	}

stop_worker:
	/*
	 * The verdict first, then the cleanup -- the verdict does not depend
	 * on it, and a cleanup that hangs must not swallow the verdict.
	 */
	if (stuck != NULL)
		printf("pset_test: WRONG — %s did not come back in %u ticks: "
		       "it is asleep on the activation lock it already holds, "
		       "and the set's lock with it (#787)\n", stuck,
		       PSET_TEST_WAIT);
	else if (kr != KERN_SUCCESS)
		; /* a NOT ASKED was already printed above */
	else if (pset_test_limit_kr != KERN_SUCCESS ||
		 pset_test_disable_kr != KERN_SUCCESS)
		printf("pset_test: WRONG — both calls came back but one "
		       "failed: limit 0x%x, disable 0x%x (#787)\n",
		       pset_test_limit_kr, pset_test_disable_kr);
	else if (worker_policy != POLICY_TIMESHARE)
		printf("pset_test: WRONG — both calls came back, but the "
		       "worker's policy is 0x%x, not TIMESHARE: the disable "
		       "did not change it (#787)\n", worker_policy);
	else
		printf("pset_test: PASS — processor_set_limit() and "
		       "processor_set_policy_disable() both came back, and the "
		       "FIFO worker is TIMESHARE after the disable (#787)\n");

	/*
	 * Best effort: let the worker terminate itself, so it does not stay a
	 * parked kernel thread for the rest of the boot.  The set it was in is
	 * left behind -- harmless, and destroying it here would race the
	 * worker's own removal from it.  Never woken while a call is stuck:
	 * the driver holds the worker's locks for good then.
	 */
	if (stuck == NULL) {
		pset_test_worker_stop = 1;
		thread_wakeup((event_t) &pset_test_event);
	}
}
