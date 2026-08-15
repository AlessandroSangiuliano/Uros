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

#include <mach/kern_return.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/processor.h>
#include <kern/cpu_number.h>
#include <kern/misc_protos.h>
#include <kern/lock.h>
#include <kern/mutex_track.h>
#include <vm/vm_page.h>
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

	queue_iterate(&default_pset.threads, th, thread_t, pset_threads) {
		printf("quiet_census:   th=%p state=%#x", th, th->state);
		census_state(th->state);
		printf(" wait_event=%p", (void *) th->wait_event);
		if (th->top_act != THR_ACT_NULL)
			printf(" task=%p susp=%d",
			       th->top_act->task, th->top_act->suspend_count);
		printf("\n");
		n++;
	}

	printf("quiet_census: %d threads listed\n", n);

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

#if	MUTEX_OWNER_TRACK
	printf("quiet_census: vm_page_queue_lock locked=%d waiters=%d "
	       "owner=%p took_it_at=%p\n",
	       (int) vm_page_queue_lock.locked,
	       (int) vm_page_queue_lock.waiters,
	       (void *) vm_page_queue_lock.own_thr,
	       (void *) vm_page_queue_lock.own_pc);
#endif	/* MUTEX_OWNER_TRACK */
}
