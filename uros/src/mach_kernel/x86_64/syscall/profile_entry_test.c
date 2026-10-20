/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -e: a profiled trap taken off its processor between the entry and the
 * dispatch (#622).
 *
 * ── What can go wrong ─────────────────────────────────────────────────
 *
 * With SYSCALL_PROFILE on, the entry stub reads the clock as a system call
 * comes in, with interrupts masked, and the profile opens the trap's sample
 * with that reading.  The dispatcher's C takes it back after the entry's sti,
 * at level zero, where this kernel preempts.  A thread taken off its
 * processor in between resumes, perhaps on another one, and takes whatever
 * reading it finds there.  Kept per processor, that can be another thread's,
 * or the thread's own from an earlier call: the stub writes it on every call,
 * and only the profiled trap takes it.  Kept on the thread, it is this call's.
 *
 * ── How it is asked ───────────────────────────────────────────────────
 *
 * The window is in the entry from ring 3, so the traps are the userland's:
 * libmach sends every message through urmach_msg, trap 33, which the profile
 * follows unless the build chose another.  This starts the spinners and
 * returns, and the boot goes on into bootstrap_create() under them.  Two a
 * processor, at a user thread's priority and free to move, so that quanta
 * running out take user threads off their processors and give them back
 * wherever there is room.  The spinners make no system calls, so a
 * processor's reading stays as the last user thread left it: the case a
 * per-processor slot gets wrong.
 *
 * The window is a few dozen instructions, and a quantum seldom ends in it.
 * ablations/622-widen-entry.patch holds it open with a spin before the
 * profiled trap takes its reading.
 *
 * Three counts from <kern/syscall_profile.h>, all answered against the
 * thread's last return to user mode and none against the reading:
 *
 *   asked	profiled traps whose thread was given a processor again after
 *		it entered: the window was crossed.  The run lasts until
 *		PROFILE_ENTRY_MIN_ASKED of them, or PROFILE_ENTRY_MAX_SECONDS,
 *		and fewer is NOT ASKED.
 *   stale	profiled traps whose reading was older than that return, an
 *		earlier call's.  WRONG.
 *   crossed_sampled
 *		profiled traps that crossed the window and were sampled all the
 *		same, with the time off the processor in their entry phase.
 *		WRONG: whatever reading such a trap found, its own fresh one
 *		included, it measures something else.
 */

#include <stdint.h>

#include <kern/lock.h>			/* mutex_pause */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched.h>			/* BASEPRI_USER */
#include <kern/sched_prim.h>
#include <kern/syscall_profile.h>	/* the two counts, when compiled in */
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_swap.h>		/* thread_swappable */

#include <cpu/regs.h>			/* cpu_pause */
#include <sync/atomic.h>
#include <syscall/profile_entry_test.h>
#include <time/tsc.h>

#define	PROFILE_ENTRY_PER_CPU	2	/* spinners a processor: room to move */

/*
 * The run lasts until this many traps have crossed the window, and a clean
 * run with fewer is NOT ASKED.  A count, as spl_test.c's is, for its reason:
 * a crossing is a chance for the defect to show, not a certainty, and the
 * rate depends on the accelerator and the clock while the question does not.
 *
 * 🔴 A HUNDRED, MEASURED.  With twenty, on the epic's tree with the window
 * held open, each of two boots on victus caught one trap sampled after a
 * crossing (KVM and TCG, 20/10/2026): one in twenty, so twenty crossings miss
 * the defect about one boot in three, and a hundred under one in a hundred.
 */
#define	PROFILE_ENTRY_MIN_ASKED		100
/*
 * ⚠️ And NOT ASKED if it takes longer.  Short, because the verdict has to come
 * while the userland is still booting: the run ends where the bundle does,
 * and a verdict after that is never read.
 */
#define	PROFILE_ENTRY_MAX_SECONDS	20

#if	SYSCALL_PROFILE

static volatile int		pe_stop;
static volatile uint32_t	pe_started;
static volatile uint32_t	pe_finished;
static uint32_t			pe_spinners;
static uint32_t			pe_ncpu;

static void
pe_spinner(void)
{
	atomic_add32(&pe_started, 1);
	while (!pe_stop)
		cpu_pause();
	atomic_add32(&pe_finished, 1);
	thread_terminate_self();
}

/*
 * spl_test.c's thread: a user thread's priority, and NOT bound, since what
 * the spinners are for is moving the user threads about.
 */
static thread_t
pe_thread(void)
{
	thread_t	th;
	thread_act_t	act;
	spl_t		s;

	if (thread_create_at(kernel_task, &th, pe_spinner) != KERN_SUCCESS)
		return THREAD_NULL;

	thread_swappable(th->top_act, FALSE);

	s = splsched();
	thread_lock(th);

	act = th->top_act;
	th->max_priority = BASEPRI_USER;
	th->priority = BASEPRI_USER;
	th->sched_pri = BASEPRI_USER;

	th->state |= TH_RUN;
	thread_setrun(th, TRUE, TAIL_Q);
	thread_unlock(th);
	splx(s);

	act_deallocate(act);
	thread_resume(act);

	return th;
}

/*
 * Wait until *count reaches want or `seconds' of TSC pass; says which.  A
 * tick asleep at a time, as in spl_test.c: this thread outranks the spinners
 * and would take a processor from them if it spun.
 */
static boolean_t
pe_wait(volatile uint32_t *count, uint32_t want, uint64_t hz,
	unsigned seconds)
{
	uint64_t	t0 = rdtsc();

	while (*count < want) {
		if (rdtsc() - t0 >= hz * seconds)
			return FALSE;
		mutex_pause();
	}
	return TRUE;
}

static void	pe_verdict(uint32_t asked, uint32_t stale, uint32_t crossed,
			   uint64_t ms);

static void
pe_monitor(void)
{
	uint64_t	hz = tsc_hz(), t0 = rdtsc(), ms;
	uint32_t	asked, stale, crossed;

	(void) pe_wait(&syscall_profile_asked, PROFILE_ENTRY_MIN_ASKED, hz,
		       PROFILE_ENTRY_MAX_SECONDS);
	asked = syscall_profile_asked;
	stale = syscall_profile_stale;
	crossed = syscall_profile_crossed_sampled;
	ms = (rdtsc() - t0) * 1000 / hz;
	pe_stop = 1;

	if (!pe_wait(&pe_finished, pe_spinners, hz, 5))
		printf("profile_entry: WRONG — %u of %u spinners stopped in 5 s: "
		       "a processor stopped scheduling (#622)\n",
		       (unsigned) pe_finished, (unsigned) pe_spinners);
	else
		pe_verdict(asked, stale, crossed, ms);
	thread_terminate_self();
}

static void
pe_verdict(uint32_t asked, uint32_t stale, uint32_t crossed, uint64_t ms)
{
	printf("profile_entry: %u spinners on %u processors for %u ms: %u "
	       "profiled traps were taken off their processor between the "
	       "entry and the dispatch, and %u of them sampled all the same; "
	       "%u opened with a reading older than their thread's last "
	       "return to user mode\n",
	       (unsigned) pe_spinners, (unsigned) pe_ncpu, (unsigned) ms,
	       (unsigned) asked, (unsigned) crossed, (unsigned) stale);

	if (stale != 0 || crossed != 0)
		printf("profile_entry: WRONG — %s (#622)\n",
		       stale != 0 ? "a profiled trap opened with an earlier "
				    "call's reading, not its own"
				  : "a profiled trap taken off its processor "
				    "before its reading was taken was sampled, "
				    "the time off in its entry phase");
	else if (asked < PROFILE_ENTRY_MIN_ASKED)
		printf("profile_entry: NOT ASKED — %u profiled traps crossed the "
		       "window in %u s, fewer than %u: a reading taken from the "
		       "wrong place had too little chance to show (#622)\n",
		       (unsigned) asked, PROFILE_ENTRY_MAX_SECONDS,
		       PROFILE_ENTRY_MIN_ASKED);
	else
		printf("profile_entry: PASS — none of the %u profiled traps that "
		       "crossed the window was sampled, and no reading was older "
		       "than its thread's last return to user mode (#622)\n",
		       (unsigned) asked);
}

#endif	/* SYSCALL_PROFILE */

void
profile_entry_test_start(void)
{
#if	SYSCALL_PROFILE
	uint64_t	hz = tsc_hz();
	uint32_t	k;
	int		i;

	if (hz == 0) {
		printf("profile_entry: NOT ASKED — no calibrated TSC to time the "
		       "run with (#622)\n");
		return;
	}

	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p != PROCESSOR_NULL && p->state != PROCESSOR_OFF_LINE)
			pe_ncpu++;
	}

	/*
	 * 🔑 NOT ASKED, and true, as spl_test.c says of its own window: on one
	 * processor a thread taken off resumes where it was, and the only
	 * reading it can find there that is its own is the one it left.
	 */
	if (pe_ncpu < 2) {
		printf("profile_entry: NOT ASKED — %u processor: a thread taken "
		       "off resumes where it was, so its own earlier reading "
		       "cannot be waiting there (#622)\n", (unsigned) pe_ncpu);
		return;
	}

	for (k = 0; k < pe_ncpu * PROFILE_ENTRY_PER_CPU; k++) {
		if (pe_thread() == THREAD_NULL)
			break;
		pe_spinners++;
	}

	printf("profile_entry: starting: %u spinners on %u processors while the "
	       "userland boots, until %u calls of trap %d have been taken off "
	       "their processor between the entry and the dispatch, for %u s "
	       "at most (#622)\n", (unsigned) pe_spinners, (unsigned) pe_ncpu,
	       PROFILE_ENTRY_MIN_ASKED, syscall_profile_trap,
	       PROFILE_ENTRY_MAX_SECONDS);
	(void) kernel_thread(kernel_task, pe_monitor, (char *) 0);
#else	/* !SYSCALL_PROFILE */
	printf("profile_entry: NOT ASKED — the trap profile is not compiled in; "
	       "build with UROS_SYSCALL_PROFILE (#622)\n");
#endif	/* SYSCALL_PROFILE */
}
