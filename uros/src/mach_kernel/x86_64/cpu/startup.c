/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What kern/startup.c asks this machine to do, and the two odds and ends
 * that go with it (#453).
 *
 * These are the machine's half of the boot sequence: the machine-independent
 * side runs a fixed order and calls out at fixed points, and this is what
 * happens at those points.  Most of the work has already been done by the
 * time they are reached -- x86_64/boot/ brought the processor up long before
 * there was a kernel to have a startup path -- so several of them are the
 * honest answer "already done", written where somebody looking for the work
 * will find it.
 */

#include <stdint.h>

#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>	/* #428: -S parks the startup thread */	/* #461: real_ncpus */
#include <kern/startup.h>	/* #448: the interface, so it is checked */
#include <kern/clock.h>		/* #425: clock_config, the clock devices */
#include <mach/machine/vm_param.h>

#include <cpu/percpu.h>
#include <cpu/smp.h>
#include <cpu/spl.h>
#include <pmap/layout.h>
#include <pmap/pmap.h>
#include <thread/fpu.h>
#include <boot/bootarg.h>	/* #461: boot_flag */
#include <cpu/lapic.h>		/* #459: LAPIC_TIMER_VECTOR */
#include <cpu/regs.h>		/* #461: cpu_pause */
#include <time/clock_event.h>	/* #459: the scheduler clock */
#include <time/preempt_test.h>	/* #461: -P on an application processor */
#include <thread/fpu_stress.h>	/* #408: -F, vector state across preemption */
#include <thread/state_test.h>	/* #408: the thread state flavour dispatch */
#include <ddb/cont_probe.h>	/* #428: -L, a thread with a continuation */
#include <ddb/ddb.h>		/* #428: -B, Debugger() from ordinary context */
#include <trap/ast_test.h>	/* #463: -A, what a ring-0 return may take */
#include <pmap/pmap.h>		/* #455: -C, the pmap under concurrency */
#include <trap/trap.h>		/* trap_set_handler */

/*
 * Machine initialisation, called once the machine-independent kernel is far
 * enough along to want a machine.
 *
 * Nothing, because everything it would do has happened: descriptor tables,
 * the interrupt controllers, the timers, the per-CPU blocks and the paging
 * structures were all built by x86_64/boot/ before C had a kernel around it.
 * The order is not ours to change -- a processor that reaches C without an
 * IDT triple-faults on its first mistake, so those cannot wait for a call
 * from the scheduler's startup.
 *
 * ⚠️ Empty, not absent, and not a panic: kern/startup.c calls it on the path
 * that must work.
 */
void
machine_init(void)
{
	/*
	 * Choose the clock backend and put its handler in the IDT (#459).
	 *
	 * Here because kern/startup.c calls this after init_timers() and
	 * timeout_init() and before any thread runs: the wheel the tick feeds
	 * exists, and nothing can tick yet.
	 *
	 * ⚠️ Chosen here, ARMED elsewhere.  No processor's timer is started
	 * by this call -- load_context() does that, for each processor, at the
	 * moment it acquires the first thread there is to charge time to.
	 */
	trap_set_handler(LAPIC_TIMER_VECTOR, clock_event_tick);
	clock_event_init(LAPIC_TIMER_VECTOR);

	/*
	 * Configure the clock devices (#425).
	 *
	 * ⚠️ Here because kern/startup.c calls machine_init() before
	 * clock_init(), and clock_config() has to run first: it is what asks
	 * each device whether it is there, and clock_init() calls c_init only
	 * on the ones that said yes.  Every other machine calls it from its
	 * model_dep.c for the same reason; this one had no call at all, which
	 * is the second half of why clock_list[] was empty and nobody noticed.
	 */
	clock_config();
}

/*
 * The kernel is up: run what had to wait for it.
 *
 * Nothing yet, and the two things i386 does here are worth naming so that the
 * emptiness is a statement rather than a gap:
 *
 *   fpu_sanity_check()  confirms CR4.OSFXSR survived bring-up, because on
 *                       i386 an AP once reached this point without it.  This
 *                       machine does not lazy-switch the FPU at all -- state
 *                       is saved eagerly with XSAVE and CR0.TS is never armed
 *                       (#408) -- so the bit it checks is programmed on the
 *                       one path that programs anything, and there is no
 *                       second path for it to disagree with.
 *
 *   hwp_init_cpu()      enables hardware P-states.  Real work here too, and
 *                       wanted, but it is not bring-up: it changes what the
 *                       clock does under measurement, so it belongs with the
 *                       numbers rather than ahead of them (#358 is i386's).
 *
 * ⚠️ Not empty any more, and the one thing in it is a test rather than
 * bring-up: see below.
 */
void
machine_kernel_ready(void)
{
	/*
	 * The thread state flavour dispatch, checked here because this is the
	 * first point at which it can be (#408).
	 *
	 * It needs a task to hang a probe activation off and an allocator for
	 * a floating-point area, which is task_init() and vm_mem_init(), both
	 * of which kern/startup.c has run by now.  It needs nothing later — no
	 * scheduler, no second processor, no user space — so it runs on every
	 * boot rather than behind a flag.
	 *
	 * ⚠️ Before the first thread and before any port exists, which is the
	 * point: act_machine_get_state() and act_machine_set_state() are the
	 * only validation between a flavour number arriving from outside and a
	 * copy into a caller's buffer, and on this target neither had ever
	 * executed.
	 */
	thread_state_dispatch_test();

	/*
	 * -B: does the machine-independent kernel's way into the debugger
	 * actually work? (#428)
	 *
	 * Debugger() is the name every failed assert() and both panic() paths
	 * reach, and until this issue it answered with a panic saying there was
	 * no debugger.  Now it raises a breakpoint and the trap path opens the
	 * prompt -- and "DDB can be entered" is the one done-when of #428 that
	 * had never been demonstrated, on a target whose every boot log is a
	 * self-test run the debugger never opens.
	 *
	 * ⚠️ Called from ordinary kernel context on purpose, and not from a
	 * fault.  A fault already has a trap frame; this is the case that does
	 * NOT, and it is the case that decides whether the frame the debugger
	 * shows describes the caller or describes the debugger.
	 *
	 * ⚠️ And its return is part of the test.  Debugger() is a call, and
	 * kern/debug.c's panic() carries on after it on a double panic.  A
	 * debugger that could only be entered by never coming back would break
	 * the one caller that matters most.
	 */
	if (boot_flag('B')) {
		printf("ddb_test: calling Debugger() from ordinary kernel "
		       "context — the prompt should open (#428)\n");
		Debugger("boot flag -B");
		printf("ddb_test: Debugger() RETURNED — a call that comes back, "
		       "which is what panic() needs on a double panic (#428)\n");
	}
}

/*
 * This processor's software interrupt level.
 *
 * The machine-independent spelling of what <cpu/spl.h> calls splget().  Two
 * names for one thing is not worth an inline forwarder in a header the whole
 * tree includes, so it is a function here.
 */
spl_t
getspl(void)
{
	return splget();
}

/*
 * Set the clock back to its regular rate after the debugger or a
 * calibration has been using it.
 *
 * Nothing, because nothing on this machine changes the rate: the local APIC
 * timer is programmed once per processor at bring-up and the TSC is not
 * programmable at all.  i386 needs this because its 8254 is reprogrammed for
 * delay loops and calibration and has to be put back.
 */
void
rtclock_reset(void)
{
}

/*
 * Bring the other processors up.
 *
 * ⚠️ Already done, and unlike the version of this comment that stood here
 * until #461, that is now the whole truth.  x86_64/cpu/smp.c starts every
 * processor in one pipelined pass during boot, because the trampoline and the
 * funnel counter pacing it are shared: doing it one processor at a time would
 * mean building and tearing that machinery down repeatedly, and the
 * pipelining is what makes the bring-up fast.
 *
 * What the old comment left out was the other half — woken is not scheduling.
 * The processors reached long mode, took their tables and their controllers,
 * and then halted, because nothing called slave_main().  Four processors, and
 * a scheduler that owned one.  That half is machine_processors_ready() below,
 * and it cannot be done here: this call comes after bootstrap_create(), which
 * on this target does not return (#422).
 */
void
start_other_cpus(void)
{
}

/*
 * Let them into the scheduler (#461).
 *
 * Here, at the point where kern/startup.c guarantees every processor has an
 * idle thread, because the idle thread is exactly what an arriving processor
 * takes: slave_main() ends in cpu_launch_first_thread(THREAD_NULL), and the
 * THREAD_NULL means "use the one you were given".  Released one line earlier
 * and an AP finds THREAD_NULL where its first thread should be.
 */
void
machine_processors_ready(void)
{
	unsigned	want = (unsigned) real_ncpus;
	unsigned	got = smp_ap_release_to_scheduler();

	if (got >= want) {
		unsigned	spins;
		int		idle;

		printf("startup: %u processors in the scheduler\n", got);

		/*
		 * And what the scheduler thinks it has (#461).
		 *
		 * cpu_up() is where a processor joins the processor set; going
		 * IDLE is a second thing that happens a little later, when its
		 * idle thread reaches the point of having nothing to run.  The
		 * two are worth separating: a processor that is in the set but
		 * never idle is one the scheduler will never dispatch to, and
		 * on the console that is indistinguishable from a processor
		 * that arrived.
		 *
		 * Every processor but this one, because this one is running the
		 * thread that is asking.  Bounded, and the count is printed
		 * whether or not it is reached -- the number is the answer, and
		 * a wait that gave up silently would turn a wrong number into
		 * no number.
		 */
		for (spins = 0; spins < 200000000U; spins++) {
			idle = *(volatile int *) &default_pset.idle_count;
			if (idle >= (int) (got - 1))
				break;
			cpu_pause();
		}

		idle = *(volatile int *) &default_pset.idle_count;
		printf("startup: %d of %u processors idle in the scheduler%s\n",
		       idle, got,
		       idle >= (int) (got - 1) ? "" :
		       " — WRONG, one that never goes idle is one the "
		       "scheduler will never dispatch to (#461)");

		/*
		 * -P on a machine with more than one processor: prove that an
		 * APPLICATION processor preempts, which is the claim #461 makes
		 * and the one the uniprocessor form of this test cannot reach.
		 *
		 * Here rather than in load_context(), which is where the
		 * uniprocessor form runs: the three threads have to be bound to
		 * a processor that is already in the scheduler, and this is the
		 * first instant at which one is.  Does not return.
		 */
		if (boot_flag('P') && want > 1)
			preempt_test_run_remote();

		/*
		 * -G: thread_get_state() and thread_set_state() themselves,
		 * against a thread that is genuinely stopped (#408).
		 *
		 * Here for the same reason as -P: the target has to be bound to
		 * a processor that is already in the scheduler, and this is the
		 * first instant at which one is.
		 *
		 * ⚠️ Returns, unlike the two around it, so the boot goes on to
		 * bootstrap_create() as usual and the ordinary end-of-run checks
		 * still apply.  What it leaves behind is one thread parked for
		 * ever in a wait nobody will signal -- which is the price of a
		 * target that thread_stop_wait() can actually stop, and why this
		 * is not on the ordinary boot.
		 */
		if (boot_flag('G') && want > 1)
			thread_state_entry_test();

		/*
		 * -A: may a return to ring 0 run an AST_APC handler? (#463)
		 *
		 * Here for the same reason as the tests around it: the probe
		 * has to be bound to a processor already in the scheduler.
		 * Returns, so the boot goes on to bootstrap_create() as usual.
		 */
		if (boot_flag('A') && want > 1)
			kernel_ast_test();

		/*
		 * -M: what concurrency does to a pmap with no locking (#455).
		 *
		 * ⚠️ -M and not -C: C is the clock burn-in, and the flags are a
		 * single namespace across boot_c.c and this file.  Enumerated
		 * from the whole tree rather than from this file, which is how
		 * the collision was found.
		 *
		 * Here for the same reason as the tests around it -- the workers
		 * are bound to processors already in the scheduler.  Returns, so
		 * the boot goes on to bootstrap_create() as usual.
		 *
		 * ⚠️ Off the ordinary boot on purpose.  It provokes the race it
		 * measures, so a run that loses tables to it is the ANSWER and
		 * not a regression; putting that on every boot would make the
		 * end-of-run checks report a finding as a failure.
		 *
		 * ⚠️ It used to be gated on `want > 1' as well, from when the
		 * bench was only about concurrency.  It is not any more: it also
		 * measures what a collection gives back and what each arm of
		 * the exclusion costs, and the one-processor figure is the
		 * BASELINE of that curve -- the point where a lock is
		 * uncontended and free.  A bench that skips the uniprocessor is
		 * a bench that cannot say anything about the configuration this
		 * project treats as first class.
		 */
		if (boot_flag('M'))
			pmap_collect_bench();

		/*
		 * -L: a thread blocked with a continuation, and the prompt
		 * opened on it (#428).
		 *
		 * Here because it needs the scheduler to actually take the
		 * thread off its processor, which is what resets the stack and
		 * makes the debugger's refusal meaningful.
		 */
		if (boot_flag('L'))
			cont_probe_start();

		/*
		 * -S: stay up (#428).
		 *
		 * This kernel reaches bootstrap_create() and panics there
		 * within milliseconds of its first tick, because no userland
		 * bundle is loaded for this target yet (#422).  That is fine
		 * for a self-test run and leaves nothing to walk up to and
		 * examine: there is no RUNNING machine on x86-64, only a
		 * machine that has already stopped.
		 *
		 * So this parks the thread that would go on to
		 * bootstrap_create, and the machine simply idles -- four
		 * processors in machine_idle, the clock ticking, and the
		 * debugger's console door polled from that tick.  It is what
		 * the serial door needs to be DEMONSTRATED against rather than
		 * asserted, and it is worth having on its own: an operator who
		 * wants to look at a live kernel has had no way to keep one.
		 *
		 * ⚠️ Parked with a plain assert_wait and no continuation, on
		 * purpose.  A continuation would throw this stack away, and
		 * the stack is precisely what an operator breaking in wants to
		 * see -- the thread would be reported by its resume point,
		 * correctly, and uselessly.
		 */
		if (boot_flag('S')) {
			static int stay_up_event;

			printf("startup: staying up on request (-S) — the "
			       "machine idles here; press Ctrl-\\ on the "
			       "console for the debugger (#428)\n");

			for (;;) {
				spl_t s = splsched();

				assert_wait((event_t) &stay_up_event, TRUE);
				splx(s);
				thread_block((void (*)(void)) 0);
			}
		}

		/*
		 * -F: does vector state survive an involuntary switch? (#408)
		 *
		 * Here for the same reason as -P: the threads have to be bound
		 * to a processor that is already in the scheduler, and this is
		 * the first instant at which one is.
		 */
		if (boot_flag('F') && want > 1) {
			fpu_stress_run();
			printf("fpu_stress: halting the machine — this boot "
			       "was the test (#408)\n");
			halt_all_cpus(FALSE);
		}
		return;
	}

	/*
	 * Reported and survived, not panicked on.  A processor that was woken
	 * and did not arrive is a fact about this boot, and the machine can run
	 * on the ones that did — real_ncpus is what was found, and the
	 * scheduler's own "is the machine up" test reads avail_cpus, so a
	 * machine short of a processor stays honestly short rather than
	 * pretending.
	 *
	 * ⚠️ It is said loudly, in the word the harness greps for, because the
	 * failure is otherwise invisible: a processor stuck before cpu_up() is a
	 * processor nothing ever dispatches to, which on the console looks
	 * exactly like a machine that has fewer processors.
	 */
	printf("startup: WRONG — %u of %u processors reached the scheduler; "
	       "the rest were woken and never arrived (#461)\n", got, want);
}

/*
 * What a processor runs on its way into the scheduler, before it has a thread.
 *
 * fpu_init() again, and it is not redundant: ap_start_c() does it during
 * bring-up so that the processor can execute a vector instruction at all, and
 * this is the machine-independent kernel's own call site for the same
 * question.  The boot processor reaches here by neither path — setup_main()
 * does not call this — so the two are the AP's only coverage of it.
 *
 * ⚠️ i386 does two more things here and this machine deliberately does
 * neither.  fpu_sanity_check() is its answer to a bring-up path that once left
 * CR4.OSFXSR clear on an AP; here the bit is programmed on the one path that
 * programs anything.  hwp_init_cpu() is real work and wanted, but it changes
 * what the clock does under measurement, so it belongs with the numbers rather
 * than ahead of them.
 */
void
slave_machine_init(void)
{
	fpu_init();
}

/*
 * A window onto a physical page, for the machine-independent code that wants
 * to read or write one without a mapping of its own.
 *
 * The direct map IS the window -- every physical page is already addressable
 * through it -- so this is an addition and its counterpart is nothing.  i386
 * cannot do this: it borrows a page-table entry, programs it, and has to
 * give it back, which is why kunmap() exists there and why kmap() takes a
 * lock.
 *
 * One of the things #407 predicted the direct map would delete, deleted.
 */
vm_offset_t
kmap(vm_offset_t phys_addr)
{
	return (vm_offset_t) phys_to_direct((uint64_t) phys_addr);
}

void
kunmap(vm_offset_t va)
{
	(void) va;
}

/*
 * The current call stack, as a list of return addresses, for whoever is
 * recording where something happened.
 *
 * Walks the frame pointer chain, which this kernel keeps precisely so that
 * this is possible: -fno-omit-frame-pointer is not an accident of the build
 * flags, it is the price paid for being able to say where the machine was.
 *
 * ⚠️ Stops at the first frame pointer that is not a kernel address rather
 * than trusting the chain.  A corrupted stack is exactly when somebody wants
 * a call stack, and a walker that followed a wild pointer would fault inside
 * the code recording the fault.
 */
void
machine_callstack(natural_t *buf, vm_size_t callstack_max)
{
	uint64_t	*frame = (uint64_t *) __builtin_frame_address(0);
	vm_size_t	i;

	for (i = 0; i < callstack_max; i++) {
		uint64_t	next;

		if (!va_is_kernel((uint64_t) frame))
			break;

		buf[i] = (natural_t) frame[1];	/* the return address */

		next = frame[0];		/* the caller's frame */
		if (next <= (uint64_t) frame)	/* stacks grow down */
			break;

		frame = (uint64_t *) next;
	}

	for (; i < callstack_max; i++)
		buf[i] = 0;
}
