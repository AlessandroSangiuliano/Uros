/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What the machine looks like once it has stopped doing anything (#476).
 *
 * #476 is a boot that goes quiet: two tasks are created and resumed, and one
 * of them sometimes never runs.  The serial log says nothing about it, because
 * a thread that never runs prints nothing -- so the state has to be read out
 * of the kernel rather than waited for.
 *
 * 🔥 IT CANNOT BE READ WITH GDB, and that is measured rather than assumed.
 * With the stub attached the failure stopped appearing: eleven runs and none,
 * against three in nine without it.  If the true rate were the one measured
 * without it, eleven clean runs would happen about one time in a hundred.  The
 * instrument was closing the window it was there to look through.
 *
 * ⚠️ Which is why this prints and does not stop anything.  No breakpoint, no
 * halted processor, no gdb: a census taken from the idle loop, in thread
 * context with interrupts on, on a processor that by definition has nothing
 * else to do.  <x86_64/cpu/idle.c> already establishes that this is the right
 * place -- clock_event_drain_reports() is there for the same reason, and says
 * so.
 *
 * Once, and only after a long stretch of quiet.  A census printed while the
 * boot is still working would be a picture of a system mid-stride, which is
 * the one thing it must not be mistaken for.
 */

#include <ddb/ksym.h>
#include <mach/kern_return.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/processor.h>
#include <kern/cpu_number.h>
#include <kern/cpu_data.h>	/* #558: who is on each processor */
#include <mach/machine.h>	/* machine_slot[] */
#include <kern/misc_protos.h>
#include <kern/lock.h>
#include <kern/mutex_track.h>
#include <vm/vm_page.h>
#include <vm/vm_object.h>
#include <pmap/pmap.h>
#include <thread/context.h>
#include <sync/mutex_trace.h>
#include <cpu/quiet_census.h>

/*
 * How long "quiet" is.
 *
 * Counted in passes through the idle loop rather than in seconds, because the
 * idle loop is where this runs and a clock it does not own is a second thing
 * that has to be right.  A halted processor wakes on every timer tick, so once
 * the machine is genuinely idle these arrive at about the tick rate -- a few
 * thousand of them is seconds, which is what is wanted, and the exact number
 * does not matter as long as it is far longer than any pause a working boot
 * takes.
 *
 * ⚠️ Reset by machine_idle_exit(), so a processor that finds work starts the
 * count again.  Without that reset this would eventually fire on a healthy
 * system that simply had a slow patch, and a census of a system that is about
 * to carry on is a false report.
 */
/*
 * ⚠️ Counted on the BOOT PROCESSOR ONLY, and that is a correction rather than
 * a simplification.
 *
 * The first version counted every processor's passes and guarded the report
 * with a plain `if (said) return; said = 1;'.  Four processors went through
 * that guard before any of them had written it: the census printed forty-one
 * times, and the lines interleaved into each other and into bootstrap's --
 * `state=0x4bootstrap: 2 boot modules'.  A race, inside the instrument built
 * to look for a race, found by the run that was only meant to prove the thing
 * could print at all.
 *
 * One processor removes both faults at once and needs no atomic: there is no
 * second writer to lose to.  What it costs is the time base -- cpu 0 alone
 * wakes at the tick rate, so a pass is about ten milliseconds once the machine
 * has settled, and this many of them is about thirty seconds of quiet.  Far
 * longer than any pause a working boot takes, which is the only property the
 * number needs.
 */
#define	QUIET_PASSES	500

/*
 * The one processor that owns the count, on both sides.
 *
 * 🔥 The version before this counted here and let every processor reset, and
 * it reported nothing at all -- not even the line it printed about itself.
 * With four processors leaving idle on every tick the count could never
 * reach its threshold, and the symptom was silence.  Which is the shape every
 * defect in this instrument has had: an absence, indistinguishable from
 * "the thing being looked for did not happen".
 */
#define	QUIET_CPU	0

/* #558: no string.h here, and one comparison does not justify pulling it in. */
static int census_streq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

static unsigned long	quiet_passes;
static unsigned long	quiet_resets;
static unsigned long	quiet_peak;
static int		quiet_said;

void
quiet_census_busy(int mycpu)
{
	if (mycpu != QUIET_CPU)
		return;
	if (quiet_passes > quiet_peak)
		quiet_peak = quiet_passes;
	quiet_resets++;
	quiet_passes = 0;
}

/*
 * The states, spelled out.
 *
 * ⚠️ Names and not the hex, because the hex is what the log already could not
 * explain.  A reader looking at #476 wants to know whether the thread that
 * never ran is asleep, suspended or runnable-and-unchosen -- those are three
 * different defects in three different pieces of code, and the number is the
 * same shape for all of them.
 */
static void
census_state(int state)
{
	if (state & TH_WAIT)
		printf(" WAIT");
	if (state & TH_SUSP)
		printf(" SUSP");
	if (state & TH_RUN)
		printf(" RUN");
	if (state & TH_UNINT)
		printf(" UNINT");
	if (state & TH_IDLE)
		printf(" IDLE");
	if (state == 0)
		printf(" (none)");
}

/*
 * #558: the stack of a thread that is asleep, walked from its switch frame.
 *
 * The two halves below name a cycle, and the cycle does not fit the code: a
 * thread asleep at vm_fault.c:685 is reported as the holder of the very
 * object lock that line releases before it blocks -- in three boots of
 * three.  Either the holder record is stale, which the lock word printed
 * beside it answers, or something further up this thread's stack still holds
 * the lock, and only the stack can say what.
 *
 * Only threads that kept their stack: a continuation means the stack was
 * given up when the thread blocked (see the debugger's `thread' command,
 * which reads the same frame through the same CTX_ indices).
 *
 * Every read stays inside the thread's own kernel stack and is asked of the
 * kernel pmap first, as x86_64_backtrace() does.  ⚠️ The machine has been
 * idle for QUIET_PASSES when this runs, but another processor can still wake
 * a sleeper during the walk.  That gives a wrong chain, never a fault -- so a
 * chain is evidence when it agrees with the from= on the line above it.
 */
#define	CENSUS_STACK_MAX	16

static void
census_stack(thread_t th)
{
	pmap_t		 kernel = pmap_kernel();
	uint64_t	 low = (uint64_t) th->kernel_stack;
	uint64_t	 high = low + KERNEL_STACK_SIZE;
	uint64_t	 sp, rbp;
	const uint64_t	*saved;
	unsigned	 depth;

	if ((th->state & TH_RUN) != 0 || th->continuation != 0 ||
	    th->top_act == THR_ACT_NULL || low == 0)
		return;

	sp = th->top_act->mact.xxx_pcb.ctx.rsp;
	if (sp < low || sp + (CTX_RETURN + 1) * 8 > high ||
	    pmap_extract(kernel, sp) == 0 ||
	    pmap_extract(kernel, sp + CTX_RETURN * 8) == 0) {
		printf("quiet_census:     stack th=%p: its saved sp %p is not "
		       "inside its own stack, not walked\n", th, (void *) sp);
		return;
	}
	saved = (const uint64_t *) sp;
	rbp = saved[CTX_RBP];

	printf("quiet_census:     stack th=%p:", th);
	for (depth = 0; depth < CENSUS_STACK_MAX; depth++) {
		const uint64_t *frame;
		uint64_t	next, ret;
		uint64_t	off = 0;
		const char     *nm;

		if ((rbp & 7) != 0 || rbp < low || rbp + 16 > high)
			break;
		if (pmap_extract(kernel, rbp) == 0 ||
		    pmap_extract(kernel, rbp + 8) == 0)
			break;

		frame = (const uint64_t *) rbp;
		next = frame[0];
		ret = frame[1];
		if (ret == 0)
			break;

		/* the lookup on its own line: see report_symbol() in trap.c */
		nm = ksym_lookup_call(ret, &off);
		if (nm != 0)
			printf(" %s+0x%lx", nm, (unsigned long) off);
		else
			printf(" %p", (void *) ret);

		if (next <= rbp)
			break;
		rbp = next;
	}
	printf("\n");
}

void
quiet_census_pass(int mycpu)
{
	thread_t	th;
	int		n = 0;

	if (mycpu != QUIET_CPU)
		return;
	if (quiet_said)
		return;

	/*
	 * ⚠️ A word about itself, rarely, because the first two versions of
	 * this both reported NOTHING and an absence cannot say which of its
	 * two causes it had: cpu 0 not reaching the threshold, or the shared
	 * counter being reset out from under it by another processor finding
	 * work.  The peak and the reset count separate those, and one line
	 * every thousand passes is not enough output to matter.
	 */
	/*
	 * ⚠️ Every hundred, not every thousand.  It was every thousand while
	 * the threshold below was three thousand; lowering the threshold to
	 * five hundred left this line unreachable -- the census fires first,
	 * every time -- so the one thing that could explain a silent instrument
	 * had become part of the silence.  The interval has to stay under the
	 * threshold, which is why it is written in terms of it.
	 */
	if ((++quiet_passes % (QUIET_PASSES / 5)) == 0)
		printf("quiet_census: passes=%lu peak=%lu resets=%lu\n",
		       quiet_passes, quiet_peak, quiet_resets);

	if (quiet_passes < QUIET_PASSES)
		return;

	quiet_said = 1;

	printf("quiet_census (#476): the machine has been idle for %lu idle "
	       "passes; %d tasks and %d threads\n",
	       quiet_passes, default_pset.task_count, default_pset.thread_count);

	/*
	 * ⚠️ The NAME as well as the pointer (#425).
	 *
	 * This report used to give `task=0xffffc000004d83a0' and nothing else,
	 * and struct task has no name field -- so a wedge log listed five live
	 * user tasks that could not be told apart, and the debugger was no help
	 * because list_tasks() prints the same bare pointer.  Twice in one
	 * afternoon the question "which program is stuck?" had to be answered by
	 * guessing from who had failed to report.
	 *
	 * thread->name is filled by libmach's crt0 from argv[0] before main()
	 * runs, so every task carries its own program name on its first thread
	 * from its first instruction -- no new kernel field and no new RPC.
	 * ⚠️ Sixteen bytes: the bundle's names all fit, but a long server name
	 * (block_device_server, virtual_terminal_server) arrives truncated.
	 */
	queue_iterate(&default_pset.threads, th, thread_t, pset_threads) {
		int	walk = 0;

		printf("quiet_census:   th=%p state=%#x", th, th->state);
		census_state(th->state);
		/*
		 * #561: does this thread's vector state travel with it?  A
		 * count of exemptions says how many were granted; this says to
		 * WHOM, which is the question when the count and the effect
		 * disagree.
		 */
		if (th->top_act != THR_ACT_NULL
		    && th->top_act->mact.pcb != PCB_NULL)
			printf(" fpu=%d", th->top_act->mact.pcb->ctx.fpu_switch);
		printf(" wait_event=%p", (void *) th->wait_event);

		/*
		 * 🔑 A RUNNABLE THREAD ON AN IDLE MACHINE IS TWO DEFECTS, AND
		 * THE STATE ALONE NAMES NEITHER (#558).
		 *
		 * TH_RUN with every processor in its idle thread means either
		 * the thread is on a run queue nobody selects from -- an
		 * accounting defect, the queue's count or its bitmap -- or it
		 * is on NO queue at all, which is a thread lost between a
		 * wakeup that claimed it and a dispatch that never happened.
		 * Those want different fixes, and `runq' separates them in one
		 * field.
		 *
		 * Only for the runnable non-idle threads: the idle threads are
		 * TH_RUN by definition and are never on a queue, so printing it
		 * for them would add a column of noise to every census.
		 */
		if ((th->state & TH_RUN) && !(th->state & TH_IDLE))
			printf(" runq=%p pri=%d/%d", (void *) th->runq,
			       (int) th->sched_pri, (int) th->priority);

		/*
		 * #558: the name, not the address.
		 *
		 * An address says a thread is asleep on something; the FUNCTION
		 * that put it there says what that something IS -- lock_write
		 * means a lock_t, _mutex_lock a mutex_t, vm_fault_page a page.
		 * The kernel already carries its own symbols for backtraces, so
		 * this costs a lookup and removes a host-side step that has to
		 * be done against the exact image, which #558 spent an hour
		 * discovering is not always still around.
		 */
		if (th->wait_from != 0) {
			uint64_t    off = 0;
			const char *nm = ksym_lookup_call(
					    (uint64_t) th->wait_from, &off);

			if (nm != 0)
				printf(" from=%s+0x%lx", nm, (unsigned long) off);
			else
				printf(" from=%p", th->wait_from);

			/*
			 * 🔑 And when the sleeper is in vm_fault_page the event
			 * IS a vm_page -- the type is known, so dereferencing it
			 * is sound rather than a guess at an arbitrary address.
			 * A page still `busy' with sleepers on it is the whole
			 * shape of the wedge this was written for.
			 */
			/*
			 * 🔑 #558: when the sleeper is in mutex_lock_wait the
			 * event IS a mutex_t, and this kernel already records
			 * who took it -- MUTEX_OWNER_TRACK (#383) keeps own_thr
			 * and own_pc, built for exactly this: "a deadlocked
			 * mutex whose own_thr points at a thread the census
			 * also lists tells you the cycle".
			 *
			 * So the two halves join here: this line names the
			 * holder, and the holder's own line, a few above or
			 * below, says what IT is waiting for.
			 */
			/*
			 * 🔑 THE WORD, NOT THE HASH (#558).  kern/sync_sema.c
			 * records it for exactly this: the wait event is a hash
			 * chosen so distinct words rarely collide, which is
			 * right for matching a wake to a waiter and useless for
			 * reading a report -- eight threads on eight hashes say
			 * only that they are eight different words.
			 */
			if (th->futex_uaddr != 0)
				printf(" futex=%p", (void *) th->futex_uaddr);

			if (nm != 0 && th->wait_event != 0 &&
			    census_streq(nm, "mutex_lock_wait")) {
				mutex_t	   *mx = (mutex_t *) th->wait_event;
				uint64_t    poff = 0;
				const char *pn;

				printf(" [mutex held-by=%p", (void *) mx->own_thr);
				pn = ksym_lookup_call((uint64_t) mx->own_pc, &poff);
				if (pn != 0)
					printf(" taken-at=%s+0x%lx", pn,
					       (unsigned long) poff);
				else
					printf(" taken-at=%p", (void *) mx->own_pc);
				/*
				 * ⚠️ And whether it is held at all.  held-by
				 * is a note written beside the lock word, not
				 * the word: a free word under five sleepers
				 * is a lost wakeup (#476), a held one is a
				 * holder, and the note alone cannot tell them
				 * apart.
				 */
				printf(" locked=%d waiters=%d]", (int) mx->locked,
				       (int) mx->waiters);
				walk = 1;
			}

			if (nm != 0 && th->wait_event != 0 &&
			    census_streq(nm, "vm_fault_page")) {
				vm_page_t m = (vm_page_t) th->wait_event;

#if	MACH_ASSERT
				/*
				 * #558: and WHICH LINE marked it busy.  The
				 * state alone names a symptom; the site names
				 * the defect.  vm_fault.c, which is where
				 * every one of these sites lives.
				 */
				if (m->busy && m->busy_line != 0)
					printf(" busied-at=vm_fault.c:%u",
					       m->busy_line);
#endif	/* MACH_ASSERT */
				printf(" [page busy=%d wanted=%d wire=%d"
				       " obj=%p off=0x%lx]",
				       (int) m->busy, (int) m->wanted,
				       (int) m->wire_count, m->object,
				       (unsigned long) m->offset);
				/*
				 * The lock of the page's own object, read from
				 * this side too: the sleeper released it at
				 * vm_fault.c:686, so a word held here with
				 * this thread's name on it is the contradiction
				 * the stack below has to explain.
				 */
				if (m->object != VM_OBJECT_NULL)
					printf(" [obj-lock locked=%d waiters=%d"
					       " owner=%p]",
					       (int) m->object->Lock.locked,
					       (int) m->object->Lock.waiters,
					       (void *) m->object->Lock.own_thr);
				walk = 1;
			}
		}
		/*
		 * 🔑 AND WHERE RING 3 WAS (#558).
		 *
		 * Nine threads asleep in urmach_futex are nine identical kernel
		 * stacks, and the question is which call in the thread library
		 * each of them made -- the same question #425 answered for the
		 * debugger, with the same frame: the one the trap pushed on the
		 * way in, still in this thread's own kernel stack.
		 *
		 * ⚠️ Printed as a bare address on purpose.  The symbols this
		 * kernel carries are its OWN, and naming a user address from
		 * them would produce a confident wrong answer; resolve it
		 * outside, against the program's binary.
		 */
		if (th->top_act != THR_ACT_NULL) {
			printf(" task=%p susp=%d",
			       th->top_act->task, th->top_act->suspend_count);
			if (th->top_act->mact.xxx_pcb.user != 0)
				printf(" user-rip=%p",
				       (void *) th->top_act->mact.xxx_pcb.user->rip);
		}
		if (th->name[0] != '\0')
			printf(" name=\"%s\"", th->name);
		printf("\n");
		if (walk)
			census_stack(th);
		n++;
	}

	printf("quiet_census: %d threads listed\n", n);

	/*
	 * 🔥 AND WHO IS ON EACH PROCESSOR, WITHOUT WHICH "IDLE" IS HALF A WORD
	 * (#558).
	 *
	 * The count above is cpu 0's alone -- quiet_census_busy() resets it when
	 * THIS processor finds work -- so "the machine has been idle" is really
	 * "cpu 0 has been idle".  A thread listed as TH_RUN on no run queue then
	 * has two readings that the list cannot tell apart: lost between a
	 * wakeup that claimed it and a dispatch that never came, or RUNNING on
	 * another processor all along, spinning somewhere.
	 *
	 * The active thread per processor decides it, and it is the same field
	 * the debugger reads to answer "which thread is this".
	 */
	{
		int i;

		for (i = 0; i < NCPUS; i++) {
			thread_t act;

			if (!machine_slot[i].is_cpu || !machine_slot[i].running)
				continue;

			act = cpu_data[i].active_thread;
			printf("quiet_census: cpu %d active=%p", i, act);
			if (act != THREAD_NULL) {
				printf(" state=%#x", act->state);
				census_state(act->state);
				if (act->name[0] != '\0')
					printf(" name=\"%s\"", act->name);
			}
			printf("\n");
		}
	}

	/*
	 * And who is holding the one that everything piles up behind (#476).
	 *
	 * In a boot that stops, the census shows threads asleep uninterruptibly
	 * on vm_page_queue_lock and on map locks, and a task created but never
	 * resumed behind them.  That is a lock held and not released, not a
	 * thread the scheduler forgot -- and the only thing missing from the
	 * report was the name of the holder.
	 *
	 * ⚠️ own_thr is only meaningful because <x86_64/sync/mutex.h> now writes
	 * it.  It obeyed MACH_LDEBUG, which is off, while the field it fills is
	 * created by MUTEX_OWNER_TRACK, which is on -- so on this machine the
	 * holder had never been recorded, and a report reading this field would
	 * have named whatever the heap contained.
	 */
	/*
	 * ⚠️ Two switches, and they are not the same question.  The state and
	 * the owner cost nothing to print and are what named #476, so they are
	 * here whenever the fields exist; the transition trace has hooks on the
	 * hot path of every mutex in the kernel, so it is off unless somebody
	 * is hunting.
	 */
	mutex_trace_report();		/* nothing unless MUTEX_TRACE_ON */

	/*
	 * 🔑 Whether the vector-state exemption fires, and how often (#561).
	 *
	 * The switch carries a thread's FPU state only for threads that
	 * declared they need it; every thread of the kernel task is exempt.
	 * That is worth 228 ns a round trip where it applies (#554) and
	 * NOTHING at all if it never applies, and the two are indistinguishable
	 * without a count.  Printed here because here is after the work: the
	 * census runs when the machine has gone quiet.
	 */
	/*
	 * 🔑 THE FREE HALF OF THE OBSERVATION (#561).
	 *
	 * How OFTEN the vector-state exemption fires was counted once, on the
	 * switch path, to justify the change -- and then taken off it: three
	 * increments on every context switch is a cost paid for ever to learn
	 * something learned once.
	 *
	 * What stays is what catches the failure, and it costs nothing here:
	 * this number, written at thread creation, and the `fpu=' on each
	 * thread's line above.  The way the exemption broke was that it was
	 * granted and then thrown away by a second context_init(), and the
	 * shape of that was exactly "exempted at creation says 14, and every
	 * live kernel thread says fpu=1".  Both halves are printed, so that
	 * disagreement cannot hide.
	 */
	printf("quiet_census: contexts exempted from vector state at "
	       "creation=%lu (#561)\n", context_fpu_exempted);
#if	CONTEXT_FPU_COUNT
	{
		unsigned long long sw, sa, re;

		context_fpu_counts(&sw, &sa, &re);
		printf("quiet_census: fpu switches=%llu, saves skipped=%llu, "
		       "restores skipped=%llu (#561)\n", sw, sa, re);
	}
#endif	/* CONTEXT_FPU_COUNT */

#if	MUTEX_OWNER_TRACK
	printf("quiet_census: vm_page_queue_lock locked=%d waiters=%d "
	       "owner=%p took_it_at=%p\n",
	       (int) vm_page_queue_lock.locked,
	       (int) vm_page_queue_lock.waiters,
	       (void *) vm_page_queue_lock.own_thr,
	       (void *) vm_page_queue_lock.own_pc);
#endif	/* MUTEX_OWNER_TRACK */
}
