/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Does vector state survive being taken off the processor? (#408)
 *
 * The switch saves and restores it -- x86_64/thread/context.c does the XSAVE
 * and the XRSTOR, and boot_c.c's fpu_probe() has checked that since #453.  But
 * that check calls context_switch() itself, on the boot processor, before there
 * is a scheduler: it asks whether the machinery works when a thread hands the
 * processor over deliberately.
 *
 * This asks the question the issue actually poses.  Several threads, each
 * holding a DIFFERENT pattern in all sixteen vector registers, sharing one
 * processor so that the only way any of them keeps running is by being taken
 * away and given back.  If the save area is per-thread and the switch uses the
 * right one, every thread sees its own pattern for the whole run.  If it is
 * shared, or if the restore reaches for the wrong area, each thread reads
 * somebody else's -- and the answer says which bits.
 *
 * ⚠️ DIFFERENT PATTERNS, not one shared value, and that is the whole design.
 * With every thread holding the same bits, a switch that restored the wrong
 * thread's area would produce exactly the right answer, and the test would
 * pass on a kernel with one save area for the machine.
 *
 * ⚠️ And the patterns have no zero and no all-ones half: a register left
 * untouched, one cleared to zero and one filled by XRSTOR from an area that
 * was never initialised are three different failures, and a pattern that
 * happens to look like any of them cannot tell them apart.
 */

#include <x86_64/thread/fpu_stress.h>
#include <x86_64/thread/fpu.h>
#include <x86_64/time/tsc.h>
#include <x86_64/cpu/regs.h>
#include <kern/misc_protos.h>
#include <kern/thread.h>
#include <kern/thread_swap.h>
#include <kern/sched_prim.h>
#include <kern/processor.h>
#include <kern/task.h>
#include <kern/cpu_number.h>
#include <mach/machine.h>
#include <mach/machine/vm_types.h>

extern uint64_t fpu_stress(uint64_t pattern, volatile int *stop);

#define	FPU_STRESS_THREADS	3

/*
 * One per thread, and none of them a round number.  See the header comment:
 * the patterns have to be distinguishable from each other AND from the three
 * ways a register can be wrong without anybody having written to it.
 */
static const uint64_t fpu_pattern[FPU_STRESS_THREADS] = {
	0xA5A5A5A5DEADBEEFULL,
	0x5A5A5A5AFEEDFACEULL,
	0x3C3C3C3CC0FFEE00ULL,
};

static volatile int	fpu_stop;
static volatile int	fpu_started;
static volatile int	fpu_finished;
static volatile uint64_t fpu_damage[FPU_STRESS_THREADS];
static volatile int	fpu_slot[FPU_STRESS_THREADS];
static volatile int	fpu_slot_want = -1;

static void
fpu_stress_thread(void)
{
	int		me;
	uint64_t	damage;

	/*
	 * Which of the three this is, taken once and atomically enough: the
	 * threads are created one at a time and each records its index before
	 * the next is made runnable.
	 */
	me = fpu_started++;
	if (me < 0 || me >= FPU_STRESS_THREADS)
		panic("fpu_stress: more threads than patterns");

	fpu_slot[me] = current_processor()->slot_num;

	damage = fpu_stress(fpu_pattern[me], &fpu_stop);

	fpu_damage[me] = damage;
	fpu_finished++;

	for (;;)
		cpu_pause();
}

void
fpu_stress_run(void)
{
	processor_t	target = PROCESSOR_NULL;
	uint64_t	t0, limit;
	int		me = cpu_number();
	int		i, bad = 0;

	/*
	 * An application processor, for the reason #461 gives: the boot
	 * processor is the one whose switch path has run since the beginning,
	 * and it is the one case that proves least.
	 */
	for (i = 0; i < NCPUS; i++) {
		if (i == me || i == master_cpu)
			continue;
		if (!machine_slot[i].is_cpu || !machine_slot[i].running)
			continue;
		target = cpu_to_processor(i);
		break;
	}

	if (target == PROCESSOR_NULL) {
		printf("fpu_stress: WRONG — no application processor other "
		       "than the boot processor is running; nothing was "
		       "measured (#408)\n");
		return;
	}

	fpu_slot_want = target->slot_num;
	printf("fpu_stress: %d threads on processor %d, one vector pattern "
	       "each, watched from processor %d (%s)\n",
	       FPU_STRESS_THREADS, fpu_slot_want, me, fpu_save_instruction());

	for (i = 0; i < FPU_STRESS_THREADS; i++) {
		thread_t	th;
		thread_act_t	act;
		spl_t		s;

		if (thread_create_at(kernel_task, &th, fpu_stress_thread)
		    != KERN_SUCCESS)
			panic("fpu_stress: could not create a test thread");

		thread_swappable(th->top_act, FALSE);

		s = splsched();
		thread_lock(th);
		act = th->top_act;
		th->max_priority = BASEPRI_SYSTEM;
		th->priority = BASEPRI_SYSTEM;
		th->sched_pri = BASEPRI_SYSTEM;
		/*
		 * Bound BEFORE it can run.  kernel_thread() would have made it
		 * runnable first and any idle processor could have taken it --
		 * which is how the #461 test came to measure three processors
		 * while claiming to measure one.
		 */
		thread_bind_locked(th, target);
		th->state |= TH_RUN;
		thread_setrun(th, TRUE, TAIL_Q);
		thread_unlock(th);
		splx(s);

		act_deallocate(act);
		thread_resume(act);
	}

	/*
	 * A second of vector state held across whatever the scheduler does,
	 * which at 100 Hz is about a hundred opportunities to lose it.
	 */
	t0 = rdtsc();
	limit = tsc_hz();
	if (limit == 0) {
		printf("fpu_stress: WRONG — no calibrated TSC, so the run "
		       "cannot be timed and nothing is claimed\n");
		return;
	}

	while (rdtsc() - t0 < limit)
		cpu_pause();

	fpu_stop = 1;

	/* Long enough for three threads sharing one processor to notice. */
	t0 = rdtsc();
	while (fpu_finished < FPU_STRESS_THREADS && rdtsc() - t0 < limit * 5)
		cpu_pause();

	if (fpu_finished < FPU_STRESS_THREADS) {
		printf("fpu_stress: WRONG — only %d of %d threads reported "
		       "back (#408)\n", fpu_finished, FPU_STRESS_THREADS);
		return;
	}

	for (i = 0; i < FPU_STRESS_THREADS; i++) {
		if (fpu_slot[i] != fpu_slot_want) {
			printf("fpu_stress: WRONG — thread %d ran on processor "
			       "%d and was meant to share processor %d, so it "
			       "was never preempted against its peers (#408)\n",
			       i, fpu_slot[i], fpu_slot_want);
			bad++;
			continue;
		}
		if (fpu_damage[i] != 0) {
			printf("fpu_stress: WRONG — thread %d held pattern "
			       "0x%016llx and read back differences 0x%016llx: "
			       "vector state did not survive being taken off "
			       "the processor (#408)\n", i,
			       (unsigned long long) fpu_pattern[i],
			       (unsigned long long) fpu_damage[i]);
			bad++;
		}
	}

	if (bad != 0) {
		printf("fpu_stress: %d of %d threads came back wrong\n",
		       bad, FPU_STRESS_THREADS);
		return;
	}

	printf("fpu_stress: PASS — %d threads shared processor %d for a "
	       "second, each holding a different pattern in all sixteen "
	       "vector registers, and every one of them read its own back "
	       "(%s)\n", FPU_STRESS_THREADS, fpu_slot_want, fpu_save_instruction());
}
