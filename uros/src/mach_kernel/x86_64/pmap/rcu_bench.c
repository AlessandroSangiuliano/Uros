/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/*
 * -R: the RCU queue on a machine that never idles (#608).
 *
 * Every processor gets a spinner, bound to it, at a user thread's priority.
 * While a spinner can run, its processor's idle loop cannot -- and the idle
 * loop was the only thing that ran RCU callbacks.  Then this thread creates
 * and destroys address spaces, each pmap_destroy() queueing its struct
 * through urmach_call_rcu(), and samples the queue as it goes.
 *
 * It says how many were queued during the run, how many came back and who
 * handed them back -- the drain thread or the idle loop -- and the most that
 * waited at once.  It judges two things:
 *
 *   - the premise: the idle loop handed nothing back during the run.  If it
 *     did, a processor idled after all, and the question was not asked
 *     (#563).
 *   - the answer: what is still waiting at the end is under half of what was
 *     queued.  With the idle loop as the only drain, nothing comes back while
 *     the machine is busy, and everything queued is still waiting.
 *
 * ⚠️ The pmap zone is expandable, so a queue that never drains does not stop
 * the machine: it grows the zone by half each time it fills, for ever.  The
 * run stops at RCU_BENCH_LIMIT waiting, which is far past anything a working
 * drain leaves and far below anything that hurts.
 */

#include <stdint.h>
#include <kern/thread.h>
#include <kern/sched.h>			/* BASEPRI_USER */
#include <kern/sched_prim.h>
#include <kern/processor.h>
#include <kern/task.h>
#include <kern/thread_swap.h>		/* thread_swappable */
#include <kern/misc_protos.h>
#include <kern/lock.h>			/* mutex_pause */
#include <kern/rcu.h>
#include <sync/atomic.h>		/* atomic_add32 */
#include <vm/pmap.h>
#include <time/tsc.h>			/* rdtsc, tsc_hz */
#include <cpu/regs.h>			/* cpu_pause */
#include <pmap/pmap.h>

#define	RCU_BENCH_SECONDS	2	/* the busy run: two hundred ticks */
#define	RCU_BENCH_LIMIT		4096	/* waiting at once: the run stops */

static volatile int		rcu_bench_stop;
static volatile uint32_t	rcu_bench_spinning;
static volatile uint32_t	rcu_bench_gone;

/*
 * preempt_test.c's preempt_thread_bound() at a user thread's priority: the
 * spinners must stay below the drain thread, which runs at the timeout
 * thread's, or they would be measuring a drain they had starved.
 */
static thread_t
rcu_bench_thread_bound(void (*fn)(void), processor_t target)
{
	thread_t	th;
	thread_act_t	act;
	spl_t		s;

	if (thread_create_at(kernel_task, &th, fn) != KERN_SUCCESS)
		return THREAD_NULL;

	thread_swappable(th->top_act, FALSE);

	s = splsched();
	thread_lock(th);

	act = th->top_act;
	th->max_priority = BASEPRI_USER;
	th->priority = BASEPRI_USER;
	th->sched_pri = BASEPRI_USER;

	thread_bind_locked(th, target);

	th->state |= TH_RUN;
	thread_setrun(th, TRUE, TAIL_Q);
	thread_unlock(th);
	splx(s);

	act_deallocate(act);
	thread_resume(act);

	return th;
}

static void
rcu_bench_spinner(void)
{
	atomic_add32(&rcu_bench_spinning, 1);
	while (!rcu_bench_stop)
		cpu_pause();
	atomic_add32(&rcu_bench_gone, 1);
	thread_terminate_self();
}

/*
 * Wait until *count reaches want or `seconds' of TSC pass; says which.
 *
 * ⚠️ It sleeps a tick at a time, it does not spin or merely yield: this
 * thread outranks the spinners, and the one bound to its own processor runs
 * only while this one is asleep.  collect_bench.c learned that a waiter that
 * holds the processor eats the subject it is waiting for.
 */
static boolean_t
rcu_bench_wait(volatile uint32_t *count, uint32_t want, uint64_t hz,
	       unsigned seconds)
{
	uint64_t	t0 = rdtsc();

	while (*count < want) {
		if (rdtsc() - t0 >= hz * seconds)
			return FALSE;
		mutex_pause();
	}
	return TRUE;
}

