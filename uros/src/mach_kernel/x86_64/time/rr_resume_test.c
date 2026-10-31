/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -k: a round-robin thread preempted by a wakeup resumes before its peer
 * (#787, section 6).
 *
 * POSIX wants a preempted FIFO or RR thread at the head of its priority's
 * list, with what was left of its quantum.  thread_setrun() raised
 * AST_QUANTUM on the processor a wakeup preempted, and thread_dispatch() put
 * the preempted thread at the tail and gave it a new quantum, as for a
 * quantum run out: its peer of the same priority ran instead.
 *
 * Two RR threads, A then B, share one priority and a long quantum; A runs and
 * B waits behind it.  H, better than both, sleeps on a three-tick timer, and
 * this thread, better than both too, wakes every tick.  Each of those wakeups
 * is made by softclock(), whose own wakeup, from the tick, preempts whichever
 * of A and B is running.  A and B note every time one of them takes over
 * from the other, and H notes which of the two was running when it woke.
 * The thread that ran first after H must be that one.
 *
 * One processor only: with more, softclock() is woken onto an idle one, and a
 * preemption from another processor comes through ast_check(), which already
 * tells a first quantum from a spent one.
 */

#include <kern/ipc_sched.h>		/* thread_will_wait_with_timeout */
#include <kern/lock.h>			/* mutex_pause */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched.h>			/* BASEPRI_SYSTEM */
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_act.h>
#include <kern/time_out.h>		/* timeout_ticks */

#include <mach/policy.h>
#include <mach/mach_host_server.h>	/* thread_set_policy */

#include <cpu/regs.h>			/* cpu_pause */
#include <time/rr_resume_test.h>

#define	RR_RESUME_WAIT		200	/* ticks: two seconds at 100 Hz */
#define	RR_RESUME_QUANTUM_MS	2000	/* no quantum runs out in the test */
#define	RR_RESUME_H_MS		30	/* three ticks */

#define	RR_A	1
#define	RR_B	2
#define	RR_H	3

static volatile int	rr_last;		/* who ran last: A, B or H */
static volatile int	rr_takeovers;		/* A and B taking over */
static volatile int	rr_before_h;		/* running when H woke */
static volatile int	rr_after_h;		/* first of A, B after H */
static volatile int	rr_h_ran;
static volatile int	rr_stop;
static volatile int	rr_gone[4];		/* A, B, H ended */
static volatile int	rr_parked[3];		/* A, B waiting to go */
static int		rr_pri;			/* A's and B's */
static int		rr_go_event[3];		/* A's and B's */

static void
rr_resume_spin(int me)
{
	assert_wait((event_t) &rr_go_event[me], FALSE);
	rr_parked[me] = 1;
	thread_block((void (*)(void)) 0);	/* until its policy is set */

	while (!rr_stop) {
		if (rr_last != me) {
			if (rr_h_ran && rr_after_h == 0)
				rr_after_h = me;
			if (rr_last == RR_A || rr_last == RR_B)
				rr_takeovers++;
			rr_last = me;
		}
		cpu_pause();
	}
	rr_gone[me] = 1;
	thread_terminate_self();
}

static void
rr_resume_a(void)
{
	rr_resume_spin(RR_A);
}

static void
rr_resume_b(void)
{
	rr_resume_spin(RR_B);
}

static void
rr_resume_h(void)
{
	thread_t	self = current_thread();

	thread_will_wait_with_timeout(self, RR_RESUME_H_MS);
	thread_block((void (*)(void)) 0);

	rr_before_h = rr_last;
	rr_last = RR_H;
	rr_h_ran = 1;
	rr_gone[RR_H] = 1;
	thread_terminate_self();
}

static kern_return_t
rr_resume_make_rr(thread_t th)
{
	policy_rr_base_data_t	base;
	policy_rr_limit_data_t	limit;

	base.base_priority = rr_pri;
	base.quantum = RR_RESUME_QUANTUM_MS;
	limit.max_priority = rr_pri;
	return thread_set_policy(th->top_act, th->processor_set, POLICY_RR,
				 (policy_base_t) &base, POLICY_RR_BASE_COUNT,
				 (policy_limit_t) &limit, POLICY_RR_LIMIT_COUNT);
}

static const char *
rr_resume_name(int who)
{
	switch (who) {
	case RR_A:	return "A";
	case RR_B:	return "B";
	case RR_H:	return "H";
	default:	return "nobody";
	}
}

void
rr_resume_test(void)
{
	thread_t	a, b, h;
	kern_return_t	kra, krb;
	natural_t	t0;
	int		i, ncpu = 0;

	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p != PROCESSOR_NULL && p->state != PROCESSOR_OFF_LINE)
			ncpu++;
	}
	if (ncpu != 1) {
		printf("rr_resume_test: NOT ASKED — %d processors: softclock() "
		       "is woken onto an idle one, and a preemption from "
		       "another processor comes through ast_check(), which "
		       "tells a first quantum from a spent one (#787)\n", ncpu);
		return;
	}

	rr_last = 0;
	rr_takeovers = 0;
	rr_before_h = 0;
	rr_after_h = 0;
	rr_h_ran = 0;
	rr_stop = 0;
	for (i = 0; i < 4; i++)
		rr_gone[i] = 0;
	rr_parked[RR_A] = rr_parked[RR_B] = 0;
	/* Below this thread, so that it runs to stop them; H, a kernel
	   thread at BASEPRI_SYSTEM, is above them either way. */
	rr_pri = current_thread()->priority + 4;
	if (rr_pri <= BASEPRI_SYSTEM || rr_pri >= MINPRI) {
		printf("rr_resume_test: NOT ASKED — this thread is at %d, and "
		       "A and B need a priority below it and below H's %d "
		       "(#787)\n", current_thread()->priority, BASEPRI_SYSTEM);
		return;
	}

	a = kernel_thread(kernel_task, rr_resume_a, (char *) 0);
	b = kernel_thread(kernel_task, rr_resume_b, (char *) 0);
	if (a == THREAD_NULL || b == THREAD_NULL) {
		printf("rr_resume_test: NOT ASKED — no RR thread (#787)\n");
		return;
	}
	t0 = timeout_ticks;
	while (!(rr_parked[RR_A] && rr_parked[RR_B]) &&
	       timeout_ticks - t0 < RR_RESUME_WAIT)
		mutex_pause();
	if (!(rr_parked[RR_A] && rr_parked[RR_B])) {
		printf("rr_resume_test: WRONG — A and B did not park in %u "
		       "ticks (#787)\n", RR_RESUME_WAIT);
		return;
	}
	kra = rr_resume_make_rr(a);
	krb = rr_resume_make_rr(b);
	if (kra != KERN_SUCCESS || krb != KERN_SUCCESS) {
		printf("rr_resume_test: NOT ASKED — A and B could not be made "
		       "RR: 0x%x, 0x%x (#787)\n", kra, krb);
		rr_stop = 1;
		thread_wakeup((event_t) &rr_go_event[RR_A]);
		thread_wakeup((event_t) &rr_go_event[RR_B]);
		return;
	}

	printf("rr_resume_test: starting: A then B, RR at %d with a %u ms "
	       "quantum, and H at %d on a %u ms timer, on one processor "
	       "(#787)\n", rr_pri, RR_RESUME_QUANTUM_MS,
	       BASEPRI_SYSTEM, RR_RESUME_H_MS);

	/* A first, then B: A runs and B waits behind it at one priority. */
	thread_wakeup((event_t) &rr_go_event[RR_A]);
	thread_wakeup((event_t) &rr_go_event[RR_B]);
	h = kernel_thread(kernel_task, rr_resume_h, (char *) 0);
	if (h == THREAD_NULL) {
		rr_stop = 1;
		printf("rr_resume_test: NOT ASKED — no H (#787)\n");
		return;
	}

	t0 = timeout_ticks;
	while ((!rr_h_ran || rr_after_h == 0) &&
	       timeout_ticks - t0 < RR_RESUME_WAIT)
		mutex_pause();
	rr_stop = 1;

	t0 = timeout_ticks;
	while (!(rr_gone[RR_A] && rr_gone[RR_B] && rr_gone[RR_H]) &&
	       timeout_ticks - t0 < RR_RESUME_WAIT)
		mutex_pause();

	if (!rr_h_ran)
		printf("rr_resume_test: WRONG — H's %u ms timer did not wake "
		       "it in %u ticks (#787)\n", RR_RESUME_H_MS,
		       RR_RESUME_WAIT);
	else if (rr_after_h == 0)
		printf("rr_resume_test: WRONG — neither A nor B ran after H in "
		       "%u ticks (#787)\n", RR_RESUME_WAIT);
	else if (rr_before_h != rr_after_h)
		printf("rr_resume_test: WRONG — %s was running when H woke and "
		       "%s ran after it: the preempted thread went behind its "
		       "peer, as for a quantum run out; A and B took over from "
		       "each other %d times (#787)\n",
		       rr_resume_name(rr_before_h), rr_resume_name(rr_after_h),
		       rr_takeovers);
	else
		printf("rr_resume_test: PASS — %s was running when H woke and "
		       "%s ran after it, and A and B took over from each other "
		       "%d times, with a wakeup preempting the one running at "
		       "every tick (#787)\n", rr_resume_name(rr_before_h),
		       rr_resume_name(rr_after_h), rr_takeovers);
}
