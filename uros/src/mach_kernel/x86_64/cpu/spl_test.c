/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -E: a raise from level zero, preempted half-way (#526).
 *
 * ── What can go wrong ─────────────────────────────────────────────────
 *
 * The level is per processor, kept in the per-CPU block.  A raise finds the
 * block, reads the level in it and writes the new one.  At level zero a
 * thread can be preempted at any instruction -- trap_take_ast() takes
 * AST_PREEMPT on every interrupt return that finds SPL0 -- and a preempted
 * thread may resume on any processor.  If the block is found before
 * interrupts are turned off, a thread preempted in between finishes its raise
 * somewhere else, through a pointer to the block it left:
 *
 *   - the processor it LEFT is now at SPLHI, under whatever thread it is
 *     running, and nobody who raised it will ever lower it.  At SPLHI that
 *     processor defers its own tick; when it is the master, timeout_tick()
 *     stops and every timed wait in the machine sleeps for good.  That is
 *     how #526's boots stopped.
 *   - the processor it ARRIVED at stays at zero, so the thread runs its
 *     critical section believing it is at SPLHI when it is not.
 *
 * ── Why it is constructed rather than waited for ──────────────────────
 *
 * In the boots that found it, the window was a handful of instructions in
 * splx() and it opened about once in thirty boots.  A test that can only fail
 * by luck is not a test (ast_test.c says why at length).  So this puts
 * threads in the one place where the window is most of what they do: a loop
 * of raises from zero, with two threads for every processor so that a quantum
 * running out moves them, and checks the contract after each raise -- the
 * processor the thread is on now is at SPLHI.
 *
 * Two counts are faces of the defect:
 *
 *   wrong	a raise after which this processor was not at SPLHI: the
 *		write went into another block;
 *   inherited	a raise that found the level already above zero: another
 *		thread's raise landed HERE.  Every thread in this loop lowers
 *		back to zero, and a thread at zero is at zero wherever it
 *		resumes, so a level found raised was left by someone else.
 *
 * And one count says whether the question was asked at all:
 *
 *   moved	a raise during which the thread changed processor.  Without
 *		moves a split raise has no chance to show, and a clean run
 *		means nothing (#563).
 */

#include <stdint.h>

#include <kern/cpu_number.h>
#include <kern/lock.h>			/* mutex_pause */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched.h>			/* BASEPRI_USER */
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_swap.h>		/* thread_swappable */

#include <cpu/regs.h>			/* cpu_pause */
#include <cpu/spl.h>
#include <cpu/spl_test.h>
#include <sync/atomic.h>
#include <time/tsc.h>

#define	SPL_TEST_PER_CPU	2	/* threads a processor: enough to rotate */
#define	SPL_TEST_SECONDS	2

/*
 * Moves below this and a clean run is NOT ASKED.  Each move is a chance for
 * the window to be the place the thread was taken off, not a certainty, so a
 * handful of moves without a fault says little about a defect that strikes
 * only a share of them.
 */
#define	SPL_TEST_MIN_MOVED	20

static volatile int		spl_test_go;
static volatile uint32_t	spl_test_started;
static volatile uint32_t	spl_test_finished;
static volatile uint64_t	spl_test_deadline;

static volatile uint64_t	spl_test_raises;
static volatile uint64_t	spl_test_moved;
static volatile uint64_t	spl_test_wrong;
static volatile uint64_t	spl_test_inherited;

/* The first of each, as found, for the line that says what it was. */
static volatile uint32_t	spl_test_wrong_said;
static int			spl_test_wrong_from, spl_test_wrong_to;
static spl_t			spl_test_wrong_level;
static volatile uint32_t	spl_test_inherited_said;
static int			spl_test_inherited_on;
static spl_t			spl_test_inherited_level;

static void
spl_test_body(void)
{
	uint64_t	raises = 0, moved = 0, wrong = 0, inherited = 0;

	atomic_add32(&spl_test_started, 1);
	while (!spl_test_go)
		cpu_pause();

	/*
	 * ⚠️ Nothing in the loop but the raise and the check, and the clock
	 * read only every 64 rounds: time spent at zero outside splx() is time
	 * in which a preemption lands somewhere harmless, and the point is to
	 * make the window most of the loop.
	 */
	for (;;) {
		int	before = cpu_number();
		spl_t	s = splsched();
		spl_t	now = splget();
		int	after = cpu_number();

		splx(s);
		raises++;

		if (after != before)
			moved++;
		if (now != SPLHI) {
			wrong++;
			if (atomic_cmpxchg32(&spl_test_wrong_said, 0, 1) == 0) {
				spl_test_wrong_from = before;
				spl_test_wrong_to = after;
				spl_test_wrong_level = now;
			}
		}
		if (s != SPL0) {
			inherited++;
			if (atomic_cmpxchg32(&spl_test_inherited_said, 0, 1)
			    == 0) {
				spl_test_inherited_on = before;
				spl_test_inherited_level = s;
			}
		}

		if ((raises & 63) == 0 && rdtsc() >= spl_test_deadline)
			break;
	}

	atomic_add64(&spl_test_raises, raises);
	atomic_add64(&spl_test_moved, moved);
	atomic_add64(&spl_test_wrong, wrong);
	atomic_add64(&spl_test_inherited, inherited);
	atomic_add32(&spl_test_finished, 1);
	thread_terminate_self();
}

/*
 * rcu_bench.c's thread at a user thread's priority, and NOT bound: the whole
 * question is what happens to a thread that moves.
 */
static thread_t
spl_test_thread(void)
{
	thread_t	th;
	thread_act_t	act;
	spl_t		s;

	if (thread_create_at(kernel_task, &th, spl_test_body) != KERN_SUCCESS)
		return THREAD_NULL;

	thread_swappable(th->top_act, FALSE);

	s = splsched();
	thread_lock(th);

	act = th->top_act;
	th->max_priority = BASEPRI_USER;
	th->priority = BASEPRI_USER;
	th->sched_pri = BASEPRI_USER;

	th->state |= TH_RUN;
	thread_setrun(th, TRUE, TAIL_Q);
	thread_unlock(th);
	splx(s);

	act_deallocate(act);
	thread_resume(act);

	return th;
}

/*
 * Wait until *count reaches want or `seconds' of TSC pass; says which.
 *
 * ⚠️ A tick asleep at a time, as in rcu_bench.c: this thread outranks the
 * test threads and would take a processor from them if it spun.  If the
 * defect stops the master's tick, the sleep waits with it -- but only until
 * the thread holding that level up reaches its deadline and goes, which is a
 * bounded wait, not a hang.
 */
static boolean_t
spl_test_wait(volatile uint32_t *count, uint32_t want, uint64_t hz,
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
spl_raise_split_test(void)
{
	uint64_t	hz = tsc_hz(), t0, ms;
	uint32_t	ncpu = 0, want = 0, k;
	int		i;

	if (hz == 0) {
		printf("spl_test: NOT ASKED — no calibrated TSC to time the "
		       "run with (#563)\n");
		return;
	}

	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p != PROCESSOR_NULL && p->state != PROCESSOR_OFF_LINE)
			ncpu++;
	}

	/*
	 * 🔑 NOT ASKED, and true: on one processor a preempted thread can
	 * resume nowhere but where it was, so the block it found is still its
	 * own.  The window exists only where there is somewhere else to go.
	 */
	if (ncpu < 2) {
		printf("spl_test: NOT ASKED — %u processor: a preempted thread "
		       "resumes where it was, so a raise cannot be split "
		       "between two (#526)\n", (unsigned) ncpu);
		return;
	}

	spl_test_go = 0;
	for (k = 0; k < ncpu * SPL_TEST_PER_CPU; k++) {
		if (spl_test_thread() == THREAD_NULL)
			break;
		want++;
	}

	if (!spl_test_wait(&spl_test_started, want, hz, 5)
	    || want < ncpu * SPL_TEST_PER_CPU) {
		printf("spl_test: NOT ASKED — %u of %u threads started in 5 s "
		       "(#563)\n", (unsigned) spl_test_started,
		       (unsigned) (ncpu * SPL_TEST_PER_CPU));
		spl_test_deadline = 0;
		spl_test_go = 1;
		(void) spl_test_wait(&spl_test_finished, want, hz, 5);
		return;
	}

	t0 = rdtsc();
	spl_test_deadline = t0 + hz * SPL_TEST_SECONDS;
	spl_test_go = 1;

	if (!spl_test_wait(&spl_test_finished, want, hz,
			   SPL_TEST_SECONDS + 5)) {
		printf("spl_test: WRONG — %u of %u threads came back in %u s: "
		       "a processor stopped scheduling (#526)\n",
		       (unsigned) spl_test_finished, (unsigned) want,
		       SPL_TEST_SECONDS + 5);
		return;
	}
	ms = (rdtsc() - t0) * 1000 / hz;

	printf("spl_test: %u threads on %u processors for %u ms: %lu raises "
	       "from zero to SPLHI, %lu of them with the thread on another "
	       "processor by the time it was raised\n", (unsigned) want,
	       (unsigned) ncpu, (unsigned) ms,
	       (unsigned long) spl_test_raises,
	       (unsigned long) spl_test_moved);

	if (spl_test_wrong != 0 || spl_test_inherited != 0) {
		if (spl_test_wrong != 0)
			printf("spl_test:   %lu raises did not land on the "
			       "processor that made them; the first began on "
			       "processor %d and ended on processor %d, which "
			       "was at level %u\n",
			       (unsigned long) spl_test_wrong,
			       spl_test_wrong_from, spl_test_wrong_to,
			       (unsigned) spl_test_wrong_level);
		if (spl_test_inherited != 0)
			printf("spl_test:   %lu raises found the level already "
			       "raised; the first on processor %d, at level "
			       "%u\n", (unsigned long) spl_test_inherited,
			       spl_test_inherited_on,
			       (unsigned) spl_test_inherited_level);
		printf("spl_test: WRONG — a raise preempted half-way finished "
		       "in the block of the processor it had left (#526)\n");
	} else if (spl_test_moved < SPL_TEST_MIN_MOVED)
		printf("spl_test: NOT ASKED — %lu raises were split by a move "
		       "to another processor, fewer than %u: a raise split "
		       "half-way had too little chance to show (#563)\n",
		       (unsigned long) spl_test_moved, SPL_TEST_MIN_MOVED);
	else
		printf("spl_test: PASS — every raise landed on the processor "
		       "that made it, %lu of them across a move to another "
		       "processor (#526)\n", (unsigned long) spl_test_moved);
}
