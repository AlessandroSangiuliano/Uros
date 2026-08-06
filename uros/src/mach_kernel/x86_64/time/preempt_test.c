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
 * preempt_test.c — is a thread that never yields actually taken away? (#459)
 *
 * The clock exists and ticks at the right rate; that has been measured.  It
 * does not follow that anything is preempted: hertz_tick() decrementing a
 * quantum and raising an AST is one thing, the AST turning into a context
 * switch is another, and between them sit ast_check() and ast_taken() -- the
 * empty versions #453 wrote, which are correct for a kernel that never
 * preempts and wrong the moment it does.
 *
 * So this demonstrates it rather than asserting it, which is what #459 asks
 * for: two threads that never yield, a counter each, and a third that has to
 * get the processor back in order to report.
 *
 * WHY ALL THREE ARE BOUND TO ONE PROCESSOR.  On four processors the two
 * counters would both advance without anything ever being preempted -- they
 * would simply be running side by side, and the test would pass on a kernel
 * with no clock at all.  Bound to one, the only way both counters can move is
 * if the processor was taken from one and given to the other.
 *
 * AND WHY THE REPORTER IS ALSO A THREAD THAT NEVER SLEEPS.  If it blocked on
 * a timer it would prove that timeouts work, not that preemption does: the
 * scheduler would have handed the processor over voluntarily.  It spins on
 * the TSC, so the only way it ever prints is by being scheduled against two
 * peers that are not giving anything up.
 *
 * ⚠️ THE FAILURE IS SILENCE, AND THAT IS DELIBERATE.  Without preemption the
 * first thread scheduled runs forever and nothing is printed at all -- the
 * harness times out and the run fails.  A test that could only report success
 * would be no test; this one cannot report anything unless the property holds.
 */

#include <x86_64/time/clock_event.h>
#include <x86_64/time/preempt_test.h>
#include <x86_64/time/tsc.h>
#include <x86_64/cpu/regs.h>
#include <x86_64/cpu/spl.h>
#include <kern/misc_protos.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/processor.h>
#include <kern/task.h>
#include <kern/thread_swap.h>
#include <kern/cpu_number.h>
#include <mach/machine.h>		/* #461: machine_slot[] */
#include <mach/machine/vm_types.h>

/*
 * volatile because the whole point is that another thread reads them while
 * these loops run.  Without it the compiler is entitled to keep a counter in
 * a register for the life of the loop -- and the reporter would read zero
 * from a thread that had run billions of times, which reads exactly like a
 * thread that never ran.
 */
static volatile unsigned long	preempt_count_a;
static volatile unsigned long	preempt_count_b;
static volatile int		preempt_done;

/*
 * Whether the three are running on a processor other than this one, and
 * whether they have finished (#461).
 *
 * When they are, the reporter must NOT end the run: it is on an application
 * processor, and stopping that one would leave the boot processor waiting for
 * a verdict from a processor that has halted.  It raises `preempt_reported'
 * instead and the waiter ends the run, which also puts the machine's own
 * stop-everybody path under the one boot that has a reason to use it.
 */
static volatile int		preempt_remote;
static volatile int		preempt_reported;

/*
 * Where each of the three actually ran, and where they were asked to (#461).
 *
 * ⚠️ RECORDED AND CHECKED, because the binding failed silently once and the
 * result read as a pass.  The claim this test makes is "one processor, three
 * threads, none yielding", and it is only a claim about preemption while the
 * first three words are true: two counters advancing on two processors is two
 * threads running side by side, which a kernel with no clock at all would
 * produce.  A test whose precondition is a comment is a test that will one day
 * measure something else and say the same thing.
 */
static volatile int		preempt_slot_a = -1;
static volatile int		preempt_slot_b = -1;
static volatile int		preempt_slot_r = -1;
static volatile int		preempt_slot_want = -1;

/*
 * A kernel thread that is bound BEFORE it can run (#461).
 *
 * ⚠️ kernel_thread() followed by thread_bind() does not do this, and the
 * difference is the whole validity of the test.  kernel_thread() sets TH_RUN
 * and calls thread_setrun() itself, so by the time it returns the thread is on
 * a run queue and any idle processor may already have taken it; the bind that
 * follows arrives after the race it was meant to prevent.
 *
 * That is not a subtle failure and it was not caught by reading.  The first
 * four-processor run of this test asked for three threads on processor 1 and
 * printed its verdict from processor 3 -- and a verdict about "one processor,
 * three threads" produced by threads that were on three processors is not a
 * weaker claim, it is a different one.  Two counters advancing means the
 * processor changed hands only if there was one processor to change.
 *
 * (The historical code knew: kern/thread.c binds inside kernel_thread() under
 * PARAGON860, for exactly this reason.)
 *
 * Everything else here mirrors kernel_thread() deliberately, including the
 * order of act_deallocate() and thread_resume(): this is that function with
 * one call inserted, not a second way of making a kernel thread.
 */
static thread_t
preempt_thread_bound(void (*fn)(void), processor_t target)
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

	return th;
}

