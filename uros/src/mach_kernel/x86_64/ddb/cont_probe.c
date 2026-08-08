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
 * thread declaring it does not need its stack kept and
 * machine_kernel_stack_init() then resets that stack to the trampoline.  The
 * memory stays mapped and stays walkable, which is what makes it dangerous: a
 * walk comes back with two or three well-formed frames of
 * context_thread_start and thread_begin_trampoline, and says nothing about
 * why the thread stopped.
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

#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_swap.h>
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

	second = tsc_hz();
	if (second == 0) {
		printf("cont_probe: WRONG — no calibrated TSC, so the wait "
		       "cannot be bounded (#428)\n");
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
