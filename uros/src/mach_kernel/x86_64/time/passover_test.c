/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -g: a thread woken before it blocks is not passed over (#787, section 3).
 *
 * wake_dequeued() clears TH_WAIT on a thread that is still running and keeps
 * TH_UNINT, and thread_select() keeps the running thread only when its state
 * is exactly TH_RUN.  A thread woken between assert_wait() and thread_block()
 * can therefore give way to anything runnable, a lower priority included,
 * until a tick looks again.
 *
 * This thread, bound to one processor, declares an uninterruptible wait, is
 * woken from it by its own thread_wakeup() before it blocks, and blocks.  L,
 * bound to the same processor below it, spins and counts.  A round in which
 * L's count moved between the block and its return is a round in which the
 * better thread was passed over for the worse; the ticks it took are how long.
 */

#include <kern/lock.h>			/* mutex_pause */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched.h>			/* MINPRI */
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/time_out.h>		/* timeout_ticks */

#include <cpu/regs.h>			/* cpu_pause */
#include <time/passover_test.h>

#define	PASSOVER_ROUNDS		200
#define	PASSOVER_WAIT		200	/* ticks: two seconds at 100 Hz */

static processor_t		passover_cpu;
static int			passover_l_pri;
static int			passover_event;
static volatile unsigned long	passover_l_count;
static volatile int		passover_l_ready;
static volatile int		passover_stop;
static volatile int		passover_l_gone;

static void
passover_l(void)
{
	thread_bind(current_thread(), passover_cpu);
	thread_block((void (*)(void)) 0);	/* runs next where it is bound */
	thread_set_own_priority(passover_l_pri);
	passover_l_ready = 1;
	while (!passover_stop) {
		passover_l_count++;
		cpu_pause();
	}
	passover_l_gone = 1;
	thread_terminate_self();
}

void
passover_test(void)
{
	thread_t	self = current_thread();
	unsigned int	passed = 0, round;
	unsigned long	c0;
	natural_t	t0, waited, most = 0;
	int		i, me = -1;

	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p != PROCESSOR_NULL && p->state != PROCESSOR_OFF_LINE)
			me = i;		/* the last one: any is fine */
	}
	passover_cpu = cpu_to_processor(me);
	passover_l_pri = self->priority + 8;
	if (passover_l_pri >= MINPRI) {
		printf("passover_test: NOT ASKED — this thread is at %d, with "
		       "no room below it for L (#787)\n", self->priority);
		return;
	}
	thread_bind(self, passover_cpu);
	thread_block((void (*)(void)) 0);

	passover_l_count = 0;
	passover_l_ready = 0;
	passover_stop = 0;
	passover_l_gone = 0;
	if (kernel_thread(kernel_task, passover_l, (char *) 0) == THREAD_NULL) {
		printf("passover_test: NOT ASKED — no L (#787)\n");
		thread_bind(self, PROCESSOR_NULL);
		return;
	}
	t0 = timeout_ticks;
	while (!passover_l_ready && timeout_ticks - t0 < PASSOVER_WAIT)
		mutex_pause();
	if (!passover_l_ready) {
		passover_stop = 1;
		printf("passover_test: NOT ASKED — L did not start on processor "
		       "%d in %u ticks (#787)\n", me, PASSOVER_WAIT);
		thread_bind(self, PROCESSOR_NULL);
		return;
	}

	printf("passover_test: starting: %u rounds on processor %d, this "
	       "thread at %d woken before it blocks, L at %d spinning beside "
	       "it (#787)\n", PASSOVER_ROUNDS, me, self->priority,
	       passover_l_pri);

	for (round = 0; round < PASSOVER_ROUNDS; round++) {
		c0 = passover_l_count;
		t0 = timeout_ticks;
		assert_wait((event_t) &passover_event, FALSE);
		thread_wakeup((event_t) &passover_event);	/* before blocking */
		thread_block((void (*)(void)) 0);
		if (passover_l_count != c0) {
			passed++;
			waited = timeout_ticks - t0;
			if (waited > most)
				most = waited;
		}
	}

	passover_stop = 1;
	t0 = timeout_ticks;
	while (!passover_l_gone && timeout_ticks - t0 < PASSOVER_WAIT)
		mutex_pause();
	thread_bind(self, PROCESSOR_NULL);

	if (passed != 0)
		printf("passover_test: WRONG — woken before it blocked, this "
		       "thread was passed over for L, below it, in %u of %u "
		       "rounds, for up to %u ticks (#787)\n", passed,
		       PASSOVER_ROUNDS, most);
	else
		printf("passover_test: PASS — woken before it blocked, this "
		       "thread kept its processor in all %u rounds, L below it "
		       "runnable on the same processor (#787)\n",
		       PASSOVER_ROUNDS);
}