static void
preempt_worker_a(void)
{
	/*
	 * One line each, on entry.  The question when nothing is printed is
	 * WHICH of the three threads ever started, and a counter cannot answer
	 * it -- a thread that never runs and a thread that runs without being
	 * observed produce the same silence.
	 */
	/*
	 * The interrupt flag, reported because it is the property this thread
	 * depends on and the one that was wrong: a thread resumed on a fresh
	 * stack used to arrive here with IF clear, run forever, and take the
	 * clock down with it.
	 */
	{
		uint64_t fl;
		__asm__ __volatile__("pushfq; popq %0" : "=r"(fl));
		preempt_slot_a = current_processor()->slot_num;
		printf("preempt_test: worker a running on processor %d "
		       "(IF=%d)\n", preempt_slot_a, (int)((fl >> 9) & 1));
	}
	while (!preempt_done)
		preempt_count_a++;
	/* Nothing to tidy: the reporter ends the run, and a worker that
	 * outlives it would only be racing the halt. */
	for (;;)
		cpu_pause();
}

static void
preempt_worker_b(void)
{
	preempt_slot_b = current_processor()->slot_num;
	printf("preempt_test: worker b running on processor %d\n",
	       preempt_slot_b);
	while (!preempt_done)
		preempt_count_b++;
	for (;;)
		cpu_pause();
}

static void
preempt_reporter(void)
{
	uint64_t	t0, deadline;
	unsigned long	a, b;

	/*
	 * A second, measured against the TSC.  Long enough that a 100 Hz tick
	 * gives roughly a hundred opportunities to switch, so a single
	 * unlucky handover cannot be mistaken for the mechanism working.
	 */
	preempt_slot_r = current_processor()->slot_num;
	printf("preempt_test: reporter running on processor %d\n",
	       preempt_slot_r);
	t0 = rdtsc();
	deadline = tsc_hz() ? tsc_hz() : 0;
	if (deadline == 0) {
		printf("preempt_test: no calibrated TSC — the run cannot be "
		       "timed, so nothing is claimed\n");
		preempt_done = 1;
		return;
	}

	while (rdtsc() - t0 < deadline)
		cpu_pause();

	a = preempt_count_a;
	b = preempt_count_b;
	preempt_done = 1;

	printf("preempt_test: one processor, three threads, none yielding — "
	       "a=%lu b=%lu reporter ran\n", a, b);

	/*
	 * The verdict is about BOTH counters, not their sizes.  They will not
	 * be equal and are not expected to be: they start at different
	 * moments and the reporter steals time from both.  What matters is
	 * that neither is zero, because a zero means that thread never had
	 * the processor at all.
	 */
	if (a == 0 || b == 0)
		panic("preempt_test: a thread bound to this processor never "
		      "ran (a=%lu b=%lu) — the quantum is not being taken "
		      "away (#459)", a, b);

	/*
	 * And the precondition, checked last because it is checked against
	 * where the threads REALLY ran rather than where they were sent.
	 */
	if (preempt_slot_a != preempt_slot_want ||
	    preempt_slot_b != preempt_slot_want ||
	    preempt_slot_r != preempt_slot_want)
		panic("preempt_test: the three threads were meant to share "
		      "processor %d and ran on %d, %d and %d — they were not "
		      "confined, so two counters advancing says nothing about "
		      "preemption (#461)", preempt_slot_want,
		      preempt_slot_a, preempt_slot_b, preempt_slot_r);

	printf("preempt_test: PASS — processor %d was taken from a running "
	       "thread and given to another, %u times a second\n",
	       current_processor()->slot_num, clock_event_hz());

	/*
	 * End the run here -- but only when "here" is the boot processor.
	 *
	 * This boot exists to answer one question and it has been answered;
	 * going on into bootstrap_create would end it in the #422 panic and
	 * bury the verdict under a backtrace.  On an application processor the
	 * same halt would strand the boot processor waiting for a verdict from
	 * a processor that has stopped, so it hands the ending back (#461).
	 */
	if (preempt_remote) {
		preempt_reported = 1;
		for (;;)
			cpu_pause();
	}

	printf("preempt_test: halted — this boot was the test (#459)\n");
	for (;;)
		__asm__ __volatile__("cli; hlt");
}

/*
 * Start the three and hand the processor to the reporter.
 *
 * Called from load_context() under -P, in place of starting the first thread:
 * this is a boot of its own, like -D and -C, because a test that shares a
 * boot with setup_main() would be racing the panic at bootstrap_create (#422)
 * rather than measuring the scheduler.
 */
