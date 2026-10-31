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
 * And a third way, found by #687 and fixed in #703: a CALLER with an abort
 * pending, which the hand-off parked anyway -- the arm before the swapped one.
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
 * ⚠️ The swapped waiter is a kernel-mode thread of an ORDINARY task, made by
 * task_create_local() with an empty user map.  The kernel_task's threads are
 * unswappable by construction -- kernel_thread() makes them so, and
 * thread_swappable() panics if asked to undo it for one of them -- and a task
 * from kernel_task_create() is the one kind this machine refuses outright:
 * x86-64 has no kernel-loaded tasks (#453), and the first version of this test
 * found that out by panicking in pcb_user_to_kernel().  The thread never
 * leaves the kernel and touches nothing of its task's but the task.
 */

#include <kern/lock.h>			/* mutex_pause() */
#include <kern/misc_protos.h>
#include <kern/sched.h>			/* sched_tick */
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_act.h>
#include <kern/thread_swap.h>
#include <kern/time_out.h>		/* hz */

#include <ipc/ipc_mqueue.h>		/* ipc_dts_declined_swapped */

#include <cpu/spl.h>
#include <trap/handoff_test.h>

static int		hq_word;	/* the futex word's stand-in   */
static int		hq_driver;	/* the waker's own word        */
static int		hq_never;	/* nobody signals this         */

static volatile int	hq_ran;		/* the waiter ran after a wake */
static volatile int	hq_state;	/* its thread state when it did */
static volatile int	hq_swap;	/* its activation's swap state */
static volatile int	hq_quiet;	/* it does not wake the driver */

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
	if (!hq_quiet)
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
 * Whether `th' is parked on `event' -- TH_WAIT and nothing else, the state the
 * hand-off switches onto -- asked once a tick for up to ten seconds.
 */
static int
hq_parked_on(thread_t th, event_t event)
{
	for (int i = 0; i < 10 * hz; i++) {
		spl_t	s = splsched();
		int	parked;

		thread_lock(th);
		parked = (th->state & (TH_SCHED_STATE | TH_SWAPPED_OUT)) == TH_WAIT
			 && th->wait_event == event;
		thread_unlock(th);
		splx(s);

		if (parked)
			return 1;
		mutex_pause();
	}
	return 0;
}

/* On hq_word, the futex word every waiter here sleeps on. */
static int
hq_parked(thread_t th)
{
	return hq_parked_on(th, (event_t) &hq_word);
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

const char *
swap_state_name(int state)
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

	if (task_create_local(kernel_task, FALSE, FALSE, &task) != KERN_SUCCESS) {
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
		       th->state, swap_state_name(act->swap_state));
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
		       hq_state, swap_state_name(hq_swap));
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

/*
 * ── The third arm: the driver hands off with an abort pending (#703) ───
 *
 * thread_abort() sets TH_ABORT under thread_lock and interrupts a wait only if
 * there is one.  On a thread that is not waiting yet, the bit is what makes its
 * next interruptible wait end at once, and thread_block_reason() is the one
 * place that reads it.  The hand-off parks its caller with thread_invoke(),
 * not thread_block(), and did not read it: a futex_wake_wait() aborted after
 * its trap and before its assert_wait() switched onto the waiter and slept on
 * its own word, the abort pending, until somebody woke that word.
 *
 * So the driver -- this thread -- carries the bit into hq_wake(), and the
 * waiter it wakes does NOT wake it back: the driver's word is one nobody
 * wakes, the case where the abort waited for ever.  The hand-off must decline,
 * wake the waiter the ordinary way, and leave the driver's wait to
 * thread_block(), which ends it at once: THREAD_INTERRUPTED.  One that
 * switches leaves the driver asleep until its timeout: THREAD_TIMED_OUT, ten
 * seconds later.
 *
 * ❌ The first version let the waiter wake the driver back, as in the other
 * arms, and called a wait ended by that wakeup a defect.  Under TCG the waiter
 * woken the ordinary way ran on another processor and woke the driver before
 * its thread_block() could answer the abort: THREAD_AWAKENED, with the
 * hand-off declining as it must, and the fixed tree failed 3 boots of 3.
 *
 * ⚠️ THE BIT IS SET HERE BY HAND, as act_abort() sets it on a thread it finds
 * not waiting.  thread_abort() refuses the current activation, and from
 * another thread it would also install a special handler on this one, which a
 * kernel thread never returns to user mode to run.  It is cleared after, as
 * special_handler() clears it on the way to user mode.
 */
static void
hq_aborted_arm(void)
{
	thread_t	self = current_thread();
	thread_t	th;
	boolean_t	handed;
	int		result;
	spl_t		s;

	th = hq_start(kernel_task, FALSE);
	if (th == THREAD_NULL) {
		printf("handoff: WRONG — could not create the waiter for the "
		       "driver with an abort pending (#703)\n");
		return;
	}

	if (!hq_parked(th)) {
		printf("handoff: WRONG — the waiter for the driver with an abort "
		       "pending never parked on its word in ten seconds (#703)\n");
		return;
	}

	hq_ran = 0;
	hq_quiet = 1;
	s = splsched();
	thread_lock(self);
	self->state |= TH_ABORT;
	thread_unlock(self);
	splx(s);

	handed = hq_wake(10 * hz);
	result = self->wait_result;

	s = splsched();
	thread_lock(self);
	self->state &= ~TH_ABORT;
	thread_unlock(self);
	splx(s);

	if (handed) {
		printf("handoff: WRONG — a driver with an abort pending was parked "
		       "by the hand-off, and its wait ended with %d%s (#703)\n",
		       result, result == THREAD_TIMED_OUT ?
		       ", its timeout ten seconds later: the abort waited for a "
		       "wakeup nobody sent" : "");
		hq_quiet = 0;
		return;
	}
	if (result != THREAD_INTERRUPTED) {
		printf("handoff: WRONG — the hand-off declined a driver with an "
		       "abort pending, and thread_block() ended its wait with %d, "
		       "not THREAD_INTERRUPTED (%d) (#703)\n", result,
		       THREAD_INTERRUPTED);
		hq_quiet = 0;
		return;
	}

	/*
	 * The waiter must still have been woken, the ordinary way, and run: a
	 * hand-off that declined by waking nobody would pass the check above
	 * and lose the futex's wakeup.
	 */
	for (int i = 0; i < 10 * hz && !hq_ran; i++)
		mutex_pause();
	hq_quiet = 0;
	if (!hq_ran) {
		printf("handoff: WRONG — the hand-off declined for the driver's "
		       "abort, and the waiter never ran in ten seconds: the "
		       "futex's wakeup was lost (#703)\n");
		return;
	}

	printf("handoff: PASS — a driver with an abort pending was not parked by "
	       "the hand-off: its wait ended at once, THREAD_INTERRUPTED, and the "
	       "waiter was woken the ordinary way and ran (#703)\n");
}

/*
 * ── The fourth arm: a driver that began in a continuation hands off (#771) ─
 *
 * thread_invoke() hands switch_context() the outgoing thread's own
 * continuation field, and each of its callers says first what that is --
 * except the hand-off, which parks its caller keeping its stack and said
 * nothing.  The field held whatever the caller's last block had left: on a
 * thread that last resumed in a continuation, the continuation itself.  Once
 * switch_context() honoured it (#771), the hand-off restarted its caller
 * there, on a fresh stack, instead of letting it come back -- a receive that
 * had already finished, run a second time.
 *
 * So the driver here is that thread.  It blocks with a real continuation,
 * hq_stale_resume, is woken into it, and from inside it does what
 * futex_wait_wake() does; its field still names hq_stale_resume, as a
 * server's still names mach_msg_receive_continue after a receive.  The
 * continuation counts its own beginnings: once is right, twice is the
 * hand-off restarting it.
 *
 * 🔑 And it asks one thing more, on the way in: a driver that comes back from
 * thread_block() at all, instead of beginning in its continuation, was
 * resumed by returning through its frames -- the continuation thrown away,
 * which is what this target did before #771.
 *
 * ⚠️ On a thread of its own and not the one running the boot: a defect here
 * strands the driver for good, and the boot has to go on.
 */
static int			hq_stale_go;	/* wakes the driver into it */
static int			hq_stale_done;	/* the driver's report       */
static volatile int		hq_stale_entries;	/* hq_stale_resume() begun */
static volatile int		hq_stale_returned;	/* thread_block() came back */
static volatile int		hq_stale_back;	/* the hand-off came back    */
static volatile boolean_t	hq_stale_handed;
static void			(*volatile hq_stale_left)(void);

static void
hq_park_for_ever(void)
{
	for (;;) {
		spl_t	s = splsched();

		assert_wait((event_t) &hq_never, FALSE);
		splx(s);
		thread_block((void (*)(void)) 0);
	}
}

/* The continuation.  There is nothing under it to return to, so it parks. */
static void
hq_stale_resume(void)
{
	if (++hq_stale_entries > 1) {
		thread_wakeup((event_t) &hq_stale_done);
		hq_park_for_ever();
	}

	hq_stale_handed = hq_wake(10 * hz);
	hq_stale_left = current_thread()->continuation;
	hq_stale_back = 1;
	thread_wakeup((event_t) &hq_stale_done);
	hq_park_for_ever();
}

static void
hq_stale_driver(void)
{
	spl_t	s;

	s = splsched();
	assert_wait((event_t) &hq_stale_go, TRUE);
	splx(s);
	thread_block(hq_stale_resume);

	/*
	 * Here only if the block came back instead of beginning in the
	 * continuation.  Carried on as the continuation would, so the hand-off
	 * is still asked; the arm reports this first.
	 */
	hq_stale_returned = 1;
	hq_stale_resume();
}

static void
hq_stale_arm(void)
{
	thread_t	waiter, driver;
	spl_t		s;

	waiter = hq_start(kernel_task, FALSE);
	if (waiter == THREAD_NULL || !hq_parked(waiter)) {
		printf("handoff: WRONG — the waiter for the driver that began in "
		       "a continuation was not created or never parked on its "
		       "word (#771)\n");
		return;
	}

	driver = kernel_thread(kernel_task, hq_stale_driver, (void *) 0);
	if (driver == THREAD_NULL
	    || !hq_parked_on(driver, (event_t) &hq_stale_go)) {
		printf("handoff: WRONG — the driver never parked to be woken into "
		       "its continuation (#771)\n");
		return;
	}

	s = splsched();
	assert_wait((event_t) &hq_stale_done, FALSE);
	thread_set_timeout(10 * hz);
	splx(s);
	thread_wakeup((event_t) &hq_stale_go);
	thread_block((void (*)(void)) 0);

	if (hq_stale_returned) {
		printf("handoff: WRONG — a thread that blocked with a continuation "
		       "came back from thread_block() instead of beginning in it: "
		       "the switch threw the continuation away (#771)\n");
		return;
	}
	if (hq_stale_entries > 1) {
		printf("handoff: WRONG — the hand-off restarted its caller in the "
		       "continuation it had already begun: hq_stale_resume began "
		       "%d times, the caller's field still naming it (#771)\n",
		       hq_stale_entries);
		return;
	}
	if (!hq_stale_back) {
		printf("handoff: WRONG — the driver that began in its continuation "
		       "never came back from the hand-off in ten seconds (#771)\n");
		return;
	}
	if (!hq_stale_handed) {
		printf("handoff: NOT ASKED — the hand-off declined the waiter, so "
		       "it never switched away with the driver's field in play "
		       "(#771)\n");
		return;
	}
	printf("handoff: PASS — a driver that began in its continuation handed "
	       "off to a parked waiter and came back to it, beginning once; its "
	       "continuation field read %p afterwards (#771)\n",
	       (void *) hq_stale_left);
}

/*
 * The stopped arm first, and not for tidiness.  The old wake put a waiter that
 * was not parked on a run queue with thread_setrun(), which asserts the thread
 * is not swapped out -- so under ablations/607-old-wake.patch the swapped arm ends
 * the boot in that assertion, and the stopped arm, run second, would never say
 * what it is there to say.  In this order each ablation shows its own arm, and
 * the aborted arm goes between them for the same reason.
 */
void
handoff_wake_test(void)
{
	printf("handoff: a futex waiter stopped, a driver with an abort pending, "
	       "a waiter swapped out, then a driver that began in a "
	       "continuation, each through the hand-off (#607, #703, #771)\n");
	hq_stopped_arm();
	hq_aborted_arm();
	hq_swapped_arm();
	hq_stale_arm();
}

/*
 * ── -O: the thread swapper, made aggressive (#607) ─────────────────────
 *
 * Every second, for the whole boot, the swapper's own scan with maxslp at zero:
 * every user thread that has waited interruptibly for one scheduler tick is
 * swapped out.  So every wake path in the system -- the futex hand-off, the
 * IPC direct switch (with -N), thread_go(), clear_wait(), the timeouts -- meets
 * swapped-out threads during an ordinary run of the userland, where on any
 * other boot it would meet none.
 *
 * 🔑 WHAT IT EXISTS TO REACH IS COUNTED, and printed after every scan that
 * changed a count: threads swapped out and back in, receivers the IPC direct
 * switch declined because they were swapped out, waiters the futex hand-off
 * declined for the same reason.  A storm whose counts stay at zero has tested
 * nothing, and says so every thirty scans.
 *
 * ❌ The first version printed every fifteen scans, and a whole userland boot
 * under KVM is over in about nineteen seconds: six runs passed without one
 * count printed, which is six runs that proved nothing about swapping.
 */
static int	storm_tick;

static void
swapper_storm(void)
{
	unsigned int	scans = 0;
	unsigned int	out = 0, in = 0, dts = 0, handoff = 0;

	maxslp = 0;

	for (;;) {
		spl_t	s;

		s = splsched();
		assert_wait((event_t) &storm_tick, FALSE);
		thread_set_timeout(hz);
		splx(s);
		thread_block((void (*)(void)) 0);

		swapout_scan();
		scans++;

		if (thread_swapouts != out || thread_swapins != in
		    || ipc_dts_declined_swapped != dts
		    || handoff_declined_swapped != handoff) {
			out = thread_swapouts;
			in = thread_swapins;
			dts = ipc_dts_declined_swapped;
			handoff = handoff_declined_swapped;
			printf("swapper storm: after %u scans, %u threads swapped "
			       "out, %u back in; declined while swapped out: %u "
			       "by the IPC direct switch, %u by the futex hand-off "
			       "(#607)\n", scans, out, in, dts, handoff);
		} else if (out == 0 && scans % 30 == 0) {
			printf("swapper storm: %u scans and NOTHING SWAPPED, so "
			       "nothing was tested yet (#607)\n", scans);
		}
	}
}

void
swapper_storm_start(void)
{
	printf("swapper storm: every second, the thread swapper's own scan with "
	       "maxslp 0 -- every thread asleep for a tick is swapped out "
	       "(#607)\n");
	(void) kernel_thread(kernel_task, swapper_storm, (char *) 0);
}
