/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What a return to ring 0 is allowed to take (#463).
 *
 * ── Why this is built rather than waited for ──────────────────────────
 *
 * The defect announced itself as a race: about one boot in five tripped
 * kern/lock.c's mutex_lock_assert_safe(), and others simply stopped.  Chasing
 * that rate was useless, and the measurement that showed why is worth keeping:
 * six builds, ten boots each, and the only two that faulted were the two where
 * a new object file was LINKED IN — one of them never executing a single
 * instruction of it.  The window is a handful of instructions wide, so the
 * addresses alone move it from never to often, and "it does not reproduce on
 * my build" means nothing at all.
 *
 * A test that can only fail by luck is not a test.  So this does not wait for
 * the interleaving; it constructs it.
 *
 * ── The state being constructed, and why it is the honest one ─────────
 *
 * thread_hold() does two things to a thread it wants to stop:
 * install_special_handler() puts a handler on the activation and act_set_apc()
 * raises AST_APC.  If the target is at that moment between assert_wait() and
 * thread_block() -- a window every blocking path in the kernel passes through
 * -- then it has an interruptible wait outstanding AND an APC pending, and it
 * is still running.
 *
 * That is the whole of the bug's precondition, and it is reached here in two
 * lines: assert_wait(), then AST_APC raised on the running activation.  Not a
 * simulation of the race -- the same fields, in the same state, with the same
 * thing about to happen to them.
 *
 * ── What happens next, on each kernel ─────────────────────────────────
 *
 * The thread then spins with interrupts on, so the timer tick certainly
 * arrives, and every trap return calls trap_take_ast().
 *
 *   Without the fix, that return takes AST_ALL.  ast_taken() reaches
 *   act_execute_returnhandlers() -- which it tries BEFORE anything else and
 *   returns straight after -- and that takes act_lock_thread(), a mutex, on a
 *   thread whose wait_event is set.  mutex_lock_assert_safe() says no, and the
 *   machine panics.  If assertions were off it would be worse: the mutex would
 *   block the thread and the wakeup it is holding a wait for would be lost.
 *
 *   With the fix, the same return takes AST_KERNEL_SAFE.  AST_APC is not in
 *   it, so ast_taken() computes `reasons = need_ast & mask' without it and
 *   leaves the bit pending for a return that can have it.  The thread blocks,
 *   is woken, and goes round again.
 *
 * ⚠️ So the control arm of this test is a PANIC, not a WRONG line.  That is
 * deliberate: the failure is the kernel stopping, and a test that caught it
 * politely would be testing something else.  The harness greps for the
 * assertion, which is how the unfixed kernel is scored.
 */

#include <kern/ast.h>
#include <kern/cpu_number.h>
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
#include <trap/ast_test.h>

/*
 * How many times the window is entered.  Each round is one arming and at
 * least one tick inside it, so the unfixed kernel has no way to reach the end
 * of the first one -- but more than one is worth having, because a fix that
 * merely moved the fault to the second iteration would otherwise read as a
 * pass.
 */
#define AST_PROBE_ROUNDS	20

static int		ast_probe_event;
static volatile int	ast_probe_armed;
static volatile int	ast_probe_rounds;
static volatile int	ast_probe_window_done;
static volatile int	ast_probe_slot = -1;

static void
ast_probe_body(void)
{
	uint64_t	window = tsc_hz() / 20;		/* ~50 ms: several ticks */

	ast_probe_slot = current_processor()->slot_num;

	for (;;) {
		spl_t		s;
		uint64_t	t0;

		/*
		 * ⚠️ Both halves under splsched, because that is what the AST
		 * macros in <kern/ast.h> assume and what assert_wait() runs at.
		 * Between them the thread is in the state thread_hold() would
		 * have left it in.
		 *
		 * ast_propagate() and not thread_ast_set() alone: the bit lives
		 * on the activation, and the trap return reads need_ast[] for
		 * this processor.  <kern/ast.h> says so at the definition --
		 * "if thread is the current thread, thread_ast_set should be
		 * followed by ast_propagate()" -- and without it this test
		 * would arm nothing and pass on a kernel that has the bug.
		 */
		s = splsched();
		assert_wait((event_t) &ast_probe_event, TRUE);
		thread_ast_set(current_act(), AST_APC);
		ast_propagate(current_act(), cpu_number());
		splx(s);

		ast_probe_armed++;

		/*
		 * Interrupts on, so the tick lands inside the window -- and
		 * NOBODY touching the event while it does.
		 *
		 * ⚠️ This is where the first version of this test was vacuous,
		 * and it passed on the unfixed kernel because of it.  The
		 * driver below used to call thread_wakeup() in a tight loop, so
		 * the wait was cleared within microseconds of being asserted
		 * and the AST arrived at a thread with nothing outstanding:
		 * act->ast=0, need_ast=0, wait_event=0 at the end of the window
		 * -- the APC had been taken and there had been nothing left for
		 * it to violate.  The handshake is what makes the window real.
		 */
		t0 = rdtsc();
		while (rdtsc() - t0 < window)
			cpu_pause();

		if (ast_probe_rounds == 0)
			printf("ast_test: the window held for %s: act->ast=0x%x "
			       "need_ast=0x%x wait_event=%p\n",
			       current_thread()->wait_event == (event_t) 0
			       ? "NOTHING — WRONG, the wait was cleared and this "
				 "test proves nothing (#463)" : "its whole length",
			       (unsigned) current_act()->ast,
			       (unsigned) need_ast[cpu_number()],
			       current_thread()->wait_event);

		ast_probe_window_done++;

		thread_block((void (*)(void)) 0);

		ast_probe_rounds++;
	}
}

void
kernel_ast_test(void)
{
	thread_t	th;
	thread_act_t	act;
	processor_t	target = PROCESSOR_NULL;
	uint64_t	t0, second;
	int		me = cpu_number();
	spl_t		s;

	second = tsc_hz();
	if (second == 0) {
		printf("ast_test: WRONG — no calibrated TSC, so the window "
		       "cannot be timed and nothing is claimed (#463)\n");
		return;
	}

	/*
	 * A processor that is not this one: the driver below waits on the
	 * probe, and a driver and a probe sharing one processor would be
	 * measuring the scheduler's fairness rather than the AST path.
	 */
	for (int i = 0; i < NCPUS; i++) {
		if (i == me || !machine_slot[i].is_cpu || !machine_slot[i].running)
			continue;
		target = cpu_to_processor(i);
		break;
	}

	if (target == PROCESSOR_NULL) {
		printf("ast_test: WRONG — no processor other than this one is "
		       "running; nothing was measured (#463)\n");
		return;
	}

	if (thread_create_at(kernel_task, &th, ast_probe_body) != KERN_SUCCESS) {
		printf("ast_test: WRONG — could not create the probe thread "
		       "(#463)\n");
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

	printf("ast_test: arming AST_APC inside the assert_wait window on "
	       "processor %d, %d times — an unfixed kernel cannot finish the "
	       "first (#463)\n", target->slot_num, AST_PROBE_ROUNDS);

	/*
	 * Wake it each time round.  The probe parks in thread_block() at the
	 * end of every round and nothing else will release it.
	 *
	 * Bounded, because the OTHER face of this defect is not a panic but a
	 * machine that stops: a thread parked by special_handler() on
	 * &suspend_count after the wakeup for it has gone by.  A test that
	 * waited for ever would report that one as silence, which is the
	 * failure mode this project keeps having to fix in its own tools.
	 */
	t0 = rdtsc();
	while (ast_probe_rounds < AST_PROBE_ROUNDS) {
		/*
		 * ⚠️ Only once the probe says its window is over.  Waking it
		 * while the window is open clears the very wait the window
		 * exists to hold, which is how the first version of this test
		 * passed on a kernel that has the defect.
		 */
		if (ast_probe_window_done > ast_probe_rounds)
			thread_wakeup((event_t) &ast_probe_event);

		if (rdtsc() - t0 > second * 30) {
			printf("ast_test: WRONG — %d of %d rounds after thirty "
			       "seconds, armed %d times: the probe is wedged, "
			       "which is this defect's quiet face (#463)\n",
			       ast_probe_rounds, AST_PROBE_ROUNDS,
			       ast_probe_armed);
			return;
		}
		cpu_pause();
	}

	if (ast_probe_slot != target->slot_num) {
		printf("ast_test: WRONG — the probe ran on processor %d and was "
		       "bound to %d, so it was not where the test put it "
		       "(#463)\n", ast_probe_slot, target->slot_num);
		return;
	}

	printf("ast_test: PASS — %d rounds with AST_APC pending across an "
	       "outstanding assert_wait, on processor %d: a kernel-mode trap "
	       "return deferred it instead of running the handler (#463)\n",
	       ast_probe_rounds, ast_probe_slot);
}
