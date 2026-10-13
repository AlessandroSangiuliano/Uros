/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * thread_swap_disable() on an activation the thread swapper is moving (#642).
 *
 * thread_terminate() makes its target unswappable before anything else, so
 * that the stacks are wired when they are freed.  A target found swapped in is
 * made so on the spot.  One found anywhere else used to be swapped in from
 * there, while someone else might be moving it, and a second swap-in of the
 * same stack is what thread_doswapin()'s assertion caught, once, on entry 28.
 * Now the swap-in is asked of the swapper and waited for.  -Q asks
 * thread_swap_disable() in each state it can meet:
 *
 *   [1] coming in: woken, so queued for the swapin thread, and asked before
 *       the swapin thread is done with it.  The state the panic was found in.
 *   [2] going out: asked while thread_swapout() unwires its stack.  A second
 *       thread asks, because the swapper's scan runs on this one.
 *   [3] out, and woken by a second thread while the swap-in runs: the wakeup
 *       queues it for the swapin thread as well.
 *
 * Each arm checks what thread_swap_disable() promises: the activation comes
 * back unswappable and swapped in, swapped in once, and a waiter that was woken
 * runs.  A second swap-in ends the boot in thread_doswapin()'s assertion: that
 * is how ablations/642-direct-swapin.patch shows.
 *
 * 🔑 NOT ASKED (#563) when an arm did not meet its state: thread_swap_disable()
 * found the activation already in, or the wakeup of [3] came after the swap-in.
 * The windows are short.  Without ablations/642-widen-swap.patch, which holds
 * thread_doswapin() and thread_swapout() open for the arm's waiter, [2] and [3]
 * meet their state only now and then.
 *
 * ── Built as -Q's #607 arms are ─────────────────────────────────────────
 *
 * The waiters are kernel-mode threads of an ordinary task, swapped out by the
 * swapper's own scan with maxslp at zero; handoff_test.c says why both.
 * thread_swap_disable() is called directly rather than through
 * thread_terminate(): the defect and the fix are both in it, and the rest of a
 * termination would only add to what a failure could mean.
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

#include <cpu/spl.h>
#include <trap/handoff_test.h>		/* swap_state_name() */
#include <trap/swap_disable_test.h>

/* A waiter, as the swapper takes one: asleep interruptibly on its word. */
struct sd_waiter {
	thread_t	th;
	thread_act_t	act;
	int		word;		/* what it sleeps on */
	volatile int	woken;		/* times it ran after a wake */
};

static struct sd_waiter	sd_waiters[3];
static struct sd_waiter	*volatile sd_starting;	/* the waiter being started */

/* The second thread of [2] and [3], and what it reports. */
static struct sd_waiter	*volatile sd_helped;
static volatile int	sd_go;		/* the arm says when */
static volatile int	sd_met;		/* [2]: the swap state it met; [3]: still out */
static volatile int	sd_done;
static int		sd_never;	/* nobody signals this */

static void
sd_waiter(void)
{
	struct sd_waiter	*w = sd_starting;
	spl_t			s;

	for (;;) {
		s = splsched();
		assert_wait((event_t) &w->word, TRUE);
		splx(s);
		thread_block((void (*)(void)) 0);
		w->woken++;
	}
}

/* Once a helper has said what it saw, it waits for ever. */
static void
sd_park(void)
{
	spl_t	s;

	for (;;) {
		s = splsched();
		assert_wait((event_t) &sd_never, FALSE);
		splx(s);
		thread_block((void (*)(void)) 0);
	}
}

/*
 * Where ablations/642-widen-swap.patch tells the swapper which activation to
 * hold open.  Nothing, without it.
 */
static void
sd_window(thread_act_t act)
{
	(void) act;
}

static int
sd_swap_state(struct sd_waiter *w, int *thread_state)
{
	int	swap;
	spl_t	s;

	s = splsched();
	thread_lock(w->th);
	swap = w->act->swap_state;
	if (thread_state)
		*thread_state = w->th->state;
	thread_unlock(w->th);
	splx(s);
	return swap;
}

/* Start a waiter in `task', as -Q's #607 arms do, and wait for it to park. */
static int
sd_start(task_t task, struct sd_waiter *w)
{
	thread_t	th;
	spl_t		s;

	sd_starting = w;
	if (thread_create_at(task, &th, sd_waiter) != KERN_SUCCESS)
		return 0;
	w->th = th;
	w->act = th->top_act;

	s = splsched();
	thread_lock(th);
	th->state |= TH_RUN;
	thread_setrun(th, TRUE, TAIL_Q);
	thread_unlock(th);
	splx(s);

	act_deallocate(w->act);
	thread_resume(w->act);

	for (int i = 0; i < 10 * hz; i++) {
		int	parked;

		s = splsched();
		thread_lock(th);
		parked = (th->state & (TH_SCHED_STATE | TH_SWAPPED_OUT)) == TH_WAIT
			 && th->wait_event == (event_t) &w->word;
		thread_unlock(th);
		splx(s);
		if (parked)
			return 1;
		mutex_pause();
	}
	return 0;
}

/*
 * One scheduler tick of sleep -- sched_tick counts seconds -- and then the
 * swapper's own scan, with the bar lowered to it and put back after.
 */
static void
sd_scan(struct sd_waiter *w)
{
	int	old;

	for (int i = 0; sched_tick <= w->th->sleep_stamp && i < 5 * hz; i++)
		mutex_pause();
	old = maxslp;
	maxslp = 0;
	swapout_scan();
	maxslp = old;
}

static int
sd_is_out(struct sd_waiter *w)
{
	int	state;
	int	swap = sd_swap_state(w, &state);

	return (state & TH_SWAPPED_OUT) != 0
	       && (swap & TH_SW_STATE) == TH_SW_OUT;
}

/*
 * What thread_swap_disable() promises, asked once it has returned: the
 * activation unswappable and swapped in, swapped in once since `ins' was read,
 * and -- unless `woken' is -1 -- the waiter run since it was woken.
 *
 * ⚠️ thread_swapins counts every thread's swap-ins.  At -Q's point in the boot
 * there is no userland, and the waiters of earlier arms are unswappable, so the
 * arm's waiter is the only thread that can be swapped in.
 */
static void
sd_verdict(int arm, struct sd_waiter *w, const char *met, unsigned int ins,
	   int woken)
{
	int		state, swap, ran;
	unsigned int	swapins;

	for (int i = 0; woken >= 0 && w->woken == woken && i < 10 * hz; i++)
		mutex_pause();

	swap = sd_swap_state(w, &state);
	swapins = thread_swapins - ins;
	ran = woken < 0 || w->woken != woken;

	if (swap != TH_SW_UNSWAPPABLE || (state & TH_SWAPPED_OUT) != 0
	    || swapins != 1 || !ran) {
		printf("swap_disable: [%d] WRONG — thread_swap_disable() met the "
		       "waiter %s, and after it the swap state is %s, the thread "
		       "state 0x%x, %u swap-ins since it went out%s (#642)\n",
		       arm, met, swap_state_name(swap), state, swapins,
		       ran ? "" : ", and the woken waiter never ran");
		return;
	}
	printf("swap_disable: [%d] PASS — thread_swap_disable() met the waiter "
	       "%s, waited, and it came back unswappable and swapped in, once%s "
	       "(#642)\n", arm, met,
	       woken >= 0 ? "; woken, it ran" : "");
}

static void
sd_coming_in_arm(task_t task)
{
	struct sd_waiter	*w = &sd_waiters[0];
	unsigned int		waits, ins;
	int			woken, swap;

	printf("swap_disable: [1] starting: a waiter swapped out, woken, and "
	       "asked about before the swapin thread is done with it (#642)\n");
	if (!sd_start(task, w)) {
		printf("swap_disable: [1] WRONG — the waiter never parked on its "
		       "word in ten seconds (#642)\n");
		return;
	}
	sd_scan(w);
	if (!sd_is_out(w)) {
		printf("swap_disable: [1] NOT ASKED — the swapper's scan did not "
		       "take the waiter (swap state %s), so nothing was asked "
		       "(#642)\n", swap_state_name(sd_swap_state(w, NULL)));
		return;
	}

	sd_window(w->act);
	waits = thread_swap_disable_waits;
	ins = thread_swapins;
	woken = w->woken;
	thread_wakeup((event_t) &w->word);	/* out: queued for the swapin thread */
	swap = sd_swap_state(w, NULL);
	thread_swap_disable(w->act);
	sd_window(THR_ACT_NULL);

	if (thread_swap_disable_waits == waits) {
		printf("swap_disable: [1] NOT ASKED — the swapin thread was done "
		       "before thread_swap_disable() looked, and it found the "
		       "waiter in (#642)\n");
		return;
	}
	sd_verdict(1, w, swap_state_name(swap), ins, woken);
}

/*
 * [2]'s second thread: waits for the waiter to be going out, and asks then.
 * It looks once a tick, which the widened window holds open for many; without
 * it the state is met by chance.
 */
static void
sd_going_out_asker(void)
{
	struct sd_waiter	*w = sd_helped;
	int			swap;

	while (!sd_go)
		mutex_pause();
	for (int i = 0; i < 10 * hz; i++) {
		swap = w->act->swap_state;
		if ((swap & TH_SW_STATE) == TH_SW_GOING_OUT) {
			sd_met = swap;
			thread_swap_disable(w->act);
			break;
		}
		if ((swap & TH_SW_STATE) == TH_SW_OUT)
			break;
		mutex_pause();
	}
	sd_done = 1;
	sd_park();
}

static void
sd_going_out_arm(task_t task)
{
	struct sd_waiter	*w = &sd_waiters[1];
	unsigned int		waits, ins;

	printf("swap_disable: [2] starting: a waiter asked about by a second "
	       "thread while the swapper's scan unwires its stack (#642)\n");
	if (!sd_start(task, w)) {
		printf("swap_disable: [2] WRONG — the waiter never parked on its "
		       "word in ten seconds (#642)\n");
		return;
	}

	sd_helped = w;
	sd_go = 0;
	sd_met = -1;
	sd_done = 0;
	(void) kernel_thread(kernel_task, sd_going_out_asker, (void *) 0);

	sd_window(w->act);
	waits = thread_swap_disable_waits;
	ins = thread_swapins;
	sd_go = 1;
	sd_scan(w);
	for (int i = 0; !sd_done && i < 10 * hz; i++)
		mutex_pause();
	sd_window(THR_ACT_NULL);

	if (!sd_done) {
		printf("swap_disable: [2] WRONG — the second thread was not done "
		       "in ten seconds: thread_swap_disable() never returned "
		       "(#642)\n");
		return;
	}
	if (sd_met < 0 || thread_swap_disable_waits == waits) {
		printf("swap_disable: [2] NOT ASKED — the second thread never saw "
		       "the waiter going out (swap state %s now), so nothing was "
		       "asked (#642)\n", swap_state_name(sd_swap_state(w, NULL)));
		return;
	}
	sd_verdict(2, w, swap_state_name(sd_met), ins, -1);
}

/*
 * [3]'s second thread: once the arm says go, wakes the waiter a tick later --
 * inside the swap-in, when the widened window holds it open -- and records
 * whether the waiter was still swapped out just before.
 */
static void
sd_out_waker(void)
{
	struct sd_waiter	*w = sd_helped;
	int			state;

	while (!sd_go)
		mutex_pause();
	mutex_pause();
	(void) sd_swap_state(w, &state);
	sd_met = (state & TH_SWAPPED_OUT) != 0;
	thread_wakeup((event_t) &w->word);
	sd_done = 1;
	sd_park();
}

static void
sd_out_woken_arm(task_t task)
{
	struct sd_waiter	*w = &sd_waiters[2];
	unsigned int		waits, ins;
	int			woken;

	printf("swap_disable: [3] starting: a waiter swapped out, asked about, "
	       "and woken by a second thread while the swap-in runs (#642)\n");
	if (!sd_start(task, w)) {
		printf("swap_disable: [3] WRONG — the waiter never parked on its "
		       "word in ten seconds (#642)\n");
		return;
	}
	sd_scan(w);
	if (!sd_is_out(w)) {
		printf("swap_disable: [3] NOT ASKED — the swapper's scan did not "
		       "take the waiter (swap state %s), so nothing was asked "
		       "(#642)\n", swap_state_name(sd_swap_state(w, NULL)));
		return;
	}

	sd_helped = w;
	sd_go = 0;
	sd_met = -1;
	sd_done = 0;
	(void) kernel_thread(kernel_task, sd_out_waker, (void *) 0);

	sd_window(w->act);
	waits = thread_swap_disable_waits;
	ins = thread_swapins;
	woken = w->woken;
	sd_go = 1;
	thread_swap_disable(w->act);
	for (int i = 0; !sd_done && i < 10 * hz; i++)
		mutex_pause();
	sd_window(THR_ACT_NULL);

	if (!sd_done) {
		printf("swap_disable: [3] WRONG — the second thread never woke the "
		       "waiter in ten seconds (#642)\n");
		return;
	}
	if (thread_swap_disable_waits == waits || sd_met != 1) {
		printf("swap_disable: [3] NOT ASKED — the wakeup came after the "
		       "swap-in was over, so it met no swap-in to race (#642)\n");
		return;
	}
	sd_verdict(3, w, "out", ins, woken);
}

void
swap_disable_test(void)
{
	task_t	task;

	printf("swap_disable: thread_swap_disable() on an activation the thread "
	       "swapper is moving: coming in, going out, out and woken (#642)\n");
	if (task_create_local(kernel_task, FALSE, FALSE, &task) != KERN_SUCCESS) {
		printf("swap_disable: WRONG — could not create a task for the "
		       "waiters (#642)\n");
		return;
	}
	sd_coming_in_arm(task);
	sd_going_out_arm(task);
	sd_out_woken_arm(task);
}
