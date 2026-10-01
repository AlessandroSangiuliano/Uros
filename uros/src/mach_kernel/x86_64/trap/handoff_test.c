/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The futex hand-off must wake a waiter it does not switch to the way every
 * other wakeup does (#607).
 *
 * thread_handoff_to_parked_waiter() switches straight onto a waiter that is
 * parked -- TH_WAIT and nothing else -- and wakes any other the ordinary way.
 * It had two ways of getting that wrong, and -Q has an arm for each.
 *
 *   The first arm is a waiter the THREAD SWAPPER has swapped out.  It is
 *   TH_WAIT|TH_SWAPPED_OUT, the scheduling-state mask did not name the second
 *   bit, so it counted as parked and the hand-off switched onto a kernel stack
 *   thread_swapout() had unwired.
 *
 *   The second is a waiter thread_stop() has stopped, TH_WAIT|TH_SUSP.  The
 *   hand-off's own copy of the wake left it TH_RUN on no run queue, and
 *   thread_unstop(), finding TH_RUN, never put it on one: it never ran again.
 *
 * ── Built, not waited for ─────────────────────────────────────────────
 *
 * Neither happens on a boot by itself.  The swapper takes a thread only after
 * it has slept MAXSLP scheduler ticks -- ten seconds -- and only when the
 * pageout daemon asks it to; a stopped futex waiter needs a debugger or a
 * signal to arrive at the right instant.  So each arm puts its waiter in that
 * state with the kernel's own code, and then does what futex_wait_wake() does:
 * declares its own wait, hands off, and blocks if the hand-off declined.
 *
 * 🔑 THE SWAPPER'S OWN SCAN DOES THE SWAPPING, not this file.  maxslp is a
 * tunable the swapper documents as one to poke, and at zero one scheduler tick
 * of sleep is enough; everything else the scan asks -- an active, swappable,
 * interruptibly waiting thread of a task that is not the kernel's -- it asks
 * as it always does.  A test that marked the waiter by hand would be testing
 * its own idea of what the swapper does.
 *
 * ⚠️ The swapped waiter belongs to a task made by kernel_task_create(),
 * because the kernel_task's threads are unswappable by construction:
 * kernel_thread() makes them so, and thread_swappable() panics if asked to
 * undo it for one of them.
 */

#include <mach/mach_server.h>		/* kernel_task_create() */

#include <kern/lock.h>			/* mutex_pause() */
#include <kern/misc_protos.h>
#include <kern/sched.h>			/* sched_tick */
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_act.h>
#include <kern/thread_swap.h>
#include <kern/time_out.h>		/* hz */

#include <cpu/spl.h>
#include <trap/handoff_test.h>

static int		hq_word;	/* the futex word's stand-in   */
static int		hq_driver;	/* the waker's own word        */
static int		hq_never;	/* nobody signals this         */

static volatile int	hq_ran;		/* the waiter ran after a wake */
static volatile int	hq_state;	/* its thread state when it did */
static volatile int	hq_swap;	/* its activation's swap state */

/*
 * The waiter, as futex_wait() leaves one: asleep interruptibly on its word.
 *
 * What it records first, once it runs, is the whole verdict, so it records
 * before it does anything that could depend on that state being right.
 */
static void
hq_waiter(void)
{
	spl_t	s;

	s = splsched();
	assert_wait((event_t) &hq_word, TRUE);
	splx(s);
	thread_block((void (*)(void)) 0);

	hq_state = current_thread()->state;
	hq_swap = current_act()->swap_state;
	hq_ran = 1;
	thread_wakeup((event_t) &hq_driver);

	/*
	 * ⚠️ And then nothing, for ever.  A waiter switched onto while still
	 * marked swapped out must not reach a run queue again --
	 * thread_setrun() asserts it may not -- so the test gives it no reason
	 * to.  The arm that sees it has already printed what it saw.
	 */
	for (;;) {
		s = splsched();
		assert_wait((event_t) &hq_never, FALSE);
		splx(s);
		thread_block((void (*)(void)) 0);
	}
}

/* Start a waiter in `task', as the -W test starts its probe. */
static thread_t
hq_start(task_t task, int swappable)
{
	thread_t	th;
	thread_act_t	act;
	spl_t		s;

	if (thread_create_at(task, &th, hq_waiter) != KERN_SUCCESS)
		return THREAD_NULL;

	act = th->top_act;
	if (!swappable)
		thread_swappable(act, FALSE);

	s = splsched();
	thread_lock(th);
	th->state |= TH_RUN;
	thread_setrun(th, TRUE, TAIL_Q);
	thread_unlock(th);
	splx(s);

	act_deallocate(act);
	thread_resume(act);
	return th;
}

/*
 * Whether `th' is parked on hq_word -- TH_WAIT and nothing else, the state the
 * hand-off switches onto -- asked once a tick for up to ten seconds.
 */
static int
hq_parked(thread_t th)
{
	for (int i = 0; i < 10 * hz; i++) {
		spl_t	s = splsched();
		int	parked;

		thread_lock(th);
		parked = (th->state & (TH_SCHED_STATE | TH_SWAPPED_OUT)) == TH_WAIT
			 && th->wait_event == (event_t) &hq_word;
		thread_unlock(th);
		splx(s);

		if (parked)
			return 1;
		mutex_pause();
	}
	return 0;
}

/*
 * What futex_wait_wake() does once its own wait is declared: hand off, and
 * block if the hand-off declined.
 *
 * ⚠️ Bounded, because the failing arms are a waiter that never runs, and a
 * driver that waited for it for ever would report the defect as silence.
 */
static boolean_t
hq_wake(int bound_ticks)
{
	boolean_t	handed;
	spl_t		s;

	s = splsched();
	assert_wait((event_t) &hq_driver, TRUE);
	thread_set_timeout(bound_ticks);
	splx(s);

	handed = thread_handoff_to_parked_waiter((event_t) &hq_word);
	if (!handed)
		thread_block((void (*)(void)) 0);
	return handed;
}

static const char *
hq_swap_name(int state)
{
	switch (state & TH_SW_STATE) {
	case TH_SW_UNSWAPPABLE:	return "unswappable";
	case TH_SW_IN:		return "in";
	case TH_SW_GOING_OUT:	return "going out";
	case TH_SW_WANT_IN:	return "wanted in";
	case TH_SW_OUT:		return "out";
	case TH_SW_COMING_IN:	return "coming in";
	default:		return "unknown";
	}
}

static void
hq_swapped_arm(void)
{
	task_t		task;
	thread_t	th;
	thread_act_t	act;
	boolean_t	handed;
	int		old, out;
	spl_t		s;

	if (kernel_task_create(kernel_task, 0, 0, &task) != KERN_SUCCESS) {
		printf("handoff: WRONG — could not create a task for the waiter "
		       "the swapper is to take (#607)\n");
		return;
	}

	th = hq_start(task, TRUE);
	if (th == THREAD_NULL) {
		printf("handoff: WRONG — could not create the waiter the "
		       "swapper is to take (#607)\n");
		return;
	}
	act = th->top_act;

	if (!hq_parked(th)) {
		printf("handoff: WRONG — the waiter never parked on its word in "
		       "ten seconds (#607)\n");
		return;
	}

	/*
	 * One scheduler tick of sleep -- sched_tick counts seconds -- and then
	 * the scan, with the bar lowered to it and put back after.
	 */
	for (int i = 0; sched_tick <= th->sleep_stamp && i < 5 * hz; i++)
		mutex_pause();

	old = maxslp;
	maxslp = 0;
	swapout_scan();
	maxslp = old;

	s = splsched();
	thread_lock(th);
	out = (th->state & TH_SWAPPED_OUT) != 0
	      && (act->swap_state & TH_SW_STATE) == TH_SW_OUT;
	thread_unlock(th);
	splx(s);

	/*
	 * 🔑 NOT ASKED (#563).  A scan that did not take the waiter has asked
	 * the hand-off nothing, and calling that a pass would be the lie this
	 * project keeps catching in its own tests.
	 */
	if (!out) {
		printf("handoff: NOT ASKED — the swapper's scan did not take the "
		       "waiter (state 0x%x, swap state %s), so nothing was asked "
		       "of the hand-off (#607)\n",
		       th->state, hq_swap_name(act->swap_state));
		return;
	}

	hq_ran = 0;
	handed = hq_wake(10 * hz);

	if (!hq_ran) {
		printf("handoff: WRONG — the swapped-out waiter never ran in ten "
		       "seconds after the hand-off %s it (#607)\n",
		       handed ? "switched onto" : "woke");
		return;
	}

	if (handed || (hq_state & TH_SWAPPED_OUT)
	    || (hq_swap & TH_SW_STATE) != TH_SW_IN) {
		printf("handoff: WRONG — the hand-off %s a waiter the swapper had "
		       "swapped out, and it ran with state 0x%x and swap state "
		       "%s: on a kernel stack thread_swapout() had unwired "
		       "(#607)\n", handed ? "switched onto" : "woke",
		       hq_state, hq_swap_name(hq_swap));
		return;
	}

	printf("handoff: PASS — a futex waiter the swapper's own scan swapped out "
	       "was not switched onto: the hand-off declined it, the swapin "
	       "thread brought it back, and it ran swapped in (state 0x%x) "
	       "(#607)\n", hq_state);
}

static void
hq_stopped_arm(void)
{
	thread_t	th;
	boolean_t	handed;
	int		after;
	spl_t		s;

	th = hq_start(kernel_task, FALSE);
	if (th == THREAD_NULL) {
		printf("handoff: WRONG — could not create the waiter to stop "
		       "(#607)\n");
		return;
	}

	if (!hq_parked(th)) {
		printf("handoff: WRONG — the waiter to stop never parked on its "
		       "word in ten seconds (#607)\n");
		return;
	}

	if (!thread_stop(th)) {
		printf("handoff: WRONG — thread_stop() could not stop the parked "
		       "waiter (#607)\n");
		return;
	}

	/*
	 * One second: a stopped waiter cannot answer, so this wait ends on its
	 * timeout -- unless the hand-off ran the waiter, which is the defect.
	 */
	hq_ran = 0;
	handed = hq_wake(hz);

	s = splsched();
	thread_lock(th);
	after = th->state;
	thread_unlock(th);
	splx(s);

	if (handed || hq_ran) {
		printf("handoff: WRONG — the hand-off %s a waiter thread_stop() "
		       "had stopped (#607)\n",
		       handed ? "switched onto" : "ran");
		return;
	}

	hq_ran = 0;
	s = splsched();
	assert_wait((event_t) &hq_driver, TRUE);
	thread_set_timeout(10 * hz);
	splx(s);
	thread_unstop(th);
	thread_block((void (*)(void)) 0);

	if (!hq_ran) {
		printf("handoff: WRONG — a stopped waiter the hand-off woke never "
		       "ran after thread_unstop(): its state was 0x%x after the "
		       "hand-off, TH_RUN on no run queue (#607)\n", after);
		return;
	}

	printf("handoff: PASS — a futex waiter thread_stop() had stopped stayed "
	       "stopped when the hand-off woke it (state 0x%x), and ran once "
	       "thread_unstop() let it go (#607)\n", after);
}

void
handoff_wake_test(void)
{
	printf("handoff: a futex waiter swapped out, then one stopped, each woken "
	       "through the hand-off (#607)\n");
	hq_swapped_arm();
	hq_stopped_arm();
}
