/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Two processors panic at the same instant (#599, -Z).
 *
 * Every panic on this target ends in halt_cpu() (cpu/model.c), and #599's
 * review rounds changed what it does when more than one processor gets there:
 * a processor that is not the panicking one waits for that one's message
 * before it stops the others, takes no interrupt after the wait, and the
 * console's final copy goes out inside the backtrace lock.  Each of those
 * answered a defect found by reading -- a message cut by the halt IPI, the
 * copy printed through the message or through the backtraces, a processor
 * re-entering halt_cpu() inside the backtrace lock's hold.  This boot asks the
 * machine, by making two processors panic within nanoseconds of each other
 * from thread context with interrupts on, which is the case in which the halt
 * IPI can land inside the panic message.
 *
 * 🔑 The answer is in the log, not here: this kernel does not survive the
 * question.  scripts/double-panic-check.sh reads it -- one whole panic message,
 * the console's final copy whole after it, one whole backtrace a processor, and
 * nothing else from the message on.  UROS_ABLATE_599_HALT_ORDER puts halt_cpu()
 * back in its old order, which is how the check is shown to be able to fail.
 */

#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_act.h>
#include <kern/thread_swap.h>
#include <mach/machine.h>

#include <cpu/halt_test.h>
#include <cpu/regs.h>
#include <cpu/spl.h>

/* The message both processors panic with; the check matches it whole. */
#define DP_MESSAGE	"double_panic: two processors panic at once, this one " \
			"is cpu %d (#599)"

static volatile int	dp_ready;
static volatile int	dp_go;

static void
dp_probe_body(void)
{
	dp_ready = 1;
	while (!dp_go)
		cpu_pause();
	panic(DP_MESSAGE, cpu_number());
}

void
double_panic_test(void)
{
	thread_t	th;
	thread_act_t	act;
	processor_t	target = PROCESSOR_NULL;
	int		me = cpu_number();
	uint64_t	spins;
	spl_t		s;

	/* A processor that is not this one, as wait_preempt_test chooses. */
	for (int i = 0; i < NCPUS; i++) {
		if (i == me || !machine_slot[i].is_cpu || !machine_slot[i].running)
			continue;
		target = cpu_to_processor(i);
		break;
	}
	if (target == PROCESSOR_NULL) {
		printf("double_panic: NOT ASKED — alone, no processor other than "
		       "this one is running (#599)\n");
		return;
	}

	/*
	 * 🔑 Interrupts on, or the question is not posed: the halt IPI can cut
	 * a panic message only where the panicking processor takes interrupts
	 * between its printfs.
	 */
	if (!interrupts_enabled()) {
		printf("double_panic: NOT ASKED — interrupts are off here, so no "
		       "halt IPI could reach the panicking processor (#599)\n");
		return;
	}

	if (thread_create_at(kernel_task, &th, dp_probe_body) != KERN_SUCCESS) {
		printf("double_panic: WRONG — could not create the second "
		       "panicker's thread (#599)\n");
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

	printf("double_panic: processors %d and %d panic at the same instant, "
	       "interrupts on; the log must keep one whole panic message, the "
	       "console's final copy whole after it, and one whole backtrace a "
	       "processor (#599)\n", me, target->slot_num);

	for (spins = 0; spins < CPU_SPIN_BUDGET && !dp_ready; spins++)
		cpu_pause();
	if (!dp_ready) {
		printf("double_panic: WRONG — the second panicker never ran on "
		       "processor %d (#599)\n", target->slot_num);
		return;
	}

	dp_go = 1;
	panic(DP_MESSAGE, me);
}
