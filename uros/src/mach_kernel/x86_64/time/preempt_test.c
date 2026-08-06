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
#include <x86_64/time/tsc.h>
#include <x86_64/cpu/regs.h>
#include <kern/misc_protos.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/processor.h>
#include <kern/task.h>
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

static void
preempt_worker_a(void)
{
	/*
	 * One line each, on entry.  The question when nothing is printed is
	 * WHICH of the three threads ever started, and a counter cannot answer
	 * it -- a thread that never runs and a thread that runs without being
	 * observed produce the same silence.
	 */
	printf("preempt_test: worker a running\n");
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
	printf("preempt_test: worker b running\n");
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
	printf("preempt_test: reporter running\n");
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

	printf("preempt_test: PASS — the processor was taken from a running "
	       "thread and given to another, %u times a second\n",
	       clock_event_hz());

	/*
	 * End the run here.  This boot exists to answer one question and it
	 * has been answered; going on into bootstrap_create would end it in
	 * the #422 panic and bury the verdict under a backtrace.
	 */
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

	a = kernel_thread(kernel_task, preempt_worker_a, (void *) 0);
	b = kernel_thread(kernel_task, preempt_worker_b, (void *) 0);
	r = kernel_thread(kernel_task, preempt_reporter, (void *) 0);

	if (a == THREAD_NULL || b == THREAD_NULL || r == THREAD_NULL)
		panic("preempt_test: could not create the test threads");

	/*
	 * All three on this processor, so that two counters advancing can
	 * only mean the processor changed hands.  On four processors they
	 * would run side by side and prove nothing.
	 */
	thread_bind(a, here);
	thread_bind(b, here);
	thread_bind(r, here);

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
