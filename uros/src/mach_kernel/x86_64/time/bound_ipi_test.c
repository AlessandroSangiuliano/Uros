/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -h: a thread woken onto another processor's queue, better than what runs
 * there, does not wait for that processor's tick (#787, section 7).
 *
 * thread_setrun()'s branch for a thread bound to another processor sent that
 * processor the AST only when its local queue had been empty.  A bound thread
 * better than the running one, queued behind others, waited for the
 * processor's next tick to be looked at.
 *
 * Processor P, not the master, runs two spinners bound to it, so that one
 * runs and the other waits in P's queue.  X, bound to P and better than both,
 * waits on an event, and this thread, on the master, wakes it once X has
 * blocked.  X notes P's tick count when it runs: unchanged since the wakeup,
 * the AST brought it; one more, it waited for P's tick.
 *
 * And X notes how many ASTs P has taken, because a tick on the way does not
 * say no AST went: on a host that runs P's processor late, the AST arrives
 * after the tick.  Campaign 42287329719 said WRONG on a nested KVM where X met
 * the tick in 13 rounds of 20, from a branch that sends the AST in every one.
 * So a round where X met the tick with no AST taken on P is the defect, and
 * one where the AST came all the same is the host's; only the first can make
 * this test say WRONG.
 */

#include <kern/lock.h>			/* mutex_pause */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched.h>			/* BASEPRI_SYSTEM, MINPRI */
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/time_out.h>		/* timeout_ticks */

#include <cpu/ipi.h>			/* ipi_ast_counts */
#include <cpu/regs.h>			/* cpu_pause */
#include <time/clock_event.h>		/* clock_event_ticks */
#include <time/bound_ipi_test.h>

#define	BOUND_IPI_ROUNDS	20
#define	BOUND_IPI_WAIT		200	/* ticks: two seconds at 100 Hz */
#define	BOUND_IPI_LO_PRI	(BASEPRI_SYSTEM + 8)	/* below X */

static processor_t		bound_ipi_cpu_p;
static int			bound_ipi_cpu;
static int			bound_ipi_event;
static thread_t			bound_ipi_x;
static volatile int		bound_ipi_ready[3];	/* S, Q, X on P */
static volatile int		bound_ipi_ran;
static volatile unsigned long	bound_ipi_x_tick;
static volatile uint64_t	bound_ipi_x_asts;	/* P's, when X ran */
static volatile int		bound_ipi_stop;
static volatile int		bound_ipi_gone[3];

#define	BOUND_IPI_S	0
#define	BOUND_IPI_Q	1
#define	BOUND_IPI_X	2

static void
bound_ipi_to_p(void)
{
	thread_bind(current_thread(), bound_ipi_cpu_p);
	thread_block((void (*)(void)) 0);	/* runs next where it is bound */
}

static void
bound_ipi_spin(int me)
{
	bound_ipi_to_p();
	thread_set_own_priority(BOUND_IPI_LO_PRI);
	bound_ipi_ready[me] = 1;
	while (!bound_ipi_stop)
		cpu_pause();
	bound_ipi_gone[me] = 1;
	thread_terminate_self();
}

static void
bound_ipi_s(void)
{
	bound_ipi_spin(BOUND_IPI_S);
}

static void
bound_ipi_q(void)
{
	bound_ipi_spin(BOUND_IPI_Q);
}

static void
bound_ipi_x_main(void)
{
	uint64_t	asts, replays;

	bound_ipi_to_p();
	bound_ipi_x = current_thread();
	bound_ipi_ready[BOUND_IPI_X] = 1;
	for (;;) {
		assert_wait((event_t) &bound_ipi_event, FALSE);
		if (bound_ipi_stop) {
			clear_wait(current_thread(), THREAD_AWAKENED, FALSE);
			break;
		}
		thread_block((void (*)(void)) 0);
		bound_ipi_x_tick = clock_event_ticks(bound_ipi_cpu);
		ipi_ast_counts(bound_ipi_cpu, &asts, &replays);
		bound_ipi_x_asts = asts;
		bound_ipi_ran = 1;
	}
	bound_ipi_gone[BOUND_IPI_X] = 1;
	thread_terminate_self();
}

static boolean_t
bound_ipi_wait_flag(volatile int *v)
{
	natural_t	t0 = timeout_ticks;

	while (*v == 0) {
		if (timeout_ticks - t0 >= BOUND_IPI_WAIT)
			return FALSE;
		mutex_pause();
	}
	return TRUE;
}

/* X blocked: waiting, and off P. */
static boolean_t
bound_ipi_x_blocked(void)
{
	natural_t	t0 = timeout_ticks;

	while ((bound_ipi_x->state & (TH_WAIT | TH_RUN)) != TH_WAIT) {
		if (timeout_ticks - t0 >= BOUND_IPI_WAIT)
			return FALSE;
		cpu_pause();
	}
	return TRUE;
}

