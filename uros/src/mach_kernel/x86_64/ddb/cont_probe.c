/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A thread blocked with a continuation, so that the debugger's refusal to
 * walk one is exercised rather than believed (#428).
 *
 * ── Why this has to be built ──────────────────────────────────────────
 *
 * The rule is that a thread which blocked with a continuation is reported by
 * its RESUME POINT and never by a backtrace, because the continuation is the
 * thread declaring it does not need its stack kept, and the switch saves
 * nothing for it (#771, #777): there is no frame to start a walk from.  What
 * its last call left on the stack stays mapped, which is what would make a
 * walk from anywhere else dangerous: well-formed frames that say nothing
 * about why the thread stopped.
 *
 * That rule was written and never once executed.  Three places in this tree
 * block with a continuation -- the IPC receive path, a VM test, and the VM
 * object reaper -- and on this target none of them runs: the first needs
 * userland (#422) and the others are not started.  Every thread in the
 * default set blocks the plain way, so the branch that refuses had no subject
 * and the debugger's most careful claim was the one nobody had checked.
 *
 * So this makes one, the same way #463's test constructs its interleaving
 * rather than waiting for it: a thread that asserts a wait, blocks with a
 * real continuation, and stays there.  The prompt then has something to
 * describe, and `l' must say "no stack, resumes at <cont_probe_resume>".
 *
 * ⚠️ The continuation is a real function and not a sentinel.  thread_block()
 * takes both through the same argument and tells them apart by address -- the
 * SAFE_* values sit in the last seven addresses of the space, where no
 * function can be (kern/thread.h).  A probe that passed a sentinel would
 * exercise the other path and prove the opposite of what it claims.
 */

#include <kern/lock.h>		/* #771: mutex_pause() */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_swap.h>
#include <kern/time_out.h>		/* #771: hz */
#include <mach/machine.h>

#include <cpu/regs.h>
#include <cpu/spl.h>
#include <ddb/cont_probe.h>
#include <ddb/ddb.h>
#include <time/tsc.h>

static int		cont_probe_event;
static volatile int	cont_probe_blocked;

/*
 * Where it would resume.  Never actually reached: nothing signals the event,
 * which is the point -- the thread has to still be there when the operator
 * looks.  Its ADDRESS is the whole of what this test is about, because that
 * is what the debugger has to report in place of a stack.
 */
static void
cont_probe_resume(void)
{
	for (;;)
		cpu_pause();
}

static void
cont_probe_body(void)
{
	spl_t s;

	s = splsched();
	assert_wait((event_t) &cont_probe_event, TRUE);
	splx(s);

	cont_probe_blocked = 1;

	thread_block(cont_probe_resume);

	/* Not reached: nobody wakes it. */
	for (;;)
		cpu_pause();
}

void
cont_probe_start(void)
{
	thread_t	th;
	thread_act_t	act;
	uint64_t	t0, second;
	spl_t		s;

	/*
	 * 🔑 NOT ASKED, not WRONG (#563).  An uncalibrated TSC is a property of
	 * the machine this booted on -- qemu offers no invariant counter, and
	 * tsc_calibrate() can find no agreeing median in four attempts -- so
	 * there is no ruler here, and a test with no ruler has not failed, it
	 * has not been run.  The sentence below already said as much while
	 * printing the word the harness greps for.
	 */
	second = tsc_hz();
	if (second == 0) {
		printf("cont_probe: NOT ASKED — no calibrated TSC on this "
		       "machine, so the wait cannot be bounded (#428, #318)\n");
		return;
	}

	if (thread_create_at(kernel_task, &th, cont_probe_body) != KERN_SUCCESS) {
		printf("cont_probe: WRONG — could not create the probe thread "
		       "(#428)\n");
		return;
	}

	act = th->top_act;
	thread_swappable(act, FALSE);

	s = splsched();
	thread_lock(th);
	th->max_priority = BASEPRI_SYSTEM;
	th->priority = BASEPRI_SYSTEM;
	th->sched_pri = BASEPRI_SYSTEM;
	th->state |= TH_RUN;
	thread_setrun(th, TRUE, TAIL_Q);
	thread_unlock(th);
	splx(s);

	act_deallocate(act);
	thread_resume(act);

	t0 = rdtsc();
	while (!cont_probe_blocked && rdtsc() - t0 < second * 10)
		cpu_pause();

	if (!cont_probe_blocked) {
		printf("cont_probe: WRONG — the probe never reached its block, "
		       "so there is nothing for the debugger to describe "
		       "(#428)\n");
		return;
	}

	/* Let it get past thread_block() and off the processor. */
	t0 = rdtsc();
	while (rdtsc() - t0 < second / 4)
		cpu_pause();

	printf("cont_probe: thread %p is blocked with a continuation at %p — "
	       "`l' must report it by its resume point and must NOT walk its "
	       "stack (#428)\n", th, cont_probe_resume);

	Debugger("a thread is blocked with a continuation");
}

/*
 * ── -l: a continuation that returns (#771) ──────────────────────────────
 *
 * The other thing a continuation can do wrong, and this one needs no
 * debugger to see.  thread_continue() calls a continuation and does not
 * expect it back -- since #771 it begins on a fresh frame, with nothing under
 * it but the trampoline -- so one that returns has nowhere to go, and the
 * kernel has to stop and say WHICH: falling off would end in a panic that
 * knows only that "a thread function" returned.
 *
 * So one is built: a thread blocks with a continuation that returns, and once
 * it is truly parked -- off its processor, which is when the switch resets
 * its stack -- it is woken into it.  The machine stops in
 * thread_begin_trampoline, naming cont_return_resume, and the merge test reads
 * that line (kind contret).  Woken before it parked, the thread would have
 * thread_block() come back instead, and nothing would be asked.
 */
static int	cont_return_event;
static int	cont_return_after;

static void
cont_return_resume(void)
{
	printf("cont_return: the continuation began, and returns (#771)\n");
}

static void
cont_return_body(void)
{
	spl_t	s;

	s = splsched();
	assert_wait((event_t) &cont_return_event, TRUE);
	splx(s);
	thread_block(cont_return_resume);

	/* Here only if the block came back instead of beginning in it. */
	printf("cont_return: WRONG — thread_block() came back instead of "
	       "beginning in the continuation: the switch threw it away "
	       "(#771)\n");
	for (;;)
		cpu_pause();
}

void
cont_return_start(void)
{
	thread_t	th;
	thread_act_t	act;
	int		parked = 0;
	spl_t		s;

	if (thread_create_at(kernel_task, &th, cont_return_body)
	    != KERN_SUCCESS) {
		printf("cont_return: WRONG — could not create the thread whose "
		       "continuation returns (#771)\n");
		return;
	}

	act = th->top_act;
	thread_swappable(act, FALSE);

	s = splsched();
	thread_lock(th);
	th->state |= TH_RUN;
	thread_setrun(th, TRUE, TAIL_Q);
	thread_unlock(th);
	splx(s);

	act_deallocate(act);
	thread_resume(act);

	/*
	 * Parked: TH_WAIT and nothing else, on its own event -- the state
	 * thread_dispatch() leaves once the thread is off its processor.  Asked
	 * once a tick for up to ten seconds, the processor given up between.
	 */
	for (int i = 0; i < 10 * hz && !parked; i++) {
		s = splsched();
		thread_lock(th);
		parked = (th->state & (TH_SCHED_STATE | TH_SWAPPED_OUT)) == TH_WAIT
			 && th->wait_event == (event_t) &cont_return_event;
		thread_unlock(th);
		splx(s);
		if (!parked)
			mutex_pause();
	}
	if (!parked) {
		printf("cont_return: WRONG — the thread never parked in ten "
		       "seconds, so there is no continuation to wake it into "
		       "(#771)\n");
		return;
	}

	printf("cont_return: thread %p is parked with a continuation that "
	       "returns, at %p; waking it, and the machine must stop naming it "
	       "(#771)\n", th, cont_return_resume);

	/*
	 * And give it the processor: a wait nobody ends, bounded at ten
	 * seconds.  The machine stops before it is over; a wait that ends was a
	 * continuation that returned and nothing that noticed.
	 */
	s = splsched();
	assert_wait((event_t) &cont_return_after, FALSE);
	thread_set_timeout(10 * hz);
	splx(s);
	thread_wakeup((event_t) &cont_return_event);
	thread_block((void (*)(void)) 0);

	printf("cont_return: WRONG — ten seconds after its thread was woken the "
	       "machine still runs: a continuation returned and nothing stopped "
	       "it (#771)\n");
}
