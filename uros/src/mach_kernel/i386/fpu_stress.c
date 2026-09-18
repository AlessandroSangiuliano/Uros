/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * Does the switch carry a thread's floating-point state?  (#560)
 *
 * Three threads share one processor, each holding a different pattern in all
 * eight SSE registers and all eight x87 stack registers.  They write those
 * registers ONCE, then sleep and read them back, two hundred times each.  A
 * thread that reads back anything other than its own pattern says so.
 *
 * 🔴 THE TEST HAS TO BE ABLE TO FAIL, OR PASSING IT SAYS NOTHING -- and the
 * first two versions of this file could not.  Both wrote the registers and
 * span re-reading them without ever leaving the processor, exactly like the
 * x86-64 test they were modelled on, and both PASSED WITH THE RESTORE REMOVED
 * from fpu_load_context().  The three threads were running one after another
 * rather than interleaved, so every read happened inside the same turn as the
 * write that preceded it.  Sleeping between the two is what turned it into a
 * test: see the loop in fpu_stress_thread().
 *
 * With the sleep in and the restore taken out, all three fail, and the failure
 * is a signature rather than noise:
 *
 *	thread 0 held 0xa5a5a5a5deadbeef, read back differences
 *	0x999999991e5250ef -- and 0xa5a5a5a5deadbeef ^ 0x999999991e5250ef is
 *	0x3c3c3c3cc0ffee00, which is thread 2's pattern exactly.
 *
 * One thread reading another thread's registers is not a paraphrase of
 * CVE-2018-3665.  It is the thing itself, on demand.
 *
 * ⚠️ Not the same question as fpu_sanity_check() (#309).  That asks whether
 * this processor came up with CR4.OSFXSR programmed -- one CPU, no threads,
 * no switch.  This asks what happens to the registers between two threads,
 * and neither implies the other.
 *
 * 🔑 Three differences from the x86-64 test this is modelled on:
 *
 *  - It also loads the x87 stack.  On x86-64 kernel code is built
 *    -mgeneral-regs-only and the only floating-point state that matters is a
 *    user thread's; here _doprnt_ext formats floats with x87 instructions, in
 *    whatever thread called printf, so the x87 half is the half that would
 *    notice a switch dropping state.
 *
 *  - The threads share the processor the driver is on, rather than an
 *    application processor.  What the test needs is that they take the unit
 *    away from each other, which requires ONE processor -- and it has to work
 *    on a uniprocessor build, where the whole lazy scheme used to live in its
 *    most extreme form (the state was not even saved, only left behind).
 *
 *  - The driver SLEEPS rather than spinning.  On x86-64 it watches from
 *    another processor; here it is on the same one, and a driver that spun
 *    would be a driver the test threads never run against.
 */

#include <i386/fpu_stress.h>
#include <i386/fpu.h>
#include <kern/misc_protos.h>
#include <kern/thread.h>
#include <kern/thread_swap.h>
#include <kern/sched_prim.h>
#include <kern/processor.h>
#include <kern/thread_act.h>		/* act_deallocate */
#include <kern/task.h>
#include <kern/cpu_number.h>
#include <kern/spl.h>
#include <mach/machine.h>

extern void		  fpu_stress_load(unsigned long long pattern);
extern unsigned long long fpu_stress_check(unsigned long long pattern);
extern unsigned long long fpu_stress_unload(unsigned long long pattern);

#define	FPU_STRESS_THREADS	3

/*
 * Read-backs per thread, each one after a sleep of a clock tick.
 *
 * 🔴 The sleep is what makes this a test.  Two earlier versions did not have
 * it -- one span until a flag was raised, one span a fixed number of times --
 * and BOTH PASSED WITH THE RESTORE REMOVED from fpu_load_context(), because
 * the three threads ran one after another: each wrote its registers at the
 * start of its own turn and read them back inside it, so no read ever came
 * after a switch.  Sleeping between the write and the read makes the switch a
 * certainty rather than something hoped for from quantum expiry.
 */
#define	FPU_STRESS_TURNS	200

/*
 * One per thread, and none of them a round number: a pattern has to be
 * distinguishable from the other threads' AND from the ways a register can be
 * wrong without anybody having written to it -- all zeroes, all ones, and the
 * value left by whoever ran last.
 *
 * ⚠️ They also have to survive a trip through the x87 stack, which holds
 * 64-bit integers exactly (the extended format has a 64-bit mantissa) but
 * nothing wider.
 */
static const unsigned long long fpu_pattern[FPU_STRESS_THREADS] = {
	0xA5A5A5A5DEADBEEFULL,
	0x5A5A5A5AFEEDFACEULL,
	0x3C3C3C3CC0FFEE00ULL,
};

static volatile int		fpu_started;
static volatile int		fpu_finished;
static volatile unsigned long long fpu_damage[FPU_STRESS_THREADS];
static volatile int		fpu_slot[FPU_STRESS_THREADS];
static volatile int		fpu_slot_want = -1;

static void	fpu_stress_sleep(int ticks);

static void
fpu_stress_thread(void)
{
	int			me, i;
	unsigned long long	damage = 0;

	/*
	 * Which of the three this is.  Taken once, and atomically enough: the
	 * threads are created one at a time and each records its index before
	 * the next one is made runnable.
	 */
	me = fpu_started++;
	if (me < 0 || me >= FPU_STRESS_THREADS)
		panic("fpu_stress: more threads than patterns");

	fpu_slot[me] = current_processor()->slot_num;

	/*
	 * Written once.  Everything after this asks the scheduler what it
	 * kept -- a second write would repair the damage being measured.
	 */
	fpu_stress_load(fpu_pattern[me]);

	for (i = 0; i < FPU_STRESS_TURNS; i++) {
		/*
		 * 🔑 Sleep first, read second.  The sleep is a switch away and
		 * back, so the read that follows it is a read of what the
		 * switch gave back -- which is the only thing this file is
		 * asking about.  Read first and the answer would come from
		 * registers nothing had taken away yet, which is how the two
		 * earlier versions of this test passed with the restore
		 * removed.
		 *
		 * ⚠️ Nothing between the sleep and the read touches the unit:
		 * the kernel is built -mno-sse and this loop is integer code.
		 * The one place kernel code does use x87 is _doprnt_ext, and
		 * a printf on this path would be reported as damage -- which
		 * it would be.
		 */
		fpu_stress_sleep(1);
		damage |= fpu_stress_check(fpu_pattern[me]);
	}

	/* The x87 half, asked once, after all of those switches. */
	damage |= fpu_stress_unload(fpu_pattern[me]);

	fpu_damage[me] = damage;
	fpu_finished++;

	/*
	 * Done, and it gets off the processor for good.
	 *
	 * ⚠️ Not a spin.  The x86-64 twin of this file parks in a pause loop,
	 * which costs nothing there because it is watched from a different
	 * processor; here all three threads are bound to the one the driver
	 * is on, and a spinning thread would be a thread the other two never
	 * run against.  Waiting on an address nobody signals parks it off the
	 * run queue instead.
	 *
	 * Nothing reaps these.  The run is a boot-time acceptance check armed
	 * by a flag, and three sleeping threads are cheaper than a teardown
	 * path that would have to be got right to be trusted.
	 */
	for (;;) {
		assert_wait((event_t) &fpu_damage[me], FALSE);
		thread_block((void (*)(void)) 0);
	}
}

/*
 * Sleep for `ticks' clock interrupts, letting the threads under test have the
 * processor.  Waits on this function's own address, which nothing signals, so
 * only the timeout ever wakes it.
 */
static void
fpu_stress_sleep(int ticks)
{
	assert_wait((event_t) &fpu_stress_sleep, FALSE);
	thread_set_timeout(ticks);
	thread_block((void (*)(void)) 0);
}

void
fpu_stress_run(void)
{
	processor_t	target = current_processor();
	int		i, bad = 0;

	fpu_slot_want = target->slot_num;
	printf("fpu_stress: %d threads on processor %d, one floating-point "
	       "pattern each in 8 SSE and 8 x87 registers\n",
	       FPU_STRESS_THREADS, fpu_slot_want);

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
		 * Bound BEFORE it can run.  Made runnable first, any idle
		 * processor could take it, and three threads on three
		 * processors never contend for a unit -- which is the one
		 * thing this test needs them to do.
		 *
		 * On a uniprocessor build thread_bind_locked() is empty,
		 * correctly: there is nowhere else for it to go.
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
	 * Wait for all three, in tenth-of-a-second naps so the processor is
	 * theirs while they work.  Thirty seconds is far past what the count
	 * above takes even on an emulator; reaching it means they are not
	 * running, which the verdict then says rather than waiting forever.
	 */
	for (i = 0; i < 300 && fpu_finished < FPU_STRESS_THREADS; i++)
		fpu_stress_sleep(hz / 10);

	if (fpu_finished < FPU_STRESS_THREADS) {
		printf("fpu_stress: WRONG — only %d of %d threads reported "
		       "back within 30 seconds, so nothing is claimed about "
		       "the rest (#560)\n",
		       fpu_finished, FPU_STRESS_THREADS);
		return;
	}

	for (i = 0; i < FPU_STRESS_THREADS; i++) {
		if (fpu_slot[i] != fpu_slot_want) {
			printf("fpu_stress: WRONG — thread %d ran on processor "
			       "%d and was meant to share processor %d, so it "
			       "was never preempted against its peers (#560)\n",
			       i, fpu_slot[i], fpu_slot_want);
			bad++;
			continue;
		}
		if (fpu_damage[i] != 0) {
			printf("fpu_stress: WRONG — thread %d held pattern "
			       "0x%08x%08x and read back differences "
			       "0x%08x%08x: floating-point state did not "
			       "survive being taken off the processor (#560)\n",
			       i,
			       (unsigned int) (fpu_pattern[i] >> 32),
			       (unsigned int) fpu_pattern[i],
			       (unsigned int) (fpu_damage[i] >> 32),
			       (unsigned int) fpu_damage[i]);
			bad++;
		}
	}

	if (bad != 0) {
		printf("fpu_stress: %d of %d threads came back wrong\n",
		       bad, FPU_STRESS_THREADS);
		return;
	}

	printf("fpu_stress: PASS — %d threads shared processor %d, each "
	       "holding a different pattern in 8 SSE and 8 x87 registers "
	       "and reading it back after each of %d sleeps, and every one "
	       "of them got its own back\n",
	       FPU_STRESS_THREADS, fpu_slot_want, FPU_STRESS_TURNS);
}