void
bound_ipi_test(void)
{
	thread_t	self = current_thread();
	unsigned int	round, fast = 0, late = 0, none = 0;
	unsigned long	c0;
	uint64_t	a0, replays;
	const char	*stopped = NULL;
	int		i, ncpu = 0;

	bound_ipi_cpu = -1;
	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p == PROCESSOR_NULL || p->state == PROCESSOR_OFF_LINE)
			continue;
		ncpu++;
		if (i != master_cpu)
			bound_ipi_cpu = i;
	}
	if (ncpu < 2 || bound_ipi_cpu < 0) {
		printf("bound_ipi_test: NOT ASKED — %d processor: the wakeup "
		       "has to come from another processor than the one the "
		       "thread is bound to (#787)\n", ncpu);
		return;
	}
	bound_ipi_cpu_p = cpu_to_processor(bound_ipi_cpu);
	thread_bind(self, master_processor);
	thread_block((void (*)(void)) 0);

	bound_ipi_stop = 0;
	bound_ipi_ran = 0;
	bound_ipi_x = THREAD_NULL;
	for (i = 0; i < 3; i++)
		bound_ipi_ready[i] = bound_ipi_gone[i] = 0;

	if (kernel_thread(kernel_task, bound_ipi_s, (char *) 0) == THREAD_NULL ||
	    kernel_thread(kernel_task, bound_ipi_q, (char *) 0) == THREAD_NULL ||
	    kernel_thread(kernel_task, bound_ipi_x_main, (char *) 0) ==
								THREAD_NULL ||
	    !bound_ipi_wait_flag(&bound_ipi_ready[BOUND_IPI_S]) ||
	    !bound_ipi_wait_flag(&bound_ipi_ready[BOUND_IPI_Q]) ||
	    !bound_ipi_wait_flag(&bound_ipi_ready[BOUND_IPI_X])) {
		bound_ipi_stop = 1;
		thread_wakeup((event_t) &bound_ipi_event);
		thread_bind(self, PROCESSOR_NULL);
		printf("bound_ipi_test: NOT ASKED — the spinners and X did not "
		       "all reach processor %d in %u ticks (#787)\n",
		       bound_ipi_cpu, BOUND_IPI_WAIT);
		return;
	}

	printf("bound_ipi_test: starting: %u rounds; processor %d runs two "
	       "spinners bound to it at %d, and X, bound to it at %d, is woken "
	       "from processor %d (#787)\n", BOUND_IPI_ROUNDS, bound_ipi_cpu,
	       BOUND_IPI_LO_PRI, BASEPRI_SYSTEM, master_cpu);

	for (round = 0; round < BOUND_IPI_ROUNDS; round++) {
		if (!bound_ipi_x_blocked()) {
			stopped = "X did not block";
			break;
		}
		bound_ipi_ran = 0;
		ipi_ast_counts(bound_ipi_cpu, &a0, &replays);
		c0 = clock_event_ticks(bound_ipi_cpu);
		thread_wakeup((event_t) &bound_ipi_event);
		if (!bound_ipi_wait_flag(&bound_ipi_ran)) {
			stopped = "X, woken, did not run";
			break;
		}
		if (bound_ipi_x_tick == c0)
			fast++;
		else if (bound_ipi_x_asts != a0)
			late++;		/* the AST came, after the tick */
		else
			none++;		/* no AST reached P: the defect */
		mutex_pause();		/* a different phase of P's tick */
	}

	bound_ipi_stop = 1;
	thread_wakeup((event_t) &bound_ipi_event);
	(void) bound_ipi_wait_flag(&bound_ipi_gone[BOUND_IPI_S]);
	(void) bound_ipi_wait_flag(&bound_ipi_gone[BOUND_IPI_Q]);
	(void) bound_ipi_wait_flag(&bound_ipi_gone[BOUND_IPI_X]);
	thread_bind(self, PROCESSOR_NULL);

	if (stopped != NULL)
		printf("bound_ipi_test: WRONG — %s within %u ticks, at round "
		       "%u of %u (#787)\n", stopped, BOUND_IPI_WAIT, round,
		       BOUND_IPI_ROUNDS);
	else if (none * 2 >= BOUND_IPI_ROUNDS)
		printf("bound_ipi_test: WRONG — X, better than what ran on "
		       "processor %d, waited for its tick in %u of %u rounds with "
		       "no AST taken there, and %u more met it with the AST late "
		       "(#787)\n", bound_ipi_cpu, none, BOUND_IPI_ROUNDS, late);
	else if ((none + late) * 2 >= BOUND_IPI_ROUNDS)
		printf("bound_ipi_test: NOT ASKED — X met processor %d's tick in "
		       "%u of %u rounds, and in %u of them the AST had come, after "
		       "the tick: this host runs that processor late, and whether "
		       "the AST would have been in time it cannot say (#787)\n",
		       bound_ipi_cpu, none + late, BOUND_IPI_ROUNDS, late);
	else
		printf("bound_ipi_test: PASS — X ran before processor %d's next "
		       "tick in %u of %u rounds; %u met a tick on the way with the "
		       "AST late, %u with no AST taken (#787)\n", bound_ipi_cpu,
		       fast, BOUND_IPI_ROUNDS, late, none);
}