thread_t
preempt_test_run(void)
{
	thread_t	a, b, r;
	processor_t	here = current_processor();

	printf("preempt_test: starting — three threads bound to processor %d, "
	       "clock %s at %u Hz\n",
	       here->slot_num, clock_event_name(), clock_event_hz());

	/*
	 * All three on this processor, so that two counters advancing can
	 * only mean the processor changed hands.  On four processors they
	 * would run side by side and prove nothing -- which is why they are
	 * bound before they are runnable and not after.
	 */
	preempt_slot_want = here->slot_num;

	a = preempt_thread_bound(preempt_worker_a, here);
	b = preempt_thread_bound(preempt_worker_b, here);
	r = preempt_thread_bound(preempt_reporter, here);

	if (a == THREAD_NULL || b == THREAD_NULL || r == THREAD_NULL)
		panic("preempt_test: could not create the test threads");

	/*
	 * The reporter is handed back to load_context() to be started IN PLACE
	 * OF the first thread, and taken off the run queue first so it is not
	 * both dispatched and switched to -- one thread running on two paths
	 * at once.
	 *
	 * In place of, and not alongside, because the first thread goes
	 * straight to bootstrap_create() and panics there within a few
	 * milliseconds (#422).  A test needing a second of wall clock cannot
	 * win that race, and the two workers would be measured over a window
	 * that ends before it starts.
	 */
	{
		spl_t s = splsched();
		thread_lock(r);
		(void) rem_runq(r);
		thread_unlock(r);
		splx(s);
	}
	return r;
}

/*
 * The same three threads, on an application processor, watched from here
 * (#461).
 *
 * WHY THIS IS THE STRONGER TEST.  The uniprocessor form above shows that a
 * clock takes a processor away from a thread that will not give it up.  It
 * shows it on the processor the firmware started, which is the one processor
 * whose clock, quantum accounting and AST delivery had already run.  Every one
 * of those is per-processor state, and an application processor reaches all of
 * it through code that -- until this issue -- had never executed there.
 *
 * ⚠️ And the boot processor deliberately does nothing but wait.  If it went on
 * into bootstrap_create() it would panic there (#422) and, since #461, that
 * panic now stops every other processor -- including the three under test,
 * mid-measurement.  Waiting is not idleness here; it is the test.
 */
void
preempt_test_run_remote(void)
{
	thread_t	a, b, r;
	processor_t	target = PROCESSOR_NULL;
	uint64_t	t0, limit;
	int		me = cpu_number();
	int		i;

	/*
	 * The first processor that is not this one.  By slot rather than by
	 * position, because the slots are sparse: firmware does not promise
	 * consecutive APIC identifiers and this kernel indexes by them.
	 */
	for (i = 0; i < NCPUS; i++) {
		if (i == me || !machine_slot[i].is_cpu || !machine_slot[i].running)
			continue;
		target = cpu_to_processor(i);
		break;
	}

	if (target == PROCESSOR_NULL) {
		printf("preempt_test: WRONG — asked for an application "
		       "processor and this machine is running on one "
		       "processor; nothing was measured (#461)\n");
		return;
	}

	printf("preempt_test: starting — three threads bound to processor %d, "
	       "watched from processor %d, clock %s at %u Hz\n",
	       target->slot_num, me, clock_event_name(), clock_event_hz());

	preempt_remote = 1;

	preempt_slot_want = target->slot_num;

	a = preempt_thread_bound(preempt_worker_a, target);
	b = preempt_thread_bound(preempt_worker_b, target);
	r = preempt_thread_bound(preempt_reporter, target);

	if (a == THREAD_NULL || b == THREAD_NULL || r == THREAD_NULL)
		panic("preempt_test: could not create the test threads");

	/*
	 * Nothing is taken off a run queue here, unlike the uniprocessor form.
	 * There the reporter had to displace the first thread because it was to
	 * run on THIS processor; here all three simply become runnable on a
	 * processor that is sitting in its idle thread, and the scheduler
	 * dispatching them to it is part of what is being tested.
	 */

	/*
	 * Ten seconds of this processor's TSC, against a test that needs one.
	 * Bounded because the failure this exists to catch is silence, and
	 * waiting forever for a verdict turns a failed test into a hung boot --
	 * which says strictly less.
	 */
	t0 = rdtsc();
	limit = tsc_hz() ? tsc_hz() * 10 : 0;

	while (!preempt_reported) {
		if (limit != 0 && rdtsc() - t0 > limit) {
			printf("preempt_test: WRONG — processor %d never "
			       "reported in ten seconds.  It reached the "
			       "scheduler and runs an idle thread, but work "
			       "bound to it either never arrives or never "
			       "yields (#461)\n", target->slot_num);
			break;
		}
		cpu_pause();
	}

	/*
	 * The run is over either way, and ending it through halt_all_cpus() is
	 * deliberate: it is the one path on this machine that stops every
	 * processor, it has never been called by anything, and this is the only
	 * boot with three processors running and a reason to stop them.
	 */
	printf("preempt_test: halting the machine — this boot was the test "
	       "(#461)\n");
	halt_all_cpus(FALSE);
	/*NOTREACHED*/
}
