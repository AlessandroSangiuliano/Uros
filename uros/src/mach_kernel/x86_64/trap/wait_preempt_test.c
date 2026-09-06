/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A thread must not be put to sleep between declaring its wait and releasing
 * the lock it still holds (#490).
 *
 * ── The window, which is Mach's own idiom ─────────────────────────────
 *
 *	assert_wait(event, interruptible);
 *	mutex_unlock(lock);
 *	thread_block((void (*)(void)) 0);
 *
 * <kern/sched_prim.h> ships that as a macro, thread_sleep_mutex(), and 67
 * places in this kernel write it out by hand.  Between the first statement and
 * the second the thread is TH_WAIT and STILL OWNS THE LOCK.  On a machine that
 * preempts in kernel mode a preemption AST there finds a thread that has
 * already declared a wait, so thread_block_reason() puts it to sleep -- holding
 * the lock -- and every later taker of that lock waits for ever.
 *
 * 🔴 It was caught in the debugger before it was constructed here: two threads
 * on one activation, one asleep on its lock and the other suspended inside
 * special_handler(), which is that idiom in kern/thread_act.c.
 *
 * ── Why this is built rather than waited for ──────────────────────────
 *
 * The window is a handful of instructions wide, so whether a boot enters it is
 * a property of where the addresses landed rather than of the kernel being
 * right.  ast_test.c beside this one paid for that lesson under #463: six
 * builds, ten boots each, and the only two that faulted were the two where an
 * object file had been linked in.  A test that can only fail by luck is not a
 * test, so this does not wait for the interleaving -- it constructs it, with
 * the same fields in the same state and the same thing about to happen to them.
 *
 * ── What each kernel does ─────────────────────────────────────────────
 *
 * The probe takes a mutex, asserts a wait, arms AST_BLOCK on its own
 * processor, and then spins with interrupts on so that a trap return certainly
 * happens inside the window.
 *
 *   Without the fix, that return takes the preemption AST.  The thread is
 *   TH_WAIT, so thread_block_reason() sleeps it -- inside the spin, holding
 *   the mutex.  It never reaches its unlock, the window never closes, and the
 *   driver below finds the mutex still held.
 *
 *   With the fix, assert_wait() has raised the preemption level, and
 *   x86_64/trap/trap.c refuses every AST on a kernel-mode return while that
 *   level is non-zero.  The probe finishes its window, unlocks, and blocks
 *   where it meant to.
 *
 * ⚠️ THE FAILING ARM IS A WEDGE, and this reports it rather than joining it.
 * The driver is bounded and says which of the two shapes it saw -- the window
 * never closing, or the mutex never coming back -- because "the machine went
 * quiet" is the one outcome this project keeps having to fix in its own tools
 * rather than in the kernel.
 */

#include <kern/ast.h>
#include <kern/cpu_number.h>
#include <kern/lock.h>
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_act.h>
#include <kern/thread_swap.h>
#include <mach/machine.h>

#include <cpu/regs.h>
#include <cpu/spl.h>
#include <time/tsc.h>
#include <trap/wait_preempt_test.h>

/*
 * How many times the window is entered.
 *
 * More than one, for ast_test's reason: a fix that merely moved the failure to
 * the second round would otherwise read as a pass.
 */
#define WP_ROUNDS		20

/* How long the window is held open, as a fraction of a second. */
#define WP_WINDOW_DIVISOR	20		/* ~50 ms: several ticks */

static int		wp_event;
decl_mutex_data(static, wp_lock)

static volatile int	wp_armed;
static volatile int	wp_window_done;
static volatile int	wp_rounds;
static volatile int	wp_slot = -1;
static volatile int	wp_lost_the_window;

static void
wp_probe_body(void)
{
	uint64_t	window = tsc_hz() / WP_WINDOW_DIVISOR;

	wp_slot = current_processor()->slot_num;

	for (;;) {
		spl_t		s;
		uint64_t	t0;

		/*
		 * 🔑 THE LOCK IS TAKEN BEFORE THE WAIT IS DECLARED, which is
		 * the order every caller of this idiom uses and the order that
		 * makes the window dangerous.  Taking it afterwards would be a
		 * different program: a mutex acquired with a wait outstanding
		 * is what #463 forbids, and this test is not about that.
		 */
		_mutex_lock(&wp_lock);

		/*
		 * ⚠️ Both halves under splsched, because that is what the AST
		 * macros in <kern/ast.h> assume and what assert_wait() itself
		 * runs at.
		 *
		 * AST_BLOCK and not AST_APC: this is preemption.  The mask the
		 * kernel-mode return path tests is what decides whether the
		 * thread survives the window, and the two bits travel different
		 * routes through ast_taken().
		 */
		s = splsched();
		assert_wait((event_t) &wp_event, TRUE);
		ast_on(cpu_number(), AST_BLOCK);
		splx(s);

		wp_armed++;

		/*
		 * Interrupts on, so a tick certainly lands here, and NOBODY
		 * touching the event while it does.
		 *
		 * ⚠️ A driver that woke the probe during the window would clear
		 * the very wait the window exists to hold, and the test would
		 * pass on a kernel that has the defect.  ast_test.c was written
		 * that way once; the handshake below is what stops it.
		 */
		t0 = rdtsc();
		while (rdtsc() - t0 < window)
			cpu_pause();

		/*
		 * Did the window survive?  A wait cleared under us means the
		 * arms below are measuring nothing, and that has to be said
		 * rather than counted as a pass.
		 */
		if (current_thread()->wait_event != (event_t) &wp_event)
			wp_lost_the_window++;

		wp_window_done++;

		/* The idiom, completed: release, then block. */
		mutex_unlock(&wp_lock);
		thread_block((void (*)(void)) 0);

		wp_rounds++;
	}
}

void
kernel_wait_preempt_test(void)
{
	thread_t	th;
	thread_act_t	act;
	processor_t	target = PROCESSOR_NULL;
	uint64_t	t0, second;
	int		me = cpu_number();
	int		took_it = 0;
	spl_t		s;

	second = tsc_hz();
	if (second == 0) {
		printf("wait_preempt: WRONG — no calibrated TSC, so the window "
		       "cannot be timed and nothing is claimed (#490)\n");
		return;
	}

	mutex_init(&wp_lock, ETAP_MISC_MASTER);

	/*
	 * A processor that is not this one, for ast_test's reason: a driver and
	 * a probe sharing one processor would be measuring the scheduler's
	 * fairness rather than the window.
	 */
	for (int i = 0; i < NCPUS; i++) {
		if (i == me || !machine_slot[i].is_cpu || !machine_slot[i].running)
			continue;
		target = cpu_to_processor(i);
		break;
	}

	if (target == PROCESSOR_NULL) {
		printf("wait_preempt: WRONG — no processor other than this one "
		       "is running; nothing was measured (#490)\n");
		return;
	}

	if (thread_create_at(kernel_task, &th, wp_probe_body) != KERN_SUCCESS) {
		printf("wait_preempt: WRONG — could not create the probe thread "
		       "(#490)\n");
		return;
	}

	act = th->top_act;
	thread_swappable(act, FALSE);

	s = splsched();
	thread_lock(th);
	th->max_priority = BASEPRI_SYSTEM;
	th->priority = BASEPRI_SYSTEM;
	th->sched_pri = BASEPRI_SYSTEM;
	thread_bind_locked(th, target);
	th->state |= TH_RUN;
	thread_setrun(th, TRUE, TAIL_Q);
	thread_unlock(th);
	splx(s);

	act_deallocate(act);
	thread_resume(act);

	printf("wait_preempt: arming AST_BLOCK inside the assert_wait window on "
	       "processor %d while a mutex is held, %d times — an unfixed "
	       "kernel sleeps there and never unlocks (#490)\n",
	       target->slot_num, WP_ROUNDS);

	/*
	 * Bounded, because the failing arm is a WEDGE.  A test that waited for
	 * ever would report this defect as silence.
	 */
	t0 = rdtsc();
	while (wp_rounds < WP_ROUNDS) {
		/*
		 * ⚠️ Only once the probe says its window is over: waking it
		 * inside the window clears the wait the window exists to hold.
		 */
		if (wp_window_done > wp_rounds) {
			/*
			 * 🔑 THE POSITIVE HALF.  "The probe did not wedge" and
			 * "the lock came back" are two statements, and only the
			 * second one is about the thing that hurts: a thread
			 * asleep holding a lock is invisible until somebody
			 * else asks for it.  So somebody else asks.
			 */
			if (_mutex_try(&wp_lock)) {
				took_it++;
				mutex_unlock(&wp_lock);
			}
			thread_wakeup((event_t) &wp_event);
		}

		if (rdtsc() - t0 > second * 30) {
			printf("wait_preempt: WRONG — %d of %d rounds after "
			       "thirty seconds, armed %d, windows closed %d, "
			       "lock taken by this processor %d times: the "
			       "probe went to sleep inside the window and is "
			       "holding the mutex (#490)\n",
			       wp_rounds, WP_ROUNDS, wp_armed, wp_window_done,
			       took_it);
			return;
		}
		cpu_pause();
	}

	if (wp_lost_the_window > 0) {
		printf("wait_preempt: WRONG — the wait was cleared under the "
		       "probe %d times, so those rounds measured nothing "
		       "(#490)\n", wp_lost_the_window);
		return;
	}

	if (took_it == 0) {
		printf("wait_preempt: WRONG — %d rounds finished and this "
		       "processor never once got the mutex, so nothing was "
		       "proved about the lock (#490)\n", wp_rounds);
		return;
	}

	if (wp_slot != target->slot_num) {
		printf("wait_preempt: WRONG — the probe ran on processor %d and "
		       "was bound to %d, so it was not where the test put it "
		       "(#490)\n", wp_slot, target->slot_num);
		return;
	}

	printf("wait_preempt: PASS — %d rounds with a preemption AST pending "
	       "across an outstanding assert_wait, on processor %d, and this "
	       "processor took the mutex %d times between them: the window "
	       "held and the lock was always released (#490)\n",
	       wp_rounds, wp_slot, took_it);
}