void
rcu_busy_bench(void)
{
	uint64_t	hz = tsc_hz(), t0, ms;
	uint32_t	want = 0, made = 0, most = 0, waiting = 0;
	unsigned	q0, ri0, rt0, q1, r1, ri1, rt1, after;
	boolean_t	limit_hit = FALSE;
	int		i;

	if (hz == 0) {
		printf("rcu_bench: NOT ASKED — no calibrated TSC to time the "
		       "run with (#563)\n");
		return;
	}

	rcu_bench_stop = 0;
	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p == PROCESSOR_NULL || p->state == PROCESSOR_OFF_LINE)
			continue;
		if (rcu_bench_thread_bound(rcu_bench_spinner, p) == THREAD_NULL)
			continue;
		want++;
	}

	if (!rcu_bench_wait(&rcu_bench_spinning, want, hz, 5)) {
		printf("rcu_bench: NOT ASKED — %u of %u spinners started in "
		       "5 s, so not every processor was kept busy (#563)\n",
		       (unsigned) rcu_bench_spinning, (unsigned) want);
		rcu_bench_stop = 1;
		return;
	}

	/*
	 * At one processor urmach_call_rcu() runs the callback at once --
	 * nobody else is running, so there is no grace period to wait for --
	 * and nothing is ever queued.
	 */
	if (want < 2) {
		printf("rcu_bench: NOT ASKED — %u processor: urmach_call_rcu() "
		       "runs the callback at once and queues nothing (#563)\n",
		       (unsigned) want);
		rcu_bench_stop = 1;
		(void) rcu_bench_wait(&rcu_bench_gone, want, hz, 5);
		return;
	}

	q0 = urmach_rcu_queued;
	ri0 = urmach_rcu_retired_idle;
	rt0 = urmach_rcu_retired_thread;

	t0 = rdtsc();
	while (rdtsc() - t0 < hz * RCU_BENCH_SECONDS) {
		pmap_t	p = pmap_create(0);

		if (p == PMAP_NULL) {
			printf("rcu_bench: pmap_create failed after %u spaces\n",
			       (unsigned) made);
			break;
		}
		pmap_destroy(p);
		made++;

		waiting = urmach_rcu_queued - urmach_rcu_retired;
		if (waiting > most)
			most = waiting;
		if (waiting >= RCU_BENCH_LIMIT) {
			limit_hit = TRUE;
			break;
		}
	}
	ms = (rdtsc() - t0) * 1000 / hz;

	q1 = urmach_rcu_queued;
	r1 = urmach_rcu_retired;
	ri1 = urmach_rcu_retired_idle;
	rt1 = urmach_rcu_retired_thread;
	waiting = q1 - r1;

	rcu_bench_stop = 1;
	if (!rcu_bench_wait(&rcu_bench_gone, want, hz, 5))
		printf("rcu_bench: %u of %u spinners left in 5 s\n",
		       (unsigned) rcu_bench_gone, (unsigned) want);

	printf("rcu_bench: %u spaces created and destroyed in %u ms with "
	       "every one of %u processors busy\n", (unsigned) made,
	       (unsigned) ms, (unsigned) want);
	printf("rcu_bench:   queued %u, handed back %u by the drain thread "
	       "and %u by the idle loop; at most %u waiting at once, %u at "
	       "the end\n", q1 - q0, rt1 - rt0, ri1 - ri0,
	       (unsigned) most, (unsigned) waiting);

	if (ri1 != ri0)
		printf("rcu_bench: NOT ASKED — the idle loop handed back %u "
		       "during the run, so a processor idled and the drain "
		       "thread was not the only way out (#563)\n", ri1 - ri0);
	else if (limit_hit || 2 * waiting > q1 - q0)
		printf("rcu_bench: WRONG — with every processor busy the queue "
		       "did not drain: %u of %u still waiting%s (#608)\n",
		       (unsigned) waiting, q1 - q0,
		       limit_hit ? ", and the run stopped at the limit" : "");
	else
		printf("rcu_bench: PASS — with every processor busy, the drain "
		       "thread handed back %u of %u, and %u were waiting at the "
		       "end (#608)\n", rt1 - rt0, q1 - q0, (unsigned) waiting);

	/* And with the processors free to idle again, for the record. */
	t0 = rdtsc();
	while (urmach_rcu_queued != urmach_rcu_retired
	       && rdtsc() - t0 < hz)
		mutex_pause();
	after = urmach_rcu_queued - urmach_rcu_retired;
	printf("rcu_bench: with the spinners gone, %u still waiting %s\n",
	       after, after == 0 ? "-- the queue is empty"
				 : "a second later");
}
