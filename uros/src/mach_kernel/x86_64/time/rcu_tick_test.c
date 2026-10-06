/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -U: a read section held across clock ticks while a callback waits (#649).
 *
 * ── What can go wrong ─────────────────────────────────────────────────
 *
 * A grace period is over when every processor has been through a quiescent
 * state since it began, and the clock tick is what notices.  Each tick first
 * reports its own processor quiescent -- unless the code it interrupted is
 * inside a read section, where it reports nothing -- and then asks whether the
 * grace period in flight is over (urmach_rcu_advance()).  Read sections turn
 * preemption off and leave interrupts on, and the tick is class 15, above
 * every spl, so a tick does land inside them.
 *
 * A tick that asks about every processor but its own, landing in a section on
 * the one processor still holding the grace period up, finds nobody holding
 * it, ends it, and wakes the thread that runs the callbacks.  The callback --
 * a pmap structure freed under a lockless walker, a device claim slot recycled
 * under check_claim() -- then runs on another processor while the reader is
 * still in its section.
 *
 * ── How it is asked ───────────────────────────────────────────────────
 *
 * A reader bound to one processor enters a read section, and this thread,
 * which cannot run there while the section holds that processor, queues a
 * callback with urmach_call_rcu().  The reader stays in the section for
 * RCU_TICK_SECTION_MS, about ten of its processor's ticks, and looks whether
 * the callback has run before it leaves; the callback looks whether the
 * reader is still inside.  Then this thread waits for the callback, because
 * one that never runs is a grace period that never ends, which is as wrong as
 * one that ends early.
 *
 * Each round counts the ticks the reader's processor took between the call
 * and the reader's exit; clock_event_ticks() counts a tick after its
 * urmach_rcu_advance(), so each one asked.  A grace period begins at one tick
 * and is judged at a later one, so a round with fewer than RCU_TICK_MIN_TICKS
 * did not ask the question, and a run in which no round asked it is NOT
 * ASKED (#563).
 */

#include <stdint.h>

#include <kern/cpu_number.h>
#include <kern/lock.h>			/* mutex_pause */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/rcu.h>
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>

#include <cpu/regs.h>			/* cpu_pause */
#include <time/clock_event.h>		/* clock_event_ticks */
#include <time/rcu_tick_test.h>
#include <time/tsc.h>

#define	RCU_TICK_ROUNDS		8
#define	RCU_TICK_SECTION_MS	100	/* about ten ticks at 100 Hz */
#define	RCU_TICK_MIN_TICKS	3	/* one begins it, one judges it, one spare */
#define	RCU_TICK_WAIT_S		5	/* for each step the other side takes */

static struct urmach_rcu_head	rcu_tick_head[RCU_TICK_ROUNDS];

/* The handshake, by round number from 1; zero is "none yet". */
static volatile int		rcu_tick_cpu;		/* where the reader goes */
static volatile int		rcu_tick_bound;		/* it is there */
static volatile int		rcu_tick_round;		/* asked of the reader */
static volatile int		rcu_tick_entered;	/* it is in the section */
static volatile int		rcu_tick_queued;	/* the callback is queued */
static volatile int		rcu_tick_left;		/* it has left */
static volatile int		rcu_tick_ran;		/* the callback has run */
static volatile int		rcu_tick_inside;	/* in the section now */
static volatile int		rcu_tick_stop;

/* Per round, as found. */
static volatile unsigned long	rcu_tick_ticks[RCU_TICK_ROUNDS];
static volatile int		rcu_tick_seen[RCU_TICK_ROUNDS];
static volatile int		rcu_tick_found[RCU_TICK_ROUNDS];
static volatile int		rcu_tick_late[RCU_TICK_ROUNDS];

/*
 * Runs where the drain runs it, the drain thread or an idle loop -- never on
 * the reader's processor while the reader holds it.
 */
static void
rcu_tick_callback(struct urmach_rcu_head *h)
{
	int	k = (int) (h - rcu_tick_head);

	rcu_tick_found[k] = rcu_tick_inside;
	rcu_tick_ran = k + 1;
}

static void
rcu_tick_reader(void)
{
	uint64_t	hz = tsc_hz(), end;
	unsigned long	t0;
	int		k, cpu;

	thread_bind(current_thread(), cpu_to_processor(rcu_tick_cpu));
	thread_block((void (*)(void)) 0);	/* runs next where it is bound */
	rcu_tick_bound = 1;

	for (k = 1; k <= RCU_TICK_ROUNDS; k++) {
		while (rcu_tick_round < k && !rcu_tick_stop)
			mutex_pause();
		if (rcu_tick_stop)
			break;

		urmach_rcu_read_lock();
		cpu = cpu_number();
		rcu_tick_inside = 1;
		rcu_tick_entered = k;

		/*
		 * The call comes from another processor, and it is waited
		 * for here, inside: the section is open before the callback
		 * exists, which is the order a lockless reader and an unlink
		 * are in.  A call that does not come leaves the round with a
		 * tick count of zero, and so not asked.
		 */
		end = rdtsc() + hz * RCU_TICK_WAIT_S;
		while (rcu_tick_queued < k && rdtsc() < end)
			cpu_pause();
		rcu_tick_late[k - 1] = (rcu_tick_queued < k);

		t0 = clock_event_ticks(cpu);
		end = rdtsc() + hz * RCU_TICK_SECTION_MS / 1000;
		while (rdtsc() < end)
			cpu_pause();
		rcu_tick_ticks[k - 1] = clock_event_ticks(cpu) - t0;
		rcu_tick_seen[k - 1] = (rcu_tick_ran >= k);

		rcu_tick_inside = 0;
		urmach_rcu_read_unlock();
		rcu_tick_left = k;
	}

	thread_terminate_self();
}

/*
 * Wait until *v reaches want or `seconds' of TSC pass; says which.  A tick
 * asleep at a time, as spl_test.c's: a waiter that spins takes a processor
 * from what it is waiting for.
 */
static boolean_t
rcu_tick_wait(volatile int *v, int want, uint64_t hz, unsigned seconds)
{
	uint64_t	t0 = rdtsc();

	while (*v < want) {
		if (rdtsc() - t0 >= hz * seconds)
			return FALSE;
		mutex_pause();
	}
	return TRUE;
}

void
rcu_tick_reader_test(void)
{
	uint64_t	hz = tsc_hz();
	uint32_t	ncpu = 0;
	unsigned long	least = ~0UL, most = 0;
	int		i, k, r = -1, rounds = 0, asked = 0, early = 0;
	int		first_early = 0, never = 0;

	if (hz == 0) {
		printf("rcu_tick: NOT ASKED — no calibrated TSC to time the "
		       "section with (#563)\n");
		return;
	}

	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p == PROCESSOR_NULL || p->state == PROCESSOR_OFF_LINE)
			continue;
		ncpu++;
		if (i != master_cpu)
			r = i;
	}

	/*
	 * 🔑 NOT ASKED, and true: with one processor nothing else can run a
	 * callback while a reader holds it, and urmach_call_rcu() runs the
	 * callback at once when nobody else is running -- with this thread on
	 * the only processor, no reader is in a section anywhere.
	 */
	if (ncpu < 2 || r < 0) {
		printf("rcu_tick: NOT ASKED — %u processor: no callback can run "
		       "while a reader holds it, so a grace period cannot end "
		       "under one (#649)\n", (unsigned) ncpu);
		return;
	}

	rcu_tick_cpu = r;
	if (kernel_thread(kernel_task, rcu_tick_reader, (char *) 0)
	    == THREAD_NULL) {
		printf("rcu_tick: NOT ASKED — the reader thread could not be "
		       "created (#563)\n");
		return;
	}
	if (!rcu_tick_wait(&rcu_tick_bound, 1, hz, RCU_TICK_WAIT_S)) {
		printf("rcu_tick: NOT ASKED — the reader did not reach processor "
		       "%d in %u s (#563)\n", r, RCU_TICK_WAIT_S);
		rcu_tick_stop = 1;
		return;
	}

	printf("rcu_tick: starting: a reader bound to processor %d holds a "
	       "read section for %u ms while a callback is queued from "
	       "another processor, %u rounds (#649)\n", r,
	       RCU_TICK_SECTION_MS, RCU_TICK_ROUNDS);

	for (k = 1; k <= RCU_TICK_ROUNDS; k++) {
		rcu_tick_round = k;
		if (!rcu_tick_wait(&rcu_tick_entered, k, hz, RCU_TICK_WAIT_S)) {
			printf("rcu_tick: WRONG — the reader did not enter round "
			       "%d in %u s (#649)\n", k, RCU_TICK_WAIT_S);
			rcu_tick_stop = 1;
			return;
		}

		urmach_call_rcu(&rcu_tick_head[k - 1], rcu_tick_callback);
		rcu_tick_queued = k;

		if (!rcu_tick_wait(&rcu_tick_left, k, hz, 2 * RCU_TICK_WAIT_S)) {
			printf("rcu_tick: WRONG — the reader did not leave round "
			       "%d in %u s (#649)\n", k, 2 * RCU_TICK_WAIT_S);
			rcu_tick_stop = 1;
			return;
		}
		if (!rcu_tick_wait(&rcu_tick_ran, k, hz, RCU_TICK_WAIT_S)) {
			never = k;
			rcu_tick_stop = 1;
			break;
		}
		rounds++;
	}

	for (k = 0; k < rounds; k++) {
		if (rcu_tick_seen[k] || rcu_tick_found[k]) {
			if (early++ == 0)
				first_early = k + 1;
		}
		if (rcu_tick_late[k] || rcu_tick_ticks[k] < RCU_TICK_MIN_TICKS)
			continue;
		asked++;
		if (rcu_tick_ticks[k] < least)
			least = rcu_tick_ticks[k];
		if (rcu_tick_ticks[k] > most)
			most = rcu_tick_ticks[k];
	}

	printf("rcu_tick: %d rounds, %d of them with at least %u ticks of "
	       "processor %d between the call and the reader's exit (%lu to "
	       "%lu)\n", rounds, asked, RCU_TICK_MIN_TICKS, r,
	       asked ? least : 0UL, most);

	if (early != 0)
		printf("rcu_tick: WRONG — the callback ran while the reader was "
		       "still in its section in %d of %d rounds, the first in "
		       "round %d after %lu ticks of its processor: a grace "
		       "period ended under a reader (#649)\n", early, rounds,
		       first_early, rcu_tick_ticks[first_early - 1]);
	else if (never != 0)
		printf("rcu_tick: WRONG — round %d's callback did not run in "
		       "%u s after the reader left: a grace period that does "
		       "not end (#649)\n", never, RCU_TICK_WAIT_S);
	else if (asked == 0)
		printf("rcu_tick: NOT ASKED — no round had %u ticks of processor "
		       "%d inside the section after the call (#563)\n",
		       RCU_TICK_MIN_TICKS, r);
	else
		printf("rcu_tick: PASS — in every round the callback waited for "
		       "the reader to leave, %d of them across at least %u of "
		       "its processor's ticks (#649)\n", asked,
		       RCU_TICK_MIN_TICKS);
}
