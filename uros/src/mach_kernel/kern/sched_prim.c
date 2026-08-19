/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */
/*
 * MkLinux
 */
/* CMU_HIST */
/*
 * Revision 2.18.4.5  92/04/30  11:59:17  bernadat
 * 	Increased MAX_STUCK_THREADS to 64
 * 	[92/03/19            bernadat]
 * 
 * Revision 2.18.4.4  92/03/28  10:10:15  jeffreyh
 * 	Pick up changes from MK71
 * 	[92/03/20  13:16:25  jeffreyh]
 * 
 * Revision 2.19  92/02/19  16:06:35  elf
 * 	Added argument to compute_priority.  We dont always want to do
 * 	a reschedule right away.
 * 	[92/01/19            rwd]
 * 
 * Revision 2.18.4.3  92/03/03  16:20:15  jeffreyh
 * 	Fix printf
 * 	[92/02/25            jeffreyh]
 * 
 * Revision 2.18.4.2  92/02/21  14:26:59  jsb
 * 	Removed spurious newline.
 * 
 * Revision 2.18.4.1  92/02/18  19:10:20  jeffreyh
 * 	Increased MAX_STUCK_THREADS to 64
 * 	[92/02/11  08:01:16  bernadat]
 * 
 * Revision 2.18  91/09/04  11:28:26  jsb
 * 	Add a temporary hack to thread_dispatch for i860 support:
 * 	don't panic if thread->state is TH_WAIT.
 * 	[91/09/04  09:24:43  jsb]
 * 
 * Revision 2.17  91/08/24  11:59:46  af
 * 	Final form of do_priority_computation was missing a pair of parenthesis.
 * 	[91/07/19            danner]
 * 
 * Revision 2.16  91/07/31  17:47:27  dbg
 * 	When re-invoking the running thread (thread_block, thread_run):
 * 	. Mark the thread interruptible.
 * 	. If there is a continuation, call it instead of returning.
 * 	[91/07/26            dbg]
 * 
 * 	Fix timeout race.
 * 	[91/05/23            dbg]
 * 
 * 	Revised scheduling state machine.
 * 	[91/05/22            dbg]
 * 
 * Revision 2.15  91/05/18  14:32:58  rpd
 * 	Added check_simple_locks to thread_block and thread_run.
 * 	[91/05/02            rpd]
 * 	Changed recompute_priorities to use a private timer.
 * 	Changed thread_timeout_setup to initialize depress_timer.
 * 	[91/03/31            rpd]
 * 
 * 	Updated thread_invoke to check stack_privilege.
 * 	[91/03/30            rpd]
 * 
 * Revision 2.14  91/05/14  16:46:16  mrt
 * 	Correcting copyright
 * 
 * Revision 2.13  91/05/08  12:48:33  dbg
 * 	Distinguish processor sets from run queues in choose_pset_thread!
 * 	Remove (long dead) 'someday' code.
 * 	[91/04/26  14:43:26  dbg]
 * 
 * Revision 2.12  91/03/16  14:51:09  rpd
 * 	Added idle_thread_continue, sched_thread_continue.
 * 	[91/01/20            rpd]
 * 
 * 	Allow swapped threads on the run queues.
 * 	Added thread_invoke, thread_select.
 * 	Reorganized thread_block, thread_run.
 * 	Changed the AST interface; idle_thread checks for ASTs now.
 * 	[91/01/17            rpd]
 * 
 * Revision 2.11  91/02/05  17:28:57  mrt
 * 	Changed to new Mach copyright
 * 	[91/02/01  16:16:51  mrt]
 * 
 * Revision 2.10  91/01/08  15:16:38  rpd
 * 	Added KEEP_STACKS support.
 * 	[91/01/06            rpd]
 * 	Added thread_continue_calls counter.
 * 	[91/01/03  22:07:43  rpd]
 * 
 * 	Added continuation argument to thread_run.
 * 	[90/12/11            rpd]
 * 	Added continuation argument to thread_block/thread_continue.
 * 	Removed FAST_CSW conditionals.
 * 	[90/12/08            rpd]
 * 
 * 	Removed thread_swap_tick.
 * 	[90/11/11            rpd]
 * 
 * Revision 2.9  90/09/09  14:32:36  rpd
 * 	Removed do_pset_scan call from sched_thread.
 * 	[90/08/30            rpd]
 * 
 * Revision 2.8  90/08/07  22:22:54  rpd
 * 	Fixed casting of a volatile comparison: the non-volatile should be casted or else.
 * 	Removed silly set_leds() mips thingy.
 * 	[90/08/07  15:56:08  af]
 * 
 * Revision 2.7  90/08/07  17:58:55  rpd
 * 	Removed sched_debug; converted set_pri, update_priority,
 * 	and compute_my_priority to real functions.
 * 	Picked up fix for thread_block, to check for processor set mismatch.
 * 	Record last processor info on all multiprocessors.
 * 	[90/08/07            rpd]
 * 
 * Revision 2.6  90/06/02  14:55:49  rpd
 * 	Updated to new scheduling technology.
 * 	[90/03/26  22:16:03  rpd]
 * 
 * Revision 2.5  90/01/11  11:43:51  dbg
 * 	Check for cpu shutting down on exit from idle loop - next_thread
 * 	will be THREAD_NULL in this case.
 * 	[90/01/03            dbg]
 * 
 * 	Make sure cpu is marked active on all exits from idle loop.
 * 	[89/12/11            dbg]
 * 
 * 	Removed more lint.
 * 	[89/12/05            dbg]
 * 
 * 	DLB's scheduling changes in thread_block don't work if partially
 * 	applied; a thread can run in two places at once.  Revert to old
 * 	code, pending a complete merge.
 * 	[89/12/04            dbg]
 * 
 * Revision 2.4  89/11/29  14:09:11  af
 * 	On Mips, delay setting of active_threads inside load_context,
 * 	or we might take exceptions in an embarassing state.
 * 	[89/11/03  17:00:04  af]
 * 
 * 	Long overdue fix: the pointers that the idle thread uses to check
 * 	for someone to become runnable are now "volatile".  This prevents
 * 	smart compilers from overoptimizing (e.g. Mips).
 * 
 * 	While looking for someone to run in the idle_thread(), rotate
 * 	console lights on Mips to show we're alive [useful when machine
 * 	becomes catatonic].
 * 	[89/10/28            af]
 * 
 * Revision 2.3  89/09/08  11:26:22  dbg
 * 	Add extra thread state cases to thread_switch, since it may now
 * 	be called by a thread about to block.
 * 	[89/08/22            dbg]
 * 
 * Revision 2.16.2.1  91/08/19  13:45:35  danner
 * 	Final form of do_priority_computation was missing a pair of parenthesis.
 * 	[91/07/19            danner]
 * 
 * 19-Dec-88  David Golub (dbg) at Carnegie-Mellon University
 *	Changes for MACH_KERNEL:
 *	. Import timing definitions from sys/time_out.h.
 *	. Split uses of PMAP_ACTIVATE and PMAP_DEACTIVATE into
 *	  separate _USER and _KERNEL macros.
 *
 * Revision 2.8  88/12/19  02:46:33  mwyoung
 * 	Corrected include file references.  Use <kern/macro_help.h>.
 * 	[88/11/22            mwyoung]
 * 	
 * 	In thread_wakeup_with_result(), only lock threads that have the
 * 	appropriate wait_event.  Both the wait_event and the hash bucket
 * 	links are only modified with both the thread *and* hash bucket
 * 	locked, so it should be safe to read them with either locked.
 * 	
 * 	Documented the wait event mechanism.
 * 	
 * 	Summarized ancient history.
 * 	[88/11/21            mwyoung]
 * 
 * Revision 2.7  88/08/25  18:18:00  mwyoung
 * 	Corrected include file references.
 * 	[88/08/22            mwyoung]
 * 	
 * 	Avoid unsigned computation in wait_hash.
 * 	[88/08/16  00:29:51  mwyoung]
 * 	
 * 	Add priority check to thread_check; make queue index unsigned,
 * 	so that checking works correctly at all.
 * 	[88/08/11  18:47:55  mwyoung]
 * 
 * 11-Aug-88  David Black (dlb) at Carnegie-Mellon University
 *	Support ast mechanism for threads.  Thread from local_runq gets
 *	minimum quantum to start.
 *
 *  9-Aug-88  David Black (dlb) at Carnegie-Mellon University
 *	Moved logic to detect and clear next_thread[] dispatch to
 *	idle_thread() from thread_block().
 *	Maintain first_quantum field in thread instead of runrun.
 *	Changed preempt logic in thread_setrun.
 *	Avoid context switch if current thread is still runnable and
 *	processor would go idle as a result.
 *	Added scanner to unstick stuck threads.
 *
 * Revision 2.6  88/08/06  18:25:03  rpd
 * Eliminated use of kern/mach_ipc_defs.h.
 * 
 * 10-Jul-88  David Golub (dbg) at Carnegie-Mellon University
 *	Check for negative priority (BUG) in thread_setrun.
 *
 * Revision 2.5  88/07/20  16:39:35  rpd
 * Changed "NCPUS > 1" conditionals that were eliminating dead
 * simple locking code to MACH_SLOCKS conditionals.
 * 
 *  7-Jul-88  David Golub (dbg) at Carnegie-Mellon University
 *	Split uses of PMAP_ACTIVATE and PMAP_DEACTIVATE into separate
 *	_USER and _KERNEL macros.
 *
 * 15-Jun-88  Michael Young (mwyoung) at Carnegie-Mellon University
 *	Removed excessive thread_unlock() occurrences in thread_wakeup.
 *	Problem discovered and solved by Richard Draves.
 *
 * Historical summary:
 *
 *	Redo priority recomputation. [dlb, 29 feb 88]
 *	New accurate timing. [dlb, 19 feb 88]
 *	Simplified choose_thread and thread_block. [dlb, 18 dec 87]
 *	Add machine-dependent hooks in idle loop. [dbg, 24 nov 87]
 *	Quantum scheduling changes. [dlb, 14 oct 87]
 *	Replaced scheduling logic with a state machine, and included
 *	 timeout handling. [dbg, 05 oct 87]
 *	Deactivate kernel pmap in idle_thread. [dlb, 23 sep 87]
 *	Favor local_runq in choose_thread. [dlb, 23 sep 87]
 *	Hacks for master processor handling. [rvb, 12 sep 87]
 *	Improved idle cpu and idle threads logic. [dlb, 24 aug 87]
 *	Priority computation improvements. [dlb, 26 jun 87]
 *	Quantum-based scheduling. [avie, dlb, apr 87]
 *	Improved thread swapper. [avie, 13 mar 87]
 *	Lots of bug fixes. [dbg, mar 87]
 *	Accurate timing support. [dlb, 27 feb 87]
 *	Reductions in scheduler lock contention. [dlb, 18 feb 87]
 *	Revise thread suspension mechanism. [avie, 17 feb 87]
 *	Real thread handling [avie, 31 jan 87]
 *	Direct idle cpu dispatching. [dlb, 19 jan 87]
 *	Initial processor binding. [avie, 30 sep 86]
 *	Initial sleep/wakeup. [dbg, 12 jun 86]
 *	Created. [avie, 08 apr 86]
 */
/* CMU_ENDHIST */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988,1987 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */
/*
 *	File:	sched_prim.c
 *	Author:	Avadis Tevanian, Jr.
 *	Date:	1986
 *
 *	Scheduling primitives
 *
 */

#include <cpus.h>
#include <mach_rt.h>
#include <mach_kdb.h>
#include <simple_clock.h>
#include <mach_host.h>
#include <hw_footprint.h>
#include <fast_tas.h>
#include <power_save.h>
#include <fast_idle.h>
#include <task_swapper.h>

#include <ddb/db_output.h>
#include <mach/machine.h>
#include <kern/ast.h>
#include <kern/counters.h>
#include <kern/cpu_number.h>
#include <kern/etap_macros.h>
#include <kern/lock.h>
#include <kern/macro_help.h>
#include <kern/machine.h>
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/queue.h>
#include <kern/rcu.h>
#include <kern/sched.h>
#include <kern/sched_prim.h>
#include <kern/ipc_sched.h>
#include <kern/syscall_subr.h>
#include <kern/thread.h>
#include <kern/thread_swap.h>
#include <kern/time_out.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <mach/policy.h>

#if	TASK_SWAPPER
#include <kern/task_swap.h>
extern int task_swap_on;
#endif	/* TASK_SWAPPER */

extern int hz;

int		min_quantum;	/* defines max context switch rate */

unsigned	sched_tick;

#if	SIMPLE_CLOCK
int		sched_usec;
#endif	/* SIMPLE_CLOCK */

thread_t	sched_thread_id;

/* Forwards */
void		wait_queue_init(void);

void		set_pri(
			thread_t	th,
			int		pri,
			int		resched);

thread_t	choose_pset_thread(
			processor_t		myprocessor,
			processor_set_t		pset);

#if	NCPUS > 1
thread_t	choose_thread(
			processor_t		myprocessor);
#endif	/*NCPUS > 1*/

int		run_queue_enqueue(
			run_queue_t	rq,
			thread_t	th,
			boolean_t	tail);

void		idle_thread_continue(void);
void		sched_thread_continue(void);
void		do_thread_scan(void);
boolean_t	do_runq_scan(
			run_queue_t	runq);

#if	FAST_IDLE
int		idle_thread_loop(int);
#endif	/* FAST_IDLE */

void		dump_run_queues(run_queue_t);
void		dump_run_queue_struct( run_queue_t );
void		dump_processor( processor_t );
void		dump_processor_set( processor_set_t );

#if	DEBUG
void		checkrq(
			run_queue_t	rq,
			char		*msg);

void		thread_check(
			thread_t	th,
			run_queue_t	rq);

#endif	/*DEBUG*/

timer_elt_data_t recompute_priorities_timer;

/*
 *	State machine
 *
 * states are combinations of:
 *  R	running
 *  W	waiting (or on wait queue)
 *  N	non-interruptible
 *  O	swapped out
 *  I	being swapped in
 *
 * init	action 
 *	assert_wait thread_block    clear_wait 		swapout	swapin
 *
 * R	RW, RWN	    R;   setrun	    -	       		-
 * RN	RWN	    RN;  setrun	    -	       		-
 *
 * RW		    W		    R	       		-
 * RWN		    WN		    RN	       		-
 *
 * W				    R;   setrun		WO
 * WN				    RN;  setrun		-
 *
 * RO				    -			-	R
 *
 */

/*
 *	Waiting protocols and implementation:
 *
 *	Each thread may be waiting for exactly one event; this event
 *	is set using assert_wait().  That thread may be awakened either
 *	by performing a thread_wakeup_prim() on its event,
 *	or by directly waking that thread up with clear_wait().
 *
 *	The implementation of wait events uses a hash table.  Each
 *	bucket is queue of threads having the same hash function
 *	value; the chain for the queue (linked list) is the run queue
 *	field.  [It is not possible to be waiting and runnable at the
 *	same time.]
 *
 *	Locks on both the thread and on the hash buckets govern the
 *	wait event field and the queue chain field.  Because wakeup
 *	operations only have the event as an argument, the event hash
 *	bucket must be locked before any thread.
 *
 *	Scheduling operations may also occur at interrupt level; therefore,
 *	interrupts below splsched() must be prevented when holding
 *	thread or hash bucket locks.
 *
 *	The wait event hash table declarations are as follows:
 */

#define NUMQUEUES	59

queue_head_t		wait_queue[NUMQUEUES];
decl_simple_lock_data(,wait_lock[NUMQUEUES])

/*
 * Which bucket an event waits in.
 *
 * The event is a pointer used as a token, so the bits that distinguish two
 * events are the low ones -- the allocator's granularity and above.  It is
 * hashed at pointer width, which on a 64-bit machine is the whole of it: the
 * old form cast to int, discarding the upper half and taking two events that
 * differ only above bit 31 to the same bucket.
 *
 * The `< 0 ? ~x : x' dance went with that cast.  It existed because a
 * pointer cast to a SIGNED int is negative whenever the top bit is set, and
 * % of a negative value in C truncates toward zero -- so it would have
 * indexed the array from below.  Hashing unsigned removes the question
 * rather than answering it.
 *
 * ⚠️ NUMQUEUES is 59, a prime, and the modulo is what mixes: with a power of
 * two it would keep only the low bits and every event in one allocation slab
 * would land in the same bucket.  The prime is load-bearing, not a taste.
 */
#define wait_hash(event) \
	(((vm_offset_t)(event)) % NUMQUEUES)

void
sched_init(void)
{
	recompute_priorities_timer.fcn = (timeout_fcn_t)recompute_priorities;
	recompute_priorities_timer.param = (void *)0;

	min_quantum = hz / 10;		/* context switch 10 times/second */
	wait_queue_init();
	pset_sys_bootstrap();		/* initialize processer mgmt. */
	queue_init(&action_queue);
	simple_lock_init(&action_lock, ETAP_THREAD_ACTION);
	sched_tick = 0;
#if	SIMPLE_CLOCK
	sched_usec = 0;
#endif	/* SIMPLE_CLOCK */
	ast_init();
}

void
wait_queue_init(void)
{
	register int	i;

	for (i = 0; i < NUMQUEUES; i++) {
		queue_init(&wait_queue[i]);
		simple_lock_init(&wait_lock[i], ETAP_THREAD_WAIT);
	}
}

/*
 *	Thread timeout routine, called when timer expires.
 */
void
thread_timeout(
	thread_t thread)
{
	spl_t		s;
	boolean_t	did_timeout;

	s = splsched();
	thread_lock(thread);
	if (thread->timer.set == TELT_PENDING) {
		did_timeout = TRUE;
		thread->timer.set = TELT_UNSET;
		clear_wait_locked(thread, THREAD_TIMED_OUT, FALSE);
	} else {
		did_timeout = FALSE;
	}
	thread_unlock(thread);
	if (did_timeout) {
		/* deallocate the extra reference for the timeout */
		thread_deallocate(thread);
	}
	splx(s);
}

/*
 *	thread_set_timeout:
 *
 *	Set a timer for the current thread, if the thread
 *	is ready to wait.  Must be called between assert_wait()
 *	and thread_block().
 */
 
void
thread_set_timeout(
	int	t)	/* timeout interval in ticks */
{
	register thread_t	thread = current_thread();
	spl_t	s;

	s = splsched();
	thread_lock(thread);
	if ((thread->state & TH_WAIT) != 0) {
		set_timeout(&thread->timer, t);
	}
	thread_unlock(thread);
	splx(s);
}

/*
 * Set up thread timeout element when thread is created.
 */
void
thread_timeout_setup(
	register thread_t	thread)
{
	thread->timer.fcn = (timeout_fcn_t)thread_timeout;
	thread->timer.param = (void *)thread;
	thread->depress_timer.fcn = (timeout_fcn_t)thread_depress_timeout;
	thread->depress_timer.param = (void *)thread;
#if	NCPUS > 1
	thread->timer.bound_processor = PROCESSOR_NULL;
	thread->depress_timer.bound_processor = PROCESSOR_NULL;
#endif	/* NCPUS > 1 */
}

/*
 *	Routine:	thread_go
 *	Purpose:
 *		Start a thread running.
 *	Conditions:
 *		IPC locks may be held.
 */

void
thread_go(thread)
	thread_t thread;
{
	int	s, state;

	s = splsched();
	thread_lock(thread);

	reset_timeout_check(&thread->timer);

	/*
	 * #299 (SMP): thread_go() wakes a thread parked via thread_will_wait()
	 * (an IPC receiver on imq_threads), which never sets wait_event.  A
	 * thread instead queued on the event/mutex wait-hash has wait_event !=
	 * NO_EVENT; we must NOT clear its TH_WAIT, or we wake it out of a lock
	 * it has not acquired and leave a stale entry on the hash (which then
	 * makes thread_wakeup_prim panic finding it without TH_WAIT).  This
	 * happens when an IPC receiver is aborted (act_abort) but, before it
	 * removes itself from imq_threads, re-blocks on imq_lock; a concurrent
	 * ipc_mqueue_changed() then dequeues it from imq_threads and thread_go's
	 * it.  Skipping it here is safe: it is still TH_WAIT on the mutex hash
	 * and will be woken normally by the lock release, then sees ith_state
	 * and handles the change on its own re-run.  assert_wait sets wait_event
	 * under thread_lock too, so the check is race-free.
	 */
	if ((thread->state & TH_WAIT) && thread->wait_event == NO_EVENT) {
		thread->state &= ~TH_WAIT;
		if (!(thread->state & TH_RUN)) {
			thread->state |= TH_RUN;
#if	THREAD_SWAPPER
			if (thread->state & TH_SWAPPED_OUT)
				thread_swapin(thread->top_act, FALSE);
			else
#endif	/* THREAD_SWAPPER */
				thread_setrun(thread, TRUE, TAIL_Q);
		}
		thread->wait_result = THREAD_AWAKENED;
		thread->at_safe_point = NOT_AT_SAFE_POINT;
	}

	thread_unlock(thread);
	splx(s);
}

/*
 *	Routine:	thread_will_wait
 *	Purpose:
 *		Assert that the thread intends to block.
 */

void
thread_will_wait(thread)
	thread_t thread;
{
	int	s;

	s = splsched();
	thread_lock(thread);

	assert(thread->wait_result = -1);	/* for later assertions */
	thread->state |= TH_WAIT;

	thread_unlock(thread);
	splx(s);
}

/*
 *	Routine:	thread_will_wait_with_timeout
 *	Purpose:
 *		Assert that the thread intends to block,
 *		with a timeout.
 */

void
thread_will_wait_with_timeout(thread, msecs)
	thread_t thread;
	mach_msg_timeout_t msecs;
{
	unsigned int ticks = convert_ipc_timeout_to_ticks(msecs);
	int s;

	s = splsched();
	thread_lock(thread);

	assert(thread->wait_result = -1);	/* for later assertions */
	thread->state |= TH_WAIT;

	set_timeout(&thread->timer, ticks);

	thread_unlock(thread);
	splx(s);
}

void __assert_wait(event_t event, boolean_t interruptible, boolean_t first);
void assert_wait_first(event_t event, boolean_t interruptible);
/*
 *	assert_wait:
 *
 *	Assert that the current thread is about to go to
 *	sleep until the specified event occurs.
 */
__inline__ void
__assert_wait(
	event_t		event,
	boolean_t	interruptible,
	boolean_t	first)
{
	register queue_t	q;
	register int		index;
	register thread_t	thread;
	register simple_lock_t	lock;
	spl_t			s;

	thread = current_thread();
	if (!thread) {
		/*
		 * Issue #190: assert_wait was being reached during early
		 * machine_init (before thread_init / scheduler) when the
		 * vm_map_entry zone needed to grow recursively.  Bare
		 * assert(thread) made this almost untriagable; surface the
		 * event and the caller so future occurrences land closer to
		 * their root cause.
		 */
		/*
		 * #445: one return address, not three.
		 *
		 * __builtin_return_address(1) and (2) ask for the caller's
		 * caller, which the C standard does not define and GCC warns
		 * about: with a tail call, with inlining, or with the frame
		 * pointer omitted, that walk has nothing to walk and may
		 * return garbage or fault.  It worked here only because this
		 * kernel is built -fno-omit-frame-pointer -- an unrelated flag
		 * chosen for the backtracer, which anyone could reasonably
		 * change.  A diagnostic that lies when a build flag moves is
		 * worse than one that says less.
		 *
		 * Argument 0 is well-defined and stays.  The rest of the chain
		 * is not lost: panic() enters the debugger, and `trace` at that
		 * prompt walks the stack properly instead of guessing at it.
		 */
		panic("assert_wait: no current_thread "
		      "(event=%p ra=%p) — caller invoked sleep before "
		      "scheduler is up; use `trace` at the ddb prompt for the "
		      "rest of the chain",
		      event, __builtin_return_address(0));
	}
	if (thread->wait_event != NO_EVENT) {
		panic("assert_wait: already asserted event %p\n",
			thread->wait_event);
	}

	s = splsched();
	if (event != NO_EVENT) {
		index = wait_hash(event);
		q = &wait_queue[index];
		lock = simple_lock_addr(wait_lock[index]);

		simple_lock(lock);
		thread_lock(thread);
		if (first) {
			enqueue_head(q, (queue_entry_t) thread);
		} else {
			enqueue_tail(q, (queue_entry_t) thread);
		}
		thread->wait_event = event;
		if (interruptible)
			thread->state |= TH_WAIT;
		else
			thread->state |= TH_WAIT | TH_UNINT;
		thread->sleep_stamp = sched_tick;
		thread_unlock(thread);
		simple_unlock(lock);
	} else {
		thread_lock(thread);
		if (interruptible)
			thread->state |= TH_WAIT;
		else
			thread->state |= TH_WAIT | TH_UNINT;
		thread->sleep_stamp = sched_tick;
		thread_unlock(thread);
	}
	splx(s);
}

void
assert_wait(
	event_t		event,
	boolean_t	interruptible)
{
	__assert_wait(event, interruptible, FALSE);
}

void
assert_wait_first(
	event_t		event,
	boolean_t	interruptible)
{
	__assert_wait(event, interruptible, TRUE);
}

/*
 * thread_[un]stop(thread)
 *	Once a thread has blocked interruptibly (via assert_wait) prevent 
 *	it from running until thread_unstop.
 *
 * 	If someone else has already stopped the thread, wait for the
 * 	stop to be cleared, and then stop it again.
 *
 * 	Return FALSE if interrupted.
 *
 * NOTE: thread_hold/thread_suspend should be called on the activation
 *	before calling thread_stop.  TH_SUSP is only recognized when
 *	a thread blocks and only prevents clear_wait/thread_wakeup
 *	from restarting an interruptible wait.  The wake_active flag is
 *	used to indicate that someone is waiting on the thread.
 */
boolean_t
thread_stop( thread_t thread )
{
	spl_t s = splsched();
	wake_lock(thread);

	while( thread->state & TH_SUSP) {
		thread->wake_active = TRUE;
		assert_wait((event_t)&thread->wake_active, TRUE);
		wake_unlock(thread);
		splx(s);
		thread_block( (void (*)(void)) 0 );
		if( current_thread()->wait_result != THREAD_AWAKENED )
			return FALSE;
		s = splsched();
		wake_lock(thread);
	}
	thread_lock(thread);
	thread->state |= TH_SUSP;
	thread_unlock(thread);

	wake_unlock(thread);
	splx(s);
	return TRUE;
}

/*
 *	Clear TH_SUSP and if the thread has been stopped and is now runnable,
 *	put it back on the run queue.
 */
void
thread_unstop( thread_t thread )
{
	spl_t s = splsched();
	wake_lock(thread);
	thread_lock(thread);

	if ((thread->state & (TH_RUN|TH_WAIT|TH_SUSP/*|TH_UNINT*/)) == TH_SUSP){
		thread->state = (thread->state & ~TH_SUSP) | TH_RUN;
#if	THREAD_SWAPPER
		if (thread->state & TH_SWAPPED_OUT)
			thread_swapin(thread->top_act, FALSE);
		else
#endif	/* THREAD_SWAPPER */
			thread_setrun(thread, TRUE, TAIL_Q);
	} else if ( thread->state & TH_SUSP ) {
		thread->state &= ~TH_SUSP;
		if( thread->wake_active ) {
			thread->wake_active = FALSE;
			thread_unlock(thread);
			wake_unlock(thread);
			splx(s);
			thread_wakeup((event_t)&thread->wake_active);
			return;
		}
	}
	thread_unlock(thread);
	wake_unlock(thread);
	splx(s);
}

/*
 * Wait for the thread's RUN bit to clear
 */
boolean_t
thread_wait( thread_t thread )
{
    	spl_t s;

    	s = splsched();
    	wake_lock(thread);
	while( thread->state & (TH_RUN/*|TH_UNINT*/) ) {
#if	NCPUS > 1
		if (thread->last_processor != PROCESSOR_NULL) {
			cause_ast_check(thread->last_processor);
		}
#endif	/* NCPUS > 1 */
		thread->wake_active = TRUE;
		assert_wait((event_t)&thread->wake_active, TRUE);
		wake_unlock(thread);
		splx(s);
		thread_block( (void (*)(void)) 0 );
		if( current_thread()->wait_result != THREAD_AWAKENED )
			return FALSE;
		s = splsched();
		wake_lock(thread);
	}
	wake_unlock(thread);
	splx(s);
	return TRUE;
}


/*
 * thread_stop_wait(thread)
 *	Stop the thread then wait for it to block interruptibly
 */
boolean_t
thread_stop_wait( thread_t thread )
{
	if (thread_stop(thread)) {
		if (thread_wait(thread)) {
			return (TRUE);
		}
		thread_unstop(thread);
	}
	return (FALSE);
}


/*
 *	clear_wait_locked:
 *
 *	Clear the wait condition for the specified thread.  Start the thread
 *	executing if that is appropriate.  Called with raised splsched
 *	and with the thread locked.
 *
 *	parameters:
 *	  thread		thread to awaken
 *	  result		Wakeup result the thread should see
 *	  interrupt_only	Don't wake up the thread if it isn't
 *				interruptible.
 */
void
clear_wait_locked(
	register thread_t	thread,
	int			result,
	boolean_t		interrupt_only)
{
	register int		index;
	register queue_t	q;
	register event_t	event;
	register simple_lock_t	lock;

	if (interrupt_only && (thread->state & TH_UNINT)) {
		/*
		 *	can`t interrupt thread
		 */
		return;
	}

	event = thread->wait_event;

	/*
	** If the wait_event field is in the transitional state,
	** we're racing with someone in thread_wakeup_prim(),
	** who has unlocked the hash bucket lock, but hasn't yet
	** woken the thread.  We can just unlock and return.
	*/

	if (event == (event_t)WAKING_EVENT) {
		return;
	}

	if (event != NO_EVENT) {
		thread_unlock(thread);
		index = wait_hash(event);
		q = &wait_queue[index];
		lock = simple_lock_addr(wait_lock[index]);
		simple_lock(lock);
		/*
		 *	If the thread is still waiting on that event,
		 *	then remove it from the list.  If it is waiting
		 *	on a different event, or no event at all, then
		 *	someone else did our job for us.
		 */
		thread_lock(thread);
		if (thread->wait_event == event) {
			remqueue(q, (queue_entry_t)thread);
			thread->wait_event = NO_EVENT;
			event = NO_EVENT;		/* cause to run below */
		}
		simple_unlock(lock);
	}
	if (event == NO_EVENT) {
		register int	state = thread->state;

		reset_timeout_check(&thread->timer);

		switch (state & TH_SCHED_STATE) {
		    case	  TH_WAIT | TH_SUSP | TH_UNINT:
		    case	  TH_WAIT           | TH_UNINT:
		    case	  TH_WAIT:
			/*
			 *	Sleeping and not suspendable - put
			 *	on run queue.
			 */
			thread->state = (state &~ TH_WAIT) | TH_RUN;
			thread->wait_result = result;
			/***** this test should not BE HERE
			if (result != THREAD_INTERRUPTED)
			 *****/
				thread->at_safe_point = NOT_AT_SAFE_POINT;
#if	THREAD_SWAPPER
			if (thread->state & TH_SWAPPED_OUT)
				thread_swapin(thread->top_act, FALSE);
			else
#endif	/* THREAD_SWAPPER */
				thread_setrun(thread, TRUE, TAIL_Q);
			break;

		    case	  TH_WAIT | TH_SUSP :
		    case TH_RUN | TH_WAIT | TH_SUSP | TH_UNINT:
		    case TH_RUN | TH_WAIT	    | TH_UNINT:
		    case TH_RUN | TH_WAIT | TH_SUSP:
		    case TH_RUN | TH_WAIT:
			/*
			 *	Either already running, or suspended.
			 */
			thread->state = state &~ TH_WAIT;
			thread->wait_result = result;
			/***** this test should not BE HERE
			if (result != THREAD_INTERRUPTED)
			 *****/
				thread->at_safe_point = NOT_AT_SAFE_POINT;
			break;

		    default:
			/*
			 *	Not waiting.
			 */
			break;
		}
	}
}

#if	MACH_LDEBUG || MACH_KDB || DEBUG
void		log_thread_action (char *, long, long, long);
#endif

/*
 *	thread_wakeup_prim:
 *
 *	Common routine for thread_wakeup, thread_wakeup_with_result,
 *	and thread_wakeup_one.
 *
 */
void
thread_wakeup_prim(
	event_t		event,
	boolean_t	one_thread,
#if	MACH_LDEBUG
	int		result,
	boolean_t	debug)
#else
	int		result)
#endif /* MACH_LDEBUG */
{
	register queue_t	q;
	register int		index;
	register thread_t	thread, next_th;
	register simple_lock_t	lock;
	register int		state;
	queue_head_t		wake_queue;
	spl_t			s;

	index = wait_hash(event);
	q = &wait_queue[index];
	s = splsched();
	lock = simple_lock_addr(wait_lock[index]);

	simple_lock(lock);
	thread = (thread_t) queue_first(q);
	queue_init (&wake_queue);

#if	MACH_LDEBUG
	if (debug) {
		log_thread_action ("thread_wakeup - entry", (long)event, 0, 0);
	}
#endif

	while (!queue_end(q, (queue_entry_t)thread)) {
		next_th = (thread_t) queue_next((queue_t) thread);

		if (thread->wait_event == event) {
		        remqueue(q, (queue_entry_t) thread);
			enqueue (&wake_queue, (queue_entry_t) thread);
			thread->wait_event = (event_t)WAKING_EVENT;

			if (one_thread)
				break;
		}

		thread = next_th;
	}

	simple_unlock(lock);

	while (!queue_empty (&wake_queue)) {
		thread = (thread_t) dequeue (&wake_queue);

		thread_lock(thread);

#if	MACH_LDEBUG
		if (debug) {
			log_thread_action ("thread_wakeup - thread", (long)event,
					(long)thread, (long)(thread->state));
		}
#endif

		reset_timeout_check(&thread->timer);
		state = thread->state;
		switch (state & TH_SCHED_STATE) {

			    case          TH_WAIT | TH_SUSP | TH_UNINT:
			    case	  TH_WAIT	    | TH_UNINT:
			    case	  TH_WAIT:
				/*
				 *	Sleeping and not suspendable - put
				 *	on run queue.
				 */
				thread->state = (state &~ TH_WAIT) | TH_RUN;
				thread->wait_result = result;
				/***** this test should not BE HERE
				if (result != THREAD_INTERRUPTED)
				 *****/
				    thread->at_safe_point = NOT_AT_SAFE_POINT;
#if	THREAD_SWAPPER
				if (thread->state & TH_SWAPPED_OUT)
					thread_swapin(thread->top_act, FALSE);
				else
#endif	/* THREAD_SWAPPER */
					thread_setrun(thread, TRUE, TAIL_Q);
				break;

			    case TH_RUN | TH_WAIT | TH_SUSP | TH_UNINT:
			    case TH_RUN | TH_WAIT | TH_SUSP:
			    case	  TH_WAIT | TH_SUSP:
			    case TH_RUN | TH_WAIT:
			    case TH_RUN | TH_WAIT	    | TH_UNINT:
				/*
				 *	Either already running, or suspended.
				 */
				thread->state = state &~ TH_WAIT;
				thread->wait_result = result;
				/***** this test should not BE HERE
				if (result != THREAD_INTERRUPTED)
				 *****/
				    thread->at_safe_point = NOT_AT_SAFE_POINT;
				break;

			default:
				panic("thread_wakeup");
				break;
		}
		thread->wait_event = NO_EVENT;
		thread_unlock(thread);
	}
	splx(s);
}

/*
 *	thread_handoff_to_parked_waiter:
 *
 *	Futex (#324) direct hand-off.  Find one thread waiting on `event`
 *	and, if it is *fully parked* (its context is saved -- detectable
 *	because thread_dispatch() clears TH_RUN once the blocked thread has
 *	been switched away), switch directly to it via thread_invoke(),
 *	bypassing the run-queue + reschedule round-trip.  The caller must
 *	have already done assert_wait() on its own wait event, so the
 *	current thread parks here (keeping its stack) and resumes when later
 *	woken.
 *
 *	SMP-safe: a thread still in the assert_wait()->thread_block() window
 *	is TH_WAIT|TH_RUN (not yet parked); we must NOT switch to it (its
 *	saved context is stale), so we wake it the normal way instead.
 *
 *	Returns TRUE if the hand-off happened (current thread blocked and was
 *	later resumed).  Returns FALSE if no parked waiter was found (the
 *	caller should fall back to a normal block); a found-but-not-parked
 *	waiter is woken normally and FALSE is returned.
 */
boolean_t
thread_handoff_to_parked_waiter(
	event_t		event)
{
	register queue_t	q;
	register int		index;
	register thread_t	thread, next_th;
	register thread_t	self = current_thread();
	register simple_lock_t	lock;
	thread_t		victim = THREAD_NULL;
	boolean_t		parked;
	register int		ostate;
	spl_t			s;

	index = wait_hash(event);
	q = &wait_queue[index];
	s = splsched();
	lock = simple_lock_addr(wait_lock[index]);

	simple_lock(lock);
	thread = (thread_t) queue_first(q);
	while (!queue_end(q, (queue_entry_t)thread)) {
		next_th = (thread_t) queue_next((queue_t) thread);
		/* Skip self: with a same-word wake+wait the caller has just
		 * asserted_wait on this very key, so it is in this queue too. */
		if (thread->wait_event == event && thread != self) {
			remqueue(q, (queue_entry_t) thread);
			thread->wait_event = (event_t) WAKING_EVENT;
			victim = thread;
			break;
		}
		thread = next_th;
	}
	simple_unlock(lock);

	if (victim == THREAD_NULL) {
		splx(s);
		return FALSE;
	}

	thread_lock(victim);
	reset_timeout_check(&victim->timer);
	victim->wait_event = NO_EVENT;
	victim->wait_result = THREAD_AWAKENED;
	victim->at_safe_point = NOT_AT_SAFE_POINT;

	/* Snapshot the scheduling state BEFORE we stamp TH_RUN below: the
	 * not-parked path must know whether the victim was already running. */
	ostate = victim->state;

	/* Parked == TH_WAIT and nothing else (not running, suspended, etc.). */
	parked = ((ostate & (TH_WAIT|TH_SUSP|TH_RUN|TH_UNINT)) == TH_WAIT);
	victim->state = (ostate &~ TH_WAIT) | TH_RUN;

	if (!parked) {
		/*
		 * Victim is not cleanly parked.  Mirror clear_wait_internal():
		 * only a thread that is genuinely blocked (neither TH_RUN nor
		 * TH_SUSP set) may be handed to thread_setrun().  If the victim
		 * still has TH_RUN -- i.e. it is executing its own
		 * assert_wait()->thread_block() window on another CPU -- calling
		 * thread_setrun() here would dispatch a thread that is still
		 * running, executing it on two CPUs at once (#360: the futex
		 * ping-pong avalanched a single waiter onto up to 6 CPUs).
		 * Clearing TH_WAIT above is sufficient: when the still-running
		 * victim reaches thread_block() it sees itself runnable (TH_RUN,
		 * no TH_WAIT) and simply does not block.
		 */
		if ((ostate & (TH_RUN | TH_SUSP)) == 0)
			thread_setrun(victim, TRUE, TAIL_Q);
		thread_unlock(victim);
		splx(s);
		return FALSE;
	}
	thread_unlock(victim);

	/* Direct switch.  self (already TH_WAIT via the caller's assert_wait)
	 * is disposed -- and so parked -- by the victim's own post-switch
	 * thread_dispatch(); self resumes here when it is later woken. */
	thread_invoke(self, victim, 0);

	splx(s);
	return TRUE;
}

#if	NCPUS > 1
/*
 *	thread_bind:
 *
 *	Force a thread to execute on the specified processor.
 *	If the thread is currently executing, it may wait until its
 *	time slice is up before switching onto the specified processor.
 *
 *	A processor of PROCESSOR_NULL causes the thread to be unbound.
 *	xxx - DO NOT export this to users.
 */
void
thread_bind(
	register thread_t	thread,
	processor_t		processor)
{
	spl_t	s;

	s = splsched();
	thread_lock(thread);
	thread_bind_locked(thread,processor);
	thread_unlock(thread);
	splx(s);
}
#endif	/*NCPUS > 1*/

/*
 * #319 instrumentation: is the global pset->runq.lock actually contended /
 * interfered with?  Per-CPU (cache-line isolated) counters; rdtsc around the
 * lock acquire in thread_select.  Dumped every few seconds from sched_thread
 * (PROCESS context -- printing from hertz_tick at interrupt level nested
 * inside the BSP's own console writes and wedged the OMEGA 32-CPU bring-up),
 * and only for intervals with real scheduling activity so boot output stays
 * untouched.  setrun vs gq (global-runq enqueues) shows the saturation ratio
 * -- how many wakeups fall onto the GLOBAL runq vs land on an idle CPU.  The
 * dump zeroes the counters, so each printed line is one interval -> the
 * cycles/acquire form a time series across a concurrency sweep.
 *
 * Diagnostic tool, compiled out by default: build with
 * cmake -DUROS_S319_INSTRUMENT=ON when measuring scheduler behavior.
 * 32-core results (2026-07): lock uncontended (~0.2% of CPU time), but idle
 * CPUs full-speed-polling runq.count inflated every acquire (842 vs 445 cyc)
 * -> the idle-loop gcount sampling fix below.
 */
#ifndef	S319_INSTRUMENT
#define	S319_INSTRUMENT	0
#endif

#if	S319_INSTRUMENT
struct s319_stat {
	unsigned long long	psetlock_cyc;	/* cycles acquiring pset->runq.lock */
	unsigned long		psetlock_cnt;	/* # acquisitions (thread_select) */
	unsigned long		psetlock_max;	/* worst single acquire (cycles) */
	unsigned long		setrun;		/* unbound thread_setrun calls */
	unsigned long		psetenq;	/* enqueues onto the GLOBAL runq */
	unsigned long		localhoff;	/* #356 hand-off-on-block enqueues */
	char			pad[64];	/* isolate each entry to its own line */
} s319[NCPUS] __attribute__((aligned(64)));

static inline unsigned long long
s319_rdtsc(void)
{
	unsigned int	lo, hi;
	__asm__ volatile("rdtsc" : "=a" (lo), "=d" (hi));
	return ((unsigned long long) hi << 32) | lo;
}

void
s319_dump(void)
{
	unsigned long	sum_avg = 0, navg = 0, mx = 0;
	unsigned long	tot_cnt = 0, tot_setrun = 0, tot_enq = 0, tot_lq = 0;
	int		cpu;

	/*
	 * No 64-bit divide (no libgcc __udivdi3 in the kernel): average each
	 * CPU's cycles in 32 bits (a few seconds per-CPU fits) then mean them.
	 */
	for (cpu = 0; cpu < NCPUS; cpu++) {
		unsigned long	cnt = s319[cpu].psetlock_cnt;

		if (cnt != 0) {
			sum_avg += (unsigned long) s319[cpu].psetlock_cyc / cnt;
			navg++;
		}
		if (s319[cpu].psetlock_max > mx)
			mx = s319[cpu].psetlock_max;
		tot_cnt += cnt;
		tot_setrun += s319[cpu].setrun;
		tot_enq += s319[cpu].psetenq;
		tot_lq += s319[cpu].localhoff;
		s319[cpu].psetlock_cyc = 0;
		s319[cpu].psetlock_cnt = 0;
		s319[cpu].psetlock_max = 0;
		s319[cpu].setrun = 0;
		s319[cpu].psetenq = 0;
		s319[cpu].localhoff = 0;
	}

	/*
	 * Only report intervals with real load: keeps bring-up and idle
	 * screens clean (and the bench's own output mostly unmolested).
	 */
	if (tot_setrun < 1000)
		return;
	printf("s319: setrun=%lu lq=%lu gq=%lu acq=%lu avg=%lu max=%lu cyc/acq\n",
	       tot_setrun, tot_lq, tot_enq, tot_cnt,
	       navg ? sum_avg / navg : 0, mx);
}
#endif	/* S319_INSTRUMENT */

/*
 *	Select a thread for this processor (the current processor) to run.
 *	May select the current thread.
 *	Assumes splsched.
 */

thread_t
thread_select(
	register processor_t	myprocessor)
{
	register thread_t	thread;
	processor_set_t		pset;
#if	NCPUS > 1
	register run_queue_t	runq = &myprocessor->runq;
#endif	/* NCPUS > 1 */
	boolean_t		other_runnable;

	/*
	 *	Check for other non-idle runnable threads.
	 */
	myprocessor->first_quantum = TRUE;
	pset = myprocessor->processor_set;
	thread = current_thread();
	thread->unconsumed_quantum = myprocessor->quantum;

#if     NCPUS > 1
	simple_lock(&runq->lock);
#endif  /* NCPUS > 1 */
#if	S319_INSTRUMENT
	{
		unsigned long long	_s0, _s1;
		unsigned long		_sd;
		register int		_sc;

		_s0 = s319_rdtsc();
		simple_lock(&pset->runq.lock);
		_s1 = s319_rdtsc();
		_sd = (unsigned long) (_s1 - _s0);
		_sc = cpu_number();
		s319[_sc].psetlock_cyc += _sd;
		s319[_sc].psetlock_cnt++;
		if (_sd > s319[_sc].psetlock_max)
			s319[_sc].psetlock_max = _sd;
	}
#else
	simple_lock(&pset->runq.lock);
#endif

	other_runnable =
#if	NCPUS > 1
	    runq->count > 0 ||
#endif	/* NCPUS > 1 */
	    pset->runq.count > 0;

	if ((!other_runnable ||
#if	NCPUS > 1
	     runq->low > thread->sched_pri &&
#endif	/* NCPUS > 1 */
				pset->runq.low > thread->sched_pri
	    )		&&
#if	MACH_HOST
	    (thread->processor_set == pset) &&
#endif	/* MACH_HOST */
#if	NCPUS > 1
	    ((thread->bound_processor == PROCESSOR_NULL) ||
	     (thread->bound_processor == myprocessor)) &&
#endif	/* NCPUS > 1 */
	    (thread->state == TH_RUN)) {

		/* I am the highest priority runnable thread: */
		simple_unlock(&pset->runq.lock);
#if     NCPUS > 1
		simple_unlock(&runq->lock);
#endif  /* NCPUS > 1 */

		thread_lock(thread);
		if (thread->sched_stamp != sched_tick)
			update_priority(thread);
		thread_unlock(thread);
#if	NCPUS > 1
	} else if (other_runnable) {
		simple_unlock(&pset->runq.lock);
		simple_unlock(&runq->lock);
		thread = choose_thread(myprocessor);
#endif	/* NCPUS > 1 */
	} else {
#if     NCPUS > 1
		simple_unlock(&runq->lock);
#endif  /* NCPUS > 1 */

		/*
		 *	Nothing non-idle runnable, including myself.
		 *	Return if this
		 *	thread is still runnable on this processor.
		 *	Check for priority update if required.
		 */
		/* get an idle thread to run */
		thread = choose_pset_thread(myprocessor, pset);
	}
	if (thread->policy == POLICY_RR ||
	    thread->policy == POLICY_FIFO)
		myprocessor->quantum = thread->unconsumed_quantum;
	else
#if	NCPUS > 1
		myprocessor->quantum = (thread->bound_processor ?
					min_quantum : pset->set_quantum);
#else	/* NCPUS > 1 */
		myprocessor->quantum = pset->set_quantum;
#endif	/* NCPUS > 1 */
	return (thread);
}


/*
 *	Stop running the current thread and start running the new thread.
 *	If continuation is non-zero, and the current thread is blocked,
 *	then it will resume by executing continuation on a new stack.
 *	Returns TRUE if the hand-off succeeds.
 *	The reason parameter == AST_QUANTUM if the thread blocked
 *	because its quantum expired.
 *	Assumes splsched.
 */

mach_counter_t  c_thread_invoke_same;

void
thread_invoke(
	register thread_t	old_thread,
	register thread_t	new_thread,
	int			reason)
{
	etap_data_t	probe_data;

	/*
	 *	Mark thread interruptible.
	 */
	thread_lock(new_thread);
	new_thread->state &= ~TH_UNINT;

	/*
	 *	Check for invoking the same thread.
	 */
	if (old_thread == new_thread) {
		counter(++c_thread_invoke_same);
		thread_unlock(new_thread);
		return;
	}

	/*
	 *	Thread is now interruptible.
	 */
#if	NCPUS > 1
	mp_disable_preemption();
	new_thread->last_processor = current_processor();
	mp_enable_preemption();
#endif	/* NCPUS > 1 */

	thread_unlock(new_thread);

	ETAP_DATA_LOAD(probe_data[0], new_thread);
	ETAP_DATA_LOAD(probe_data[1], old_thread->etap_reason);
	ETAP_DATA_LOAD(probe_data[2], reason);
	ETAP_PROBE_DATA(ETAP_P_THREAD_CTX,
			EVENT_END,
			old_thread,
			&probe_data,
			ETAP_DATA_ENTRY*3);

	ETAP_SET_REASON(old_thread, BLOCKED_ON_CLEAR);

	/*
	 *	Set up ast context of new thread and switch to its timer.
	 */
	mp_disable_preemption();
	ast_context(new_thread->top_act, cpu_number());
	mp_enable_preemption();
	timer_switch(&new_thread->system_timer);

	/*
	 *	switch_context is machine-dependent.  It does the
	 *	machine-dependent components of a context-switch, like
	 *	changing address spaces.  It updates active_threads.
	 */
#if	MACH_ASSERT
	if (watchacts & WA_SWITCH) {
	    vm_offset_t stack = new_thread->kernel_stack;
	    printf("thread_invoke(old=%p,new=%p) \n",
			old_thread, new_thread);
	    printf("\tcurrent_thr=%p continuation = %p\n",
			current_thread(), new_thread->continuation);
	}
#endif	/* MACH_ASSERT */

	old_thread->reason = reason;
	counter_always(c_thread_invoke_csw++);

	/*
	 * N.B. On return from the call to switch_context, 'old_thread'
	 * points at the thread that yielded to us.  Unfortunately, at
	 * this point, there are no simple_locks held, so if we are preempted
	 * before the call to thread_dispatch blocks preemption, it is
	 * possible for 'old_thread' to terminate, leaving us with a
	 * stale thread pointer.
	 */

	disable_preemption();
	old_thread = switch_context(old_thread, old_thread->continuation,
				    new_thread);

	/*
	 *	We're back.  Now old_thread is the thread that resumed
	 *	us, and we have to dispatch it.
	 *	(Only reached when continuation was NULL — otherwise
	 *	 the thread resumes via Thread_continue/thread_continue.)
	 */
#if	MACH_RT
	if (old_thread->preempt == TH_NOT_PREEMPTABLE) {
	    /*
	     * Mark that we have been really preempted
	     */
	    old_thread->preempt = TH_PREEMPTED;
	}
#endif /* MACH_RT */
	thread_dispatch(old_thread);
	enable_preemption();

	ETAP_DATA_LOAD(probe_data[0], old_thread);
	ETAP_PROBE_DATA(ETAP_P_THREAD_CTX,
			EVENT_BEGIN,
			current_thread(),
			&probe_data,
			ETAP_DATA_ENTRY*1);
}

/*
 *	thread_continue:
 *
 *	Called when the launching a new thread, at splsched();
 */
void
thread_continue(
	register thread_t old_thread)
{
	register thread_t self = current_thread();
	register void (*continuation)(void) = self->continuation;

	/*
	 *	We must dispatch the old thread and then
	 *	call the current thread's continuation.
	 *	There might not be an old thread, if we are
	 *	the first thread to run on this processor.
	 */

	if (old_thread != THREAD_NULL)
		thread_dispatch(old_thread);
	self->at_safe_point = NOT_AT_SAFE_POINT;

	/*
	 * N.B. - the following is necessary, since thread_invoke()
	 * inhibits preemption on entry and reenables before it
	 * returns.  Unfortunately, the first time a newly-created
	 * thread executes, it magically appears here, and never
	 * executes the enable_preemption() call in thread_invoke().
	 */

	enable_preemption();
	spllo();

#if	MACH_ASSERT
	if (watchacts & WA_SCHED) {
		printf("thread_continue(old=%p) thr_self=%p, continuation=%p\n",
			old_thread, self, continuation);
	}
#endif	/* MACH_ASSERT */

	(*continuation)();
	/*NOTREACHED*/
}

#if	MACH_LDEBUG || MACH_KDB || DEBUG

#define THREAD_LOG_SIZE		300

struct {
	unsigned int	stamp;
	thread_t	thread;
	long		info1;
	long		info2;
	long		info3;
	char		* action;
} thread_log[THREAD_LOG_SIZE];

int		thread_log_index;

void		check_thread_time(long n);


int	check_thread_time_crash;

#if 0
void
check_thread_time(long us)
{
	struct t64	temp;

	if (!check_thread_time_crash)
		return;

	temp = thread_log[0].stamp;
	cyctm05_diff (&thread_log[1].stamp, &thread_log[0].stamp, &temp);

	if (temp.l >= us && thread_log[1].info != 0x49) /* HACK!!! */
		panic ("check_thread_time");
}
#endif

/*
 * 🔥 THE GUARD ON THE DEFINITION DID NOT COVER THE GUARD ON THE CALLERS (#415).
 *
 * This is compiled under `#if MACH_LDEBUG || MACH_KDB' -- see the block that
 * opens above -- and device/ds_routines.c calls it under `#if DEBUG'.  Two
 * conditions, never compared, and on i386 they happened to overlap because
 * that target builds with MACH_KDB=1.  x86-64 builds with MACH_KDB=0, so a
 * Debug configuration there compiled thirteen calls to a function nothing
 * defined and did not link.
 *
 * ⚠️ And the declarations disagreed as well: kern/clock.c said
 * `void (thread_t, char *)' against this `(char *, long, long, long)'.  C
 * compares nothing across translation units, and the linker was never asked
 * because no configuration that compiled the callers had ever been built.
 *
 * ⚠️ I first "fixed" this by adding a second definition in kern/debug.c,
 * having concluded from a grep truncated by `head' that there was none.  The
 * i386 Debug build said `multiple definition' immediately.  An enumeration
 * that has been cut short is not an enumeration.
 */
void
log_thread_action(char * action, long info1, long info2, long info3)
{
	int	i;
	spl_t	x;
	static  unsigned int tstamp;

#if 0
	if (!cyctm05_initialized)
		return;
#endif

	x = sploff();

	for (i = THREAD_LOG_SIZE-1; i > 0; i--) {
		thread_log[i] = thread_log[i-1];
	}

#if 0
	do {
		cyctm05_stamp_masked (&thread_log[0].stamp);

	} while ((thread_log[0].stamp.l & 0x0000ffff) == 0);
#else
	thread_log[0].stamp = tstamp++;
#endif
	thread_log[0].thread = current_thread();
	thread_log[0].info1 = info1;
	thread_log[0].info2 = info2;
	thread_log[0].info3 = info3;
	thread_log[0].action = action;
/*	strcpy (&thread_log[0].action[0], action);*/

	splon(x);
}
#endif /* MACH_LDEBUG || MACH_KDB */

#if	MACH_KDB
#include <ddb/db_output.h>
void		db_show_thread_log(void);

void
db_show_thread_log(void)
{
	int	i;

	db_printf ("%s %s %s %s %s %s\n", " Thread ", "  Info1 ", "  Info2 ",
			"  Info3 ", "    Timestamp    ", "Action");

	for (i = 0; i < THREAD_LOG_SIZE; i++) {
		db_printf ("%08x %08x %08x %08x %08x %s\n",
			thread_log[i].thread,
			thread_log[i].info1,
			thread_log[i].info2,
			thread_log[i].info3,
			thread_log[i].stamp,
			thread_log[i].action);
	}
}
#endif	/* MACH_KDB */

/*
 *	thread_block_reason:
 *
 *	Block the current thread.  If the thread is runnable
 *	then someone must have woken it up between its request
 *	to sleep and now.  In this case, it goes back on a
 *	run queue.
 *
 *	If a continuation is specified, then thread_block will
 *	attempt to discard the thread's kernel stack.  When the
 *	thread resumes, it will execute the continuation function
 *	on a new kernel stack.
 */

counter(mach_counter_t  c_thread_block_calls = 0;)
 
#if     FAST_IDLE
unsigned int    fast_idle_enabled = 0;
counter(mach_counter_t  c_thread_block_run_hit = 0;)
counter(mach_counter_t  c_thread_block_run_miss = 0;)
counter(mach_counter_t  c_thread_block_fast_idle_rejected = 0;)
#endif  /* FAST_IDLE */

void
thread_block_reason(
	void	(*continuation)(void),
	int	reason)
{
	register thread_t thread = current_thread();
	register processor_t myprocessor;
	register thread_t new_thread;
	spl_t		s;
	int             ast_flags;
	register int	aborted;
#if	FAST_IDLE
	int		fast_idle_allowed = 1;
#endif	/* FAST_IDLE */

	counter(++c_thread_block_calls);

	check_simple_locks();

	s = splsched();

	mp_disable_preemption();
	myprocessor = current_processor();

	thread_lock(thread);
	/*
	 * Distinguish safe-point sentinels from real continuations.
	 * Sentinels (SAFE_*) are small negative values, so as addresses
	 * they sit in the last SAFE_POINT_SENTINEL_MAX bytes of the
	 * address space, where no function can be: store in
	 * at_safe_point, zero the continuation (it's not a real function
	 * pointer).  Real continuations are valid kernel addresses: the
	 * caller must pre-set at_safe_point before calling
	 * thread_block().  NULL means no continuation and no safe-point.
	 */
	if ((vm_offset_t)continuation >= (vm_offset_t)-SAFE_POINT_SENTINEL_MAX) {
		/* Sentinel: use for at_safe_point, zero continuation */
		thread->at_safe_point = (int)(long) continuation;
		continuation = (void (*)(void)) 0;
	} else if (continuation == (void (*)(void)) 0) {
		thread->at_safe_point = NOT_AT_SAFE_POINT;
	}
	/* else: real continuation — at_safe_point pre-set by caller */

	aborted = (thread->state & TH_ABORT);
	if( aborted )
		clear_wait_locked( thread, THREAD_INTERRUPTED, TRUE );
	thread_unlock(thread);

#if	FAST_TAS
	{
		extern int recover_ras();

		if (csw_needed(thread, myprocessor))
			recover_ras(thread);
	}
#endif	/* FAST_TAS */

	/* Unconditionally remove either | both */
	ast_off(cpu_number(), (AST_QUANTUM|AST_BLOCK|AST_URGENT));

      restart: (void)&&restart; /* suppress unused-label: goto inside #if FAST_IDLE */
	new_thread = thread_select(myprocessor);
	assert(new_thread);

#if	FAST_IDLE
	/*
	 *	If the thread yielding the processor is "important",
	 *	i.e., not idle or depressed, and is about to switch
	 *	to an un-important thread, then short-cut instead
	 *	into the idle loop, in the context of the current
	 *	thread.  That way, the current thread can be "awoken"
	 *	and restored to a fully-running condition very quickly,
	 *	without any context switch overhead or latency.
	 *	However, only try this short-cut once per thread_block,
	 *	because there are still good reasons to wind up in the
	 *	real idle thread.
	 */
	/* FAST_IDLE is incompatible with continuations (stack is discarded) */
	if (continuation == (void (*)(void)) 0 &&
	    new_thread->priority >= DEPRESSPRI &&
	    thread->priority < DEPRESSPRI &&
	    fast_idle_enabled &&
	    fast_idle_allowed) {
		/*
		 *	Re-enqueue the newly-selected thread so that
		 *	it remains eligible to run.  The thread priority
		 *	checks, above, as a side-effect guarantee that
		 *	we don't have to worry about the case where
		 *	thread_select specifies the same thread to run again.
		 */
		if (new_thread->state & TH_IDLE) {
#if	NCPUS > 1
			myprocessor->processor_set->idle_count--;
#else	/* NCPUS > 1 */
			default_pset.idle_count--;
#endif	/* NCPUS > 1 */
			myprocessor->state = PROCESSOR_RUNNING;
		} else {
			thread_lock(new_thread);
			thread_setrun(new_thread, FALSE, TAIL_Q);
			thread_unlock(new_thread);
		}
		spllo();
		fast_idle_allowed = idle_thread_loop(FAKE_IDLE_THREAD);
		/*
		 *	Taking thread lock to peek at these bits
		 *	can't eliminate a race; so don't bother.
		 */
		if ((thread->state & (TH_WAIT|TH_SUSP|TH_RUN)) == TH_RUN) {
			/*
			 *	Caller may have asked to block
			 *	uninterruptibly, and the current
			 *	scheduler algorithms don't always
			 *	clear this bit when waking a thread
			 *	in the weird state we were just in
			 *	(RUN/WAIT/SUSP).
			 */
			if (thread->state & TH_UNINT) {
				thread_lock(thread);
				thread->state &= ~TH_UNINT;
				thread_unlock(thread);
			}
			counter(++c_thread_block_run_hit);
			splx(s);
			mp_enable_preemption();
			return;
		} else {
			counter(++c_thread_block_run_miss);
			counter(fast_idle_allowed ? 0 :
				++c_thread_block_fast_idle_rejected);
			(void) splsched();
			goto restart;
		}
	}
#endif	/* FAST_IDLE */

	mp_enable_preemption();

	thread->continuation = continuation;
	thread_invoke(thread, new_thread, reason);

	splx(s);
}

/*
 *	thread_block:
 *
 *	Now calls thread_block_reason() which forwards the
 *	the reason parameter to thread_invoke() so it can
 *	do the right thing if the thread's quantum expired.
 */
void
thread_block(
	void	(*continuation)(void))
{
	thread_block_reason(continuation, 0);
}

/*
 *	thread_run:
 *
 *	Switch directly from the current thread to a specified
 *	thread.  Both the current and new threads must be
 *	runnable.
 */
void
thread_run(
	void			(*continuation)(void),
	register thread_t	new_thread)
{
	register thread_t thread = current_thread();
	spl_t	s;

#if	MACH_ASSERT
	if (watchacts & WA_SWITCH)
		printf("thread_run(cont=%p,thr=%p) self=%p\n",
			continuation, new_thread, thread);
#endif	/* MACH_ASSERT */

	s = splsched();
	thread_lock(thread);
	/* Apply same sentinel detection as thread_block_reason */
	if ((vm_offset_t)continuation >= (vm_offset_t)-SAFE_POINT_SENTINEL_MAX) {
		thread->at_safe_point = (int)(long) continuation;
		continuation = (void (*)(void)) 0;
	} else if (continuation == (void (*)(void)) 0) {
		thread->at_safe_point = NOT_AT_SAFE_POINT;
	}
	thread_unlock(thread);
	thread->continuation = continuation;
	thread_invoke(thread, new_thread, 0);
	splx(s);

#if	MACH_ASSERT
	if (watchacts & WA_SWITCH)
		printf("\tthread_run BACK AS thr=%p\n", current_thread());
#endif	/* MACH_ASSERT */
}


/*
 *	Dispatches a running thread that is not	on a runq.
 *	Called at splsched.
 */

void
thread_dispatch(
	register thread_t	thread)
{
	/*
	 *	If we are discarding the thread's stack, we must do it
	 *	before the thread has a chance to run.
	 */

#if	MACH_ASSERT
	if (watchacts & WA_SWITCH)
		printf("\tthread_dispatch(thr=%p)\n", thread);
#endif	/* MACH_ASSERT */

	/*
	 * #331 step 2: primary QSBR quiescent point.  We run here as the new
	 * thread just after a context switch, so this CPU has passed through a
	 * point with no active RCU reader (a reader never blocks/switches).
	 */
	urmach_rcu_quiescent_state();

	wake_lock(thread);
	thread_lock(thread);

	/* thread->continuation may be non-zero if the thread blocked
	 * with a real continuation (it will resume via Thread_continue) */

	switch (thread->state & (TH_RUN|TH_WAIT|TH_UNINT|TH_IDLE)) {
	    case TH_RUN			    | TH_UNINT:
	    case TH_RUN:
		/*
		 *	No reason to stop.  Put back on a run queue.
		 */
	
		if (thread->reason & AST_QUANTUM) {
			thread_setrun(thread, FALSE, TAIL_Q);
			thread->unconsumed_quantum = thread->sched_data;
		}
		else {
			thread_setrun(thread, FALSE, HEAD_Q);
		}
		break;

	    case TH_RUN | TH_WAIT	    | TH_UNINT:
	    case TH_RUN | TH_WAIT:
		thread->sleep_stamp = sched_tick;
		/* fallthrough */
	    case	  TH_WAIT:			/* this happens! */
	
		/*
		 *	Waiting
		 */
		thread->state &= ~TH_RUN;
		if (thread->wake_active) {
		    thread->wake_active = FALSE;
			thread_unlock(thread);
		    wake_unlock(thread);
		    thread_wakeup((event_t)&thread->wake_active);
		    return;
		}
		break;

	    case TH_RUN | TH_IDLE:
		/*
		 *	Drop idle thread -- it is already in
		 *	idle_thread_array.
		 */
		break;

	    default:
		panic("State 0x%x \n",thread->state);
	}
	thread_unlock(thread);
	wake_unlock(thread);
}

/*
 *	Define shifts for simulating (5/8)**n
 */

shift_data_t	wait_shift[32] = {
	{1,1},{1,3},{1,-3},{2,-7},{3,5},{3,-5},{4,-8},{5,7},
	{5,-7},{6,-10},{7,10},{7,-9},{8,-11},{9,12},{9,-11},{10,-13},
	{11,14},{11,-13},{12,-15},{13,17},{13,-15},{14,-17},{15,19},{16,18},
	{16,-19},{17,22},{18,20},{18,-20},{19,26},{20,22},{20,-22},{21,-27}};

/*
 *	do_priority_computation:
 *
 *	Calculate new priority for thread based on its base priority plus
 *	accumulated usage.  PRI_SHIFT and PRI_SHIFT_2 convert from
 *	usage to priorities.  SCHED_SHIFT converts for the scaling
 *	of the sched_usage field by SCHED_SCALE.  This scaling comes
 *	from the multiplication by sched_load (thread_timer_delta)
 *	in sched.h.  sched_load is calculated as a scaled overload
 *	factor in compute_mach_factor (mach_factor.c).
 */
#ifdef	PRI_SHIFT_2
#if	PRI_SHIFT_2 > 0
#define do_priority_computation(th, pri)				\
	MACRO_BEGIN							\
	(pri) = (th)->priority	/* start with base priority */		\
	    + ((th)->sched_usage >> (PRI_SHIFT + SCHED_SHIFT))		\
	    + ((th)->sched_usage >> (PRI_SHIFT_2 + SCHED_SHIFT));	\
	if ((pri) > LPRI) (pri) = LPRI;					\
	MACRO_END
#else	/* PRI_SHIFT_2 */
#define do_priority_computation(th, pri)				\
	MACRO_BEGIN							\
	(pri) = (th)->priority	/* start with base priority */		\
	    + ((th)->sched_usage >> (PRI_SHIFT + SCHED_SHIFT))		\
	    - ((th)->sched_usage >> (SCHED_SHIFT - PRI_SHIFT_2));	\
	if ((pri) > LPRI) (pri) = LPRI;					\
	MACRO_END
#endif	/* PRI_SHIFT_2 */
#else	/* defined(PRI_SHIFT_2) */
#define do_priority_computation(th, pri)				\
	MACRO_BEGIN							\
	(pri) = (th)->priority	/* start with base priority */		\
	    + ((th)->sched_usage >> (PRI_SHIFT + SCHED_SHIFT));		\
	if ((pri) > LPRI) (pri) = LPRI;					\
	MACRO_END
#endif	/* defined(PRI_SHIFT_2) */

/*
 *	compute_priority:
 *
 *	Compute the effective priority of the specified thread.
 *	The effective priority computation is as follows:
 *
 *	Take the base priority for this thread and add
 *	to it an increment derived from its cpu_usage.
 *
 *	The thread *must* be locked by the caller. 
 */

void
compute_priority(
	register thread_t	thread,
        boolean_t		resched)
{
	register int	pri;
	
	pri = 0;

	if (thread->policy == POLICY_TIMESHARE) {
	    do_priority_computation(thread, pri);
	    if (thread->depress_priority < 0)
		set_pri(thread, pri, resched);
	    else
		thread->depress_priority = pri;
	}
	else {
	    set_pri(thread, thread->priority, resched);
	}
}

/*
 *	compute_my_priority:
 *
 *	Version of compute priority for current thread or thread
 *	being manipulated by scheduler (going on or off a runq).
 *	Only used for priority updates.  Policy or priority changes
 *	must call compute_priority above.  Caller must have thread
 *	locked and know it is timesharing and not depressed.
 */

void
compute_my_priority(
	register thread_t	thread)
{
	register int temp_pri;

	do_priority_computation(thread,temp_pri);
	assert(thread->runq == RUN_QUEUE_NULL);
	thread->sched_pri = temp_pri;
}

/*
 *	recompute_priorities:
 *
 *	Update the priorities of all threads periodically.
 */
void
recompute_priorities(void)
{
	register thread_t	thread;

#if	SIMPLE_CLOCK
	int			new_usec;
#endif	/* SIMPLE_CLOCK */

	sched_tick++;		/* age usage one more time */
	set_timeout(&recompute_priorities_timer, hz);
#if	SIMPLE_CLOCK
	/*
	 *	Compensate for clock drift.  sched_usec is an
	 *	exponential average of the number of microseconds in
	 *	a second.  It decays in the same fashion as cpu_usage.
	 */
	new_usec = sched_usec_elapsed();
	sched_usec = (5*sched_usec + 3*new_usec)/8;
#endif	/* SIMPLE_CLOCK */
	/*
	 *	Wakeup scheduler thread.
	 */
	thread = sched_thread_id;
	if (thread != THREAD_NULL) {
		spl_t s = splsched();
		thread_lock(thread);
		clear_wait_locked(thread, THREAD_AWAKENED, FALSE);
		if (sched_tick - thread->sched_stamp > 1) {
			if (sched_tick - thread->sched_stamp > 1) {
				update_priority(thread);
			}
		}
		thread_unlock(thread);
		splx(s);
	}
}

/*
 *	update_priority
 *
 *	Cause the priority computation of a thread that has been 
 *	sleeping or suspended to "catch up" with the system.  Thread
 *	*MUST* be locked by caller.  If thread is running, then this
 *	can only be called by the thread on itself.
 */
void
update_priority(
	register thread_t	thread)
{
	register unsigned int	ticks;
	register shift_t	shiftp;
	register int		temp_pri;

	ticks = sched_tick - thread->sched_stamp;
	assert(ticks != 0);

	/*
	 *	If asleep for more than 30 seconds forget all
	 *	cpu_usage, else catch up on missed aging.
	 *	5/8 ** n is approximated by the two shifts
	 *	in the wait_shift array.
	 */
	thread->sched_stamp += ticks;
	thread_timer_delta(thread);
	if (ticks >  30) {
		thread->cpu_usage = 0;
		thread->sched_usage = 0;
	}
	else {
		thread->cpu_usage += thread->cpu_delta;
		thread->sched_usage += thread->sched_delta;
		shiftp = &wait_shift[ticks];
		if (shiftp->shift2 > 0) {
		    thread->cpu_usage =
			(thread->cpu_usage >> shiftp->shift1) +
			(thread->cpu_usage >> shiftp->shift2);
		    thread->sched_usage =
			(thread->sched_usage >> shiftp->shift1) +
			(thread->sched_usage >> shiftp->shift2);
		}
		else {
		    thread->cpu_usage =
			(thread->cpu_usage >> shiftp->shift1) -
			(thread->cpu_usage >> -(shiftp->shift2));
		    thread->sched_usage =
			(thread->sched_usage >> shiftp->shift1) -
			(thread->sched_usage >> -(shiftp->shift2));
		}
	}
	thread->cpu_delta = 0;
	thread->sched_delta = 0;

	/*
	 *	Recompute priority if appropriate.
	 */
	if (
	    (thread->policy == POLICY_TIMESHARE) &&
	    (thread->depress_priority < 0)) {
		run_queue_t	rq;

		do_priority_computation(thread, temp_pri);
		rq = rem_runq(thread);
		thread->sched_pri = temp_pri;
		if (rq != RUN_QUEUE_NULL)
			thread_setrun(thread, TRUE, TAIL_Q);
	}
}

/*
 * Enqueue thread on run_queue.
 */
int
run_queue_enqueue(
	register run_queue_t	rq,
	register thread_t	th,
	boolean_t		tail)
{
	register int	whichq;
	int oldrqcount;

	whichq = (th)->sched_pri;
	if (whichq < 0 || whichq > MINPRI) {
		panic("run_queue_enqueue: bad pri (%d)\n", whichq);
	}

	simple_lock(&(rq)->lock);	/* lock the run queue */
#if	DEBUG
	checkrq((rq), "run_queue_enqueue: before adding thread");
#endif	/* DEBUG */
	assert(th->runq == RUN_QUEUE_NULL);
	if ( tail ) {
		enqueue_tail(&(rq)->runq[whichq], (queue_entry_t) (th));
	} else {
		enqueue_head(&(rq)->runq[whichq], (queue_entry_t) (th));
	}
	setbit(whichq, (rq)->bitmap);
	if (whichq < (rq)->low) {
		(rq)->low = whichq;
	}
	oldrqcount = (rq)->count++;
	if (whichq == DEPRESSPRI)
	    (rq)->depress_count++;
	(th)->runq = (rq);
	(th)->whichq = whichq;
#if	DEBUG
	thread_check((th), (rq));
	checkrq((rq), "run_queue_enqueue: after adding thread");
#endif	/* DEBUG */
	simple_unlock(&(rq)->lock);
	return( oldrqcount );
}

#if	NCPUS > 1
/*
 * #356: run-time gate for the synchronous-RPC hand-off-on-block placement
 * (see the hint consumption in thread_setrun).  Off by default; enabled by
 * the -P boot argument for same-binary A/B on hardware.
 *
 * The hand-off also stays dormant until EVERY CPU has passed cpu_up()
 * (machine_info.avail_cpus reaches real_ncpus -- guaranteed on any boot
 * that completes, since start_other_cpus barriers on all APs): shifting
 * bench wakeup placement DURING bring-up widened a latent window where a
 * slave-init AP (no current_thread yet) kernel-faults into a contended map
 * mutex and panics in assert_wait ("sleep before scheduler is up", seen on
 * OMEGA cpu 20).  Placement only matters under load, so keep it off until
 * bring-up is done.
 */
/* .data, NOT bss: parse_arguments() runs BEFORE i386_init()'s BSS clear, so a
 * zero-initialised flag would be silently wiped back to 0 after -P set it --
 * the exact #337 trap (see halt_in_debugger in model_dep.c). */
int	sched_rpc_handoff __attribute__((section(".data"))) = 0;
extern int	real_ncpus;
#endif	/* NCPUS > 1 */

/*
 *	thread_setrun:
 *
 *	Make thread runnable; dispatch directly onto an idle processor
 *	if possible.  Else put on appropriate run queue (processor
 *	if bound, else processor set.  Caller must have lock on thread.
 *	This is always called at splsched.
 *	The tail parameter, if TRUE || TAIL_Q, indicates that the 
 *	thread should be placed at the tail of the runq. If 
 *	FALSE || HEAD_Q the thread will be placed at the head of the 
 *      appropriate runq.
 */
void
thread_setrun(
	register thread_t	th,
	boolean_t		may_preempt,
	boolean_t		tail)
{
	register processor_t	processor;
	register run_queue_t	rq;
	thread_t		cur_th;
#if	NCPUS > 1
	register processor_set_t	pset;
#endif	/* NCPUS > 1 */
	ast_t			ast_flags = AST_QUANTUM;

	mp_disable_preemption();

	assert(! (th->state & TH_SWAPPED_OUT));

	/*
	 *	Update priority if needed.
	 */
	if (th->sched_stamp != sched_tick) {
		update_priority(th);
	}

	assert(th->runq == RUN_QUEUE_NULL);

#if MACH_RT
        if ((th->policy == POLICY_FIFO || th->policy == POLICY_RR) &&
			th->priority < BASEPRI_SYSTEM) {
		ast_flags |= AST_URGENT;
#if 0
		log_thread_action ((long)th, "thread_setrun");
#endif
	}
#endif /* MACH_RT */

#if	NCPUS > 1
	/*
	 *	Try to dispatch the thread directly onto an idle processor.
	 */
	if ((processor = th->bound_processor) == PROCESSOR_NULL) {
	    /*
	     *	Not bound, any processor in the processor set is ok.
	     */
	    pset = th->processor_set;
#if	S319_INSTRUMENT
	    s319[cpu_number()].setrun++;
#endif

	    /*
	     * #356 hand-off on block: the waker flagged that it is about to
	     * block (combined mach_msg send phase -- the same syscall proceeds
	     * to the receive and sleeps).  A synchronous RPC pair is
	     * inherently serial, so instead of shipping the wakee to a remote
	     * idle CPU (cold caches, wake-from-idle latency -- the measured
	     * 32-core pre-saturation collapse) queue it on THIS cpu's local
	     * runq: thread_select picks it up the moment the waker blocks, on
	     * the one CPU where its data is warm.  Same-task only (an
	     * inter-task switch on one CPU costs a cr3 reload = full TLB flush
	     * on i386, measured worse than the split).  One-shot: consumed by
	     * the first wakeup of the send.  Runtime-gated by -P boot arg.
	     */
	    if (sched_rpc_handoff &&
		machine_info.avail_cpus >= real_ncpus) {
		register thread_t	self = current_thread();

		/* #370: self is THREAD_NULL on an early-AP that has not yet
		 * installed its current thread; nothing to hand off then. */
		if (self != THREAD_NULL && self->handoff_hint) {
			self->handoff_hint = FALSE;
			if (
#if	MACH_HOST
			    pset == current_processor()->processor_set &&
#endif	/* MACH_HOST */
			    self->top_act != THR_ACT_NULL &&
			    th->top_act != THR_ACT_NULL &&
			    self->top_act->task == th->top_act->task) {
#if	S319_INSTRUMENT
				s319[cpu_number()].localhoff++;
#endif
				(void) run_queue_enqueue(
					&current_processor()->runq, th, tail);
				mp_enable_preemption();
				return;
			}
		}
	    }

#if	HW_FOOTPRINT
	    /*
	     *	But first check the last processor it ran on.
	     */
	    processor = th->last_processor;
	    if (processor->state == PROCESSOR_IDLE) {
		    simple_lock(&processor->lock);
		    simple_lock(&pset->idle_lock);
		    if ((processor->state == PROCESSOR_IDLE)
#if	MACH_HOST
			&& (processor->processor_set == pset)
#endif	/* MACH_HOST */
			) {
			    queue_remove(&pset->idle_queue, processor,
			        processor_t, processor_queue);
			    pset->idle_count--;
			    processor->next_thread = th;
			    processor->state = PROCESSOR_DISPATCHING;
			    simple_unlock(&pset->idle_lock);
			    simple_unlock(&processor->lock);
#if	POWER_SAVE
			    machine_idle_wake(processor->slot_num);
#endif	/* POWER_SAVE */
			    mp_enable_preemption();
		            return;
		    }
		    simple_unlock(&pset->idle_lock);
		    simple_unlock(&processor->lock);
	    }
#endif	/* HW_FOOTPRINT */

	    if (pset->idle_count > 0) {
		simple_lock(&pset->idle_lock);
		if (pset->idle_count > 0) {
		    processor = (processor_t) queue_first(&pset->idle_queue);
		    queue_remove(&(pset->idle_queue), processor, processor_t,
				processor_queue);
		    pset->idle_count--;
		    processor->next_thread = th;
		    processor->state = PROCESSOR_DISPATCHING;
		    simple_unlock(&pset->idle_lock);
#if	POWER_SAVE
		    machine_idle_wake(processor->slot_num);
#endif	/* POWER_SAVE */
		    mp_enable_preemption();
		    return;
		}
		simple_unlock(&pset->idle_lock);
	    }
	    rq = &(pset->runq);

	    /*
	     * Preempt check
	     */
	    processor = current_processor();
	    if (may_preempt &&
#if	MACH_HOST
		(pset == processor->processor_set) &&
#endif	/* MACH_HOST */
		/* #370: an early-AP can enter thread_setrun before its current
		 * thread is installed -- a periodic tick or resched IPI slipping
		 * past the bring-up splhigh() in slave_machine_init.  Nothing is
		 * running to preempt and current_thread() is THREAD_NULL, so the
		 * old unconditional deref read NULL+0x58 (sched_pri): the cr2=0x58
		 * early-AP page fault caught on OMEGA E-cores (cpu 25).  Skip it. */
		current_thread() != THREAD_NULL &&
		(current_thread()->sched_pri > th->sched_pri)) {

		    /*
		     * XXX if we have a non-empty local runq or are
		     * XXX running a bound thread, ought to check for
		     * XXX another cpu running lower-pri thread to preempt.
		     *
		     *	Turn off first_quantum to allow csw.
		     */
		if (processor->state == PROCESSOR_DISPATCHING) {
		    cur_th = processor->next_thread;
		    processor->next_thread = th;
		    th = cur_th;
		} else {
		    processor->first_quantum = FALSE;
		    ast_on(cpu_number(), ast_flags);
		}
	    }
#if	S319_INSTRUMENT
	    s319[cpu_number()].psetenq++;
#endif
	    (void)run_queue_enqueue(rq, th, tail);
	}
	else {
	    /*
	     *	Bound, can only run on bound processor.  Have to lock
	     *  processor here because it may not be the current one.
	     */
	    if (processor->state == PROCESSOR_IDLE) {
		simple_lock(&processor->lock);
		pset = processor->processor_set;
		simple_lock(&pset->idle_lock);
		if (processor->state == PROCESSOR_IDLE) {
		    queue_remove(&pset->idle_queue, processor,
			processor_t, processor_queue);
		    pset->idle_count--;
		    processor->next_thread = th;
		    processor->state = PROCESSOR_DISPATCHING;
		    simple_unlock(&pset->idle_lock);
		    simple_unlock(&processor->lock);
#if	POWER_SAVE
		    machine_idle_wake(processor->slot_num);
#endif	/* POWER_SAVE */
		    mp_enable_preemption();
		    return;
		}
		simple_unlock(&pset->idle_lock);
		simple_unlock(&processor->lock);
	    }

	    /*
	     * Cause ast on processor if processor is on line, and the
	     * currently executing thread is not bound to that processor
	     * (bound threads have implicit priority over non-bound threads).
	     * We also avoid sending the AST to the idle thread (if it got
	     * scheduled in the window between the 'if' above and here),
	     * since the idle_thread is bound.
	     */
	    rq = &(processor->runq);
	    if (processor == current_processor()) {
		/* #370: same early-AP guard as the unbound path -- no current
		 * thread means nothing to preempt (bound-thread case). */
		if (current_thread() != THREAD_NULL &&
		    (current_thread()->bound_processor == PROCESSOR_NULL ||
		     current_thread()->sched_pri > th->sched_pri)) {
		    if (processor->state == PROCESSOR_DISPATCHING) {
			cur_th = processor->next_thread;
			processor->next_thread = th;
			th = cur_th;
		    } else {
			processor->first_quantum = FALSE;
			ast_on(cpu_number(), ast_flags);
		    }
		}
		(void)run_queue_enqueue(rq, th, tail);

	    } else if (run_queue_enqueue(rq, th, tail) == 0 &&
		       processor->state != PROCESSOR_OFF_LINE &&
		       (th = cpu_data[processor->slot_num].active_thread) &&
		       (th->bound_processor != processor)) {
#if 0
		/*
		 * Using ast_on() for another processor is illegal.
		 * We could pass some flags to cause_ast_check()
		 * to have it send more information about the ASTs we
		 * want to raise and let it handle it...
		 */
		ast_on(processor->slot_num, ast_flags);
#endif
		cause_ast_check(processor);
	    }
	}
#else	/* NCPUS > 1 */
	/*
	 *	XXX should replace queue with a boolean in this case.
	 */
	if (default_pset.idle_count > 0) {
	    processor = (processor_t) queue_first(&default_pset.idle_queue);
	    assert(processor == current_processor());
	    queue_remove(&default_pset.idle_queue, processor,
		processor_t, processor_queue);
	    default_pset.idle_count--;
	    processor->next_thread = th;
	    processor->state = PROCESSOR_DISPATCHING;
	    return;
	}

	rq = &(default_pset.runq);
	if (may_preempt) {
	    /*
	     * Preempt check
	     */
	    processor = current_processor();
	    if (processor->state == PROCESSOR_DISPATCHING) {
		/*
		 *	A second thread is to be awaken while dispatching
		 */
		if (processor->next_thread->sched_pri > th->sched_pri) {
		    cur_th = processor->next_thread;
		    processor->next_thread = th;
		    th = cur_th;
		}
	    } else if (current_thread()->sched_pri > th->sched_pri) {
		/*
		 *	Turn off first_quantum to allow context switch.
		 */
		current_processor()->first_quantum = FALSE;
		ast_on(cpu_number(), ast_flags);
	    }
	}
	(void) run_queue_enqueue(rq, th, tail);
#endif	/* NCPUS > 1 */
	mp_enable_preemption();
}

/*
 *	set_pri:
 *
 *	Set the priority of the specified thread to the specified
 *	priority.  This may cause the thread to change queues.
 *
 *	The thread *must* be locked by the caller.
 */

void
set_pri(
	thread_t	th,
	int		pri,
	boolean_t	resched)
{
	register struct run_queue	*rq;

	rq = rem_runq(th);
	assert(th->runq == RUN_QUEUE_NULL);
	th->sched_pri = pri;
	if (rq != RUN_QUEUE_NULL) {
	    if (resched)
		thread_setrun(th, TRUE, TAIL_Q);
	    else
		(void)run_queue_enqueue(rq, th, TAIL_Q);
	}
}

/*
 *	rem_runq:
 *
 *	Remove a thread from its run queue.
 *	The run queue that the process was on is returned
 *	(or RUN_QUEUE_NULL if not on a run queue).  Thread *must* be locked
 *	before calling this routine.  Unusual locking protocol on runq
 *	field in thread structure makes this code interesting; see thread.h.
 */

run_queue_t
rem_runq(
	thread_t	th)
{
	register struct run_queue	*rq;

	rq = th->runq;
	/*
	 *	If rq is RUN_QUEUE_NULL, the thread will stay out of the
	 *	run_queues because the caller locked the thread.  Otherwise
	 *	the thread is on a runq, but could leave.
	 */
	if (rq != RUN_QUEUE_NULL) {
#if DEBUG
		thread_t	t;
		int		whichq;
#endif
		simple_lock(&rq->lock);
#if	DEBUG
		checkrq(rq, "rem_runq: at entry");
#endif	/* DEBUG */
		if (rq == th->runq) {
			/*
			 *	Thread is in a runq and we have a lock on
			 *	that runq.
			 */
#if	DEBUG
			checkrq(rq, "rem_runq: before removing thread");
			thread_check(th, rq);
#endif	/* DEBUG */
			remqueue(&rq->runq[0], (queue_entry_t) th);
			rq->count--;
#if	DEBUG
			/* find in which queue the thread was */
			for (t = (thread_t) th->links.next;
			     (queue_head_t *) t < &rq->runq[0] ||
			     (queue_head_t *) t > &rq->runq[MINPRI];
			     t = (thread_t) t->links.next);
			whichq = ((queue_head_t *) t) - &rq->runq[0];
			if (whichq != th->sched_pri) {
				panic("rem_runq: whichq %d != sched_pri %d\n",
				      whichq, th->sched_pri);
			}
#endif	/* DEBUG */
			if (th->whichq == DEPRESSPRI)
			    rq->depress_count--;

			if (queue_empty(rq->runq + th->sched_pri)) {
				/* update run queue status */
				clrbit(th->sched_pri, rq->bitmap);
				rq->low = ffsbit(rq->bitmap);
			}
#if	DEBUG
			checkrq(rq, "rem_runq: after removing thread");
#endif	/* DEBUG */
			th->runq = RUN_QUEUE_NULL;
			simple_unlock(&rq->lock);
		}
		else {
			/*
			 *	The thread left the runq before we could
			 * 	lock the runq.  It is not on a runq now, and
			 *	can't move again because this routine's
			 *	caller locked the thread.
			 */
			assert(th->runq == RUN_QUEUE_NULL);
			simple_unlock(&rq->lock);
			rq = RUN_QUEUE_NULL;
		}
	}

	return(rq);
}

#if	NCPUS > 1
/*
 *	choose_thread:
 *
 *	Choose a thread to execute.  The thread chosen is removed
 *	from its run queue.  Note that this requires only that the runq
 *	lock be held.
 *
 *	Strategy:
 *		Check processor runq first; if anything found, run it.
 *		Else check pset runq; if nothing found, return idle thread.
 *
 *	Second line of strategy is implemented by choose_pset_thread.
 *	This is only called on processor startup and when thread_block
 *	thinks there's something in the processor runq.
 */

thread_t
choose_thread(
	processor_t		myprocessor)
{
	thread_t		th;
	register queue_t	q;
	register run_queue_t	runq;
	processor_set_t		pset;

	runq = &myprocessor->runq;
	pset = myprocessor->processor_set;

	simple_lock(&runq->lock);
	if (runq->count > 0 && runq->low <= pset->runq.low) {
		q = runq->runq + runq->low;
#if	MACH_ASSERT
		if (!queue_empty(q)) {
#endif	/*MACH_ASSERT*/
			th = (thread_t)q->next;
			((queue_entry_t)th)->next->prev = q;
			q->next = ((queue_entry_t)th)->next;
			th->runq = RUN_QUEUE_NULL;
			runq->count--;
			if (th->whichq == DEPRESSPRI)
			    runq->depress_count--;
			if (queue_empty(q)) {
				clrbit(runq->low, runq->bitmap);
				runq->low = ffsbit(runq->bitmap);
			}
			simple_unlock(&runq->lock);
			return(th);
#if	MACH_ASSERT
		}
		panic("choose_thread");
#endif	/*MACH_ASSERT*/
		/*NOTREACHED*/
	}
	simple_unlock(&runq->lock);
	simple_lock(&pset->runq.lock);
	return(choose_pset_thread(myprocessor, pset));
}
#endif	/*NCPUS > 1*/

/*
 *	choose_pset_thread:  choose a thread from processor_set runq or
 *		set processor idle and choose its idle thread.
 *
 *	Caller must be at splsched and have a lock on the runq.  This
 *	lock is released by this routine.  myprocessor is always the current
 *	processor, and pset must be its processor set.
 *	This routine chooses and removes a thread from the runq if there
 *	is one (and returns it), else it sets the processor idle and
 *	returns its idle thread.
 */

thread_t
choose_pset_thread(
	register processor_t	myprocessor,
	processor_set_t		pset)
{
	register run_queue_t	runq;
	register thread_t	th;
	register queue_t	q;

	runq = &pset->runq;
	if (runq->count > 0) {
		q = runq->runq + runq->low;
#if	MACH_ASSERT
		if (!queue_empty(q)) {
#endif	/*MACH_ASSERT*/
			th = (thread_t)q->next;
			((queue_entry_t)th)->next->prev = q;
			q->next = ((queue_entry_t)th)->next;
			th->runq = RUN_QUEUE_NULL;
			runq->count--;
			if (th->whichq == DEPRESSPRI)
			    runq->depress_count--;
			if (queue_empty(q)) {
				clrbit(runq->low, runq->bitmap);
				runq->low = ffsbit(runq->bitmap);
			}
			simple_unlock(&runq->lock);
			return(th);
#if	MACH_ASSERT
		}
		panic("choose_pset_thread");
#endif	/*MACH_ASSERT*/
		/*NOTREACHED*/
	}
	simple_unlock(&runq->lock);

	/*
	 *	Nothing is runnable, so set this processor idle if it
	 *	was running.  If it was in an assignment or shutdown,
	 *	leave it alone.  Return its idle thread.
	 */
	simple_lock(&pset->idle_lock);
	if (myprocessor->state == PROCESSOR_RUNNING) {
	    myprocessor->state = PROCESSOR_IDLE;
	    /*
	     *	XXX Until it goes away, put master on end of queue, others
	     *	XXX on front so master gets used last.
	     */
	    if (myprocessor == master_processor) {
		queue_enter(&(pset->idle_queue), myprocessor,
			processor_t, processor_queue);
	    }
	    else {
		queue_enter_first(&(pset->idle_queue), myprocessor,
			processor_t, processor_queue);
	    }

	    pset->idle_count++;
	}
	simple_unlock(&pset->idle_lock);
	return(myprocessor->idle_thread);

#undef	pset
}

/*
 *	no_dispatch_count counts number of times processors go non-idle
 *	without being dispatched.  This should be very rare.
 */
int	no_dispatch_count = 0;

/*
 *	This is the idle thread, which just looks for other threads
 *	to execute.
 */

#if	FAST_IDLE
counter(mach_counter_t	c_idle_thread_loop_calls = 0;)
counter(mach_counter_t	c_idle_thread_fast = 0;)
counter(mach_counter_t	c_idle_thread_fast_thd_exits = 0;)
counter(mach_counter_t	c_idle_thread_fast_thd_state_exits = 0;)
counter(mach_counter_t	c_idle_thread_fast_def_runq = 0;)
counter(mach_counter_t	c_idle_thread_fast_myproc_runq = 0;)
counter(mach_counter_t	c_idle_thread_fast_proc_running = 0;)
counter(mach_counter_t	c_idle_thread_fast_proc_dispatch = 0;)
counter(mach_counter_t	c_idle_thread_fast_proc_idle = 0;)
counter(mach_counter_t	c_idle_thread_fast_proc_assign = 0;)
counter(mach_counter_t	c_idle_thread_fast_proc_shutdown = 0;)
#endif	/* FAST_IDLE */

#if	FAST_IDLE
void
idle_thread_continue(void)
{
	(void) idle_thread_loop(REAL_IDLE_THREAD);
	panic("idle_thread_continue:  real idle loop returned!");
}
#endif	/* FAST_IDLE */


#if	FAST_IDLE
/*
 *	When doing a fast idle (idle_thread_type == FAKE_IDLE_THREAD),
 *	return an indication to the caller whether further fast_idle
 *	activity is permitted on this thread's trip through the
 *	scheduler.  Non-zero means further fast_idle is permitted,
 *	zero means that the caller should immediately fall back to
 *	the deafult idle behavior.
 */
int
idle_thread_loop(int idle_thread_type)
#else	/* FAST_IDLE */
void
idle_thread_continue(void)
#endif	/* FAST_IDLE */
{
	register processor_t myprocessor;
	register volatile thread_t *threadp;
	register volatile int *gcount;
#if	NCPUS > 1
	register volatile int *lcount;
#endif	/*NCPUS > 1*/
	register thread_t new_thread;
	register int state;
	int	mycpu;
	spl_t	s;
	register unsigned int idle_passes = 0;	/* #319 gcount sampling */
#if	FAST_IDLE
	register volatile int *thread_statep;
	register volatile int *depress_countp;
#if	NCPUS > 1
	register volatile int *depress2_countp;
#endif	/* NCPUS > 1 */
#endif	/* FAST_IDLE */
	extern spl_t    curr_ipl[];
	thread_t	idle_self = current_thread();

	mycpu = cpu_number();
	myprocessor = current_processor();
	threadp = (volatile thread_t *) &myprocessor->next_thread;
#if	NCPUS > 1
	lcount = (volatile int *) &myprocessor->runq.count;
#endif	/*NCPUS > 1*/
#if	FAST_IDLE
	thread_statep = (volatile int *) &(current_thread()->state);
	counter(++c_idle_thread_loop_calls);
	counter(idle_thread_type==FAKE_IDLE_THREAD ? ++c_idle_thread_fast : 0);
#if	NCPUS > 1
	depress2_countp = (volatile int *) &myprocessor->runq.depress_count;
#endif	/* NCPUS > 1 */
#endif	/* FAST_IDLE */

	while (TRUE) {
#if 0
		if (curr_ipl[cpu_number()])
			panic ("Idle thread at spl > 0?");
#endif
		/*
		 * #331 step 2: an idle CPU holds no RCU read reference, so
		 * report a quiescent state each pass -- lets grace periods
		 * finish promptly on idle CPUs without relying solely on the
		 * clock tick.
		 */
		urmach_rcu_quiescent_state();

#ifdef	MARK_CPU_IDLE
		MARK_CPU_IDLE(mycpu);
#endif	/* MARK_CPU_IDLE */

#if	MACH_HOST
		gcount = (volatile int *)
				&myprocessor->processor_set->runq.count;
#if	FAST_IDLE
		depress_countp = (volatile int *)
			&myprocessor->processor_set->runq.depress_count;
#endif	/* FAST_IDLE */
#else	/* MACH_HOST */
		gcount = (volatile int *) &default_pset.runq.count;
#if	FAST_IDLE
		depress_countp = (volatile int *)
			&default_pset.runq.depress_count;
#endif	/* FAST_IDLE */
#endif	/* MACH_HOST */


/*
 *	This cpu will be dispatched (by thread_setrun) by setting next_thread
 *	to the value of the thread to run next.  Also check runq counts.
 */
		/*
		 * #331 step 2: this CPU is about to wait for work -- mark it
		 * quiescent for QSBR for the whole idle span (cleared once it
		 * gets a thread to run below).
		 */
		urmach_rcu_idle_enter();

		s = splsched();
		while ((*threadp == (volatile thread_t)THREAD_NULL) &&
#if	FAST_IDLE
		       (idle_thread_type == REAL_IDLE_THREAD ||
			*thread_statep & TH_WAIT)
		       && (*gcount <= *depress_countp)
#if	NCPUS > 1
		       && (*lcount <= *depress2_countp)
#endif	/* NCPUS > 1 */
#else	/* FAST_IDLE */
		       /*
		        * #319: sample the SHARED pset->runq.count only every
		        * 64th pass.  N idle CPUs spinning full speed on that
		        * one global line invalidation-storm every runq write
		        * by the busy CPUs (measured on 32-core bare metal:
		        * pset lock acquire avg 842 cyc with ~24 idle pollers
		        * vs 445 with none).  next_thread/local runq stay
		        * full-rate, so idle-dispatch latency is untouched;
		        * only the rare enqueue-to-global-while-idle race
		        * waits up to ~64 passes (a few microseconds).
		        */
		       ((idle_passes++ & 63) != 0 || *gcount == 0)
#if	NCPUS > 1
		       && (*lcount == 0)
#endif	/*NCPUS > 1*/
#endif	/* FAST_IDLE */
		       ) {

			/* check for ASTs while we wait */

			if (need_ast[mycpu] &~ AST_SCHEDULING) {
				/* don't allow scheduling ASTs */
				need_ast[mycpu] &= ~AST_SCHEDULING;
				ast_taken(FALSE, AST_ALL, s
#if	FAST_IDLE
					  ,idle_thread_type
#endif	/* FAST_IDLE */
					  );
				/* back at spllo */
			}
			else
				splx(s);

			/* #319: spin-wait hint (rep;nop) -- eases the memory
			 * pipeline and the SMT sibling while idle-polling. */
			__asm__ volatile("pause");

			/*
			 * machine_idle is a machine dependent function,
			 * to conserve power.
			 */
#if	POWER_SAVE
			machine_idle(mycpu);
#endif /* POWER_SAVE */
			s = splsched();
		}

		/* #331 step 2: leaving idle -- about to run a real thread. */
		urmach_rcu_idle_exit();

#if	POWER_SAVE
		/* #357: idle stint over -- reset the HLT grace window. */
		machine_idle_exit(mycpu);
#endif	/* POWER_SAVE */

#ifdef	MARK_CPU_ACTIVE
		splx(s);
		MARK_CPU_ACTIVE(mycpu);
		s = splsched();
#endif	/* MARK_CPU_ACTIVE */

		/*
		 *	This is not a switch statement to avoid the
		 *	bounds checking code in the common case.
		 *	The PROCESSOR_IDLE branch may re-loop here if state
		 *	changes under us.
		 */
	    for (;;) {
		state = myprocessor->state;
		if (state == PROCESSOR_DISPATCHING) {
			/*
			 *	Commmon case -- cpu dispatched.
			 */
			new_thread = (thread_t) *threadp;
			*threadp = (volatile thread_t) THREAD_NULL;
			myprocessor->state = PROCESSOR_RUNNING;
			/*
			 *	set up quantum for new thread.
			 */
			if (new_thread->policy == POLICY_TIMESHARE) {
				/*
				 *  Just use set quantum.  No point in
				 *  checking for shorter local runq quantum;
				 *  csw_needed will handle correctly.
				 */
#if	MACH_HOST
				myprocessor->quantum = new_thread->
					processor_set->set_quantum;
#else	/* MACH_HOST */
				myprocessor->quantum =
					default_pset.set_quantum;
#endif	/* MACH_HOST */
			}
			else {
				/*
				 *	POLICY_RR || POLICY_FIFO
				 */
				myprocessor->quantum = new_thread->unconsumed_quantum;
			}
			myprocessor->first_quantum = TRUE;
#if	FAST_IDLE
			if (idle_thread_type == FAKE_IDLE_THREAD) {
				if (new_thread == current_thread())
					panic("new_thread 0x%x !FAKE",
					      new_thread);
				counter(++c_idle_thread_fast_proc_dispatch);
				return 1;
			}
#endif	/* FAST_IDLE */
			counter(c_idle_thread_handoff++);
	
			ETAP_SET_REASON(idle_self, BLOCKED_ON_IDLE_DONE);
			thread_run((void(*)(void))0, new_thread);
		}
#if	FAST_IDLE
		else if (state == PROCESSOR_RUNNING) {
			/*
			 *	We have something to run.  Return to
			 *	caller to sort this out.
			 */
			if (idle_thread_type != FAKE_IDLE_THREAD)
				panic("idle_thread_loop:  RUNNING but FAKE");

			counter(++c_idle_thread_fast_proc_running);
			counter((*thread_statep & TH_WAIT) ? 0 :
				++c_idle_thread_fast_thd_state_exits);
			counter((*threadp != (volatile thread_t)THREAD_NULL) ?
				++c_idle_thread_fast_thd_exits : 0);
			counter((*gcount > *depress_countp) ?
				++c_idle_thread_fast_def_runq : 0);
#if	NCPUS > 1
			counter((*lcount > *depress2_countp) ?
				++c_idle_thread_fast_myproc_runq : 0);
#endif	/* NCPUS > 1 */
			return 1;
		}
#endif	/* FAST_IDLE */
		else if (state == PROCESSOR_IDLE) {
			register processor_set_t pset;

			pset = myprocessor->processor_set;
			simple_lock(&pset->idle_lock);
			if (myprocessor->state != PROCESSOR_IDLE) {
				/*
				 *	Something happened, try again.
				 */
				simple_unlock(&pset->idle_lock);
				continue;
			}
			/*
			 *	Processor was not dispatched (Rare).
			 *	Set it running again.
			 */
			no_dispatch_count++;
			pset->idle_count--;
			queue_remove(&pset->idle_queue, myprocessor,
				processor_t, processor_queue);
			myprocessor->state = PROCESSOR_RUNNING;
			simple_unlock(&pset->idle_lock);
			counter(c_idle_thread_block++);
#if	FAST_IDLE
			if (idle_thread_type == FAKE_IDLE_THREAD) {
				counter(++c_idle_thread_fast_proc_idle);
				/*
				 *	Rare case:  out of pure paranoia,
				 *	fall back to non-fast idle.
				 */
				return 0;
			}
#endif	/* FAST_IDLE */

			ETAP_SET_REASON(idle_self, BLOCKED_ON_IDLE_DONE);
			thread_block((void(*)(void))0);
		}
		else if ((state == PROCESSOR_ASSIGN) ||
			 (state == PROCESSOR_SHUTDOWN)) {
			/*
			 *	Changing processor sets, or going off-line.
			 *	Release next_thread if there is one.  Actual
			 *	thread to run in on a runq.
			 */
			if ((new_thread = (thread_t)*threadp)!= THREAD_NULL) {
				*threadp = (volatile thread_t) THREAD_NULL;
				thread_setrun(new_thread, FALSE, TAIL_Q);
			}

#if	FAST_IDLE
			if (idle_thread_type == FAKE_IDLE_THREAD) {
				counter(state == PROCESSOR_ASSIGN ?
					++c_idle_thread_fast_proc_assign : 0);
				counter(state == PROCESSOR_SHUTDOWN ?
					++c_idle_thread_fast_proc_shutdown : 0);
				/*
				 *	These cases are a little strange.
				 *	Make sure that the next attempt
				 *	to reschedule uses a real idle
				 *	thread, and doesn't allow fast idle.
				 */
				return 0;
			}
#endif	/* FAST_IDLE */
			counter(c_idle_thread_block++);

			ETAP_SET_REASON(idle_self, BLOCKED_ON_IDLE_DONE);
			thread_block((void(*)(void))0);
		}
		else {
			printf(" Bad processor state %d (Cpu %d)\n",
				cpu_state(mycpu), mycpu);
			panic("idle_thread");
		}
		break;	/* unreachable, all branches above return or panic */
	    } /* end for(;;) retry */

		splx(s);
	}
}

void
idle_thread(void)
{
	register thread_t	self = current_thread();
	spl_t			s;

	stack_privilege(self);
	thread_swappable(current_act(), FALSE);

	s = splsched();
	self->priority = IDLEPRI;
	self->sched_pri = IDLEPRI;
	self->policy = POLICY_RR;

	/*
	 *	Set the idle flag to indicate that this is an idle thread,
	 *	enter ourselves in the idle array, and thread_block() to get
	 *	out of the run queues (and set the processor idle when we
	 *	run next time).
	 */
	thread_lock(self);
	self->state |= TH_IDLE;
	current_processor()->idle_thread = self;
	thread_unlock(self);
	splx(s);

	counter(c_idle_thread_block++);
	thread_block((void(*)(void))0);
	idle_thread_continue();
	/*NOTREACHED*/

	panic("idle_thread_continue!");
}

/*
 *	sched_thread: scheduler thread.
 *
 *	This thread handles periodic calculations in the scheduler that
 *	we don't want to do at interrupt level.  This allows us to
 *	avoid blocking 
 */
void
sched_thread(void)
{
    sched_thread_id = current_thread();
    thread_swappable(current_act(), FALSE);

    /*
     *	Sleep on event NO_EVENT, recompute_priorities() will awaken
     *	us by calling clear_wait().
     */
#if	MACH_ASSERT
    if (watchacts & WA_BOOT)
		printf("sched_thread RUNNING\n");
#endif	/* MACH_ASSERT */

    assert_wait((event_t) 0, FALSE);
    counter(c_sched_thread_block++);
    thread_block((void(*)(void))0);

    while (TRUE) {
	(void) compute_mach_factor();
#if	TASK_SWAPPER
	if (task_swap_on)
		compute_vm_averages();
#endif	/* TASKS_SWAPPER */

#if	S319_INSTRUMENT
	/* #319 TEMP: report runq-lock contention every ~5s, process context. */
	{
		static unsigned int	s319_beats;

		if (++s319_beats >= 5) {
			s319_beats = 0;
			s319_dump();
		}
	}
#endif

	/*
	 *	Check for stuck threads.  This can't be done off of
	 *	the callout queue because it requires operations that
	 *	can't be used from interrupt level.
	 */
	if (sched_tick & 1)
	    	do_thread_scan();

	assert_wait((event_t) 0, FALSE);
	counter(c_sched_thread_block++);
	thread_block((void(*)(void))0);
    }
    /*NOTREACHED*/
}

#define	MAX_STUCK_THREADS	64

/*
 *	do_thread_scan: scan for stuck threads.  A thread is stuck if
 *	it is runnable but its priority is so low that it has not
 *	run for several seconds.  Its priority should be higher, but
 *	won't be until it runs and calls update_priority.  The scanner
 *	finds these threads and does the updates.
 *
 *	Scanner runs in two passes.  Pass one squirrels likely
 *	thread ids away in an array  (takes out references for them).
 *	Pass two does the priority updates.  This is necessary because
 *	the run queue lock is required for the candidate scan, but
 *	cannot be held during updates [set_pri will deadlock].
 *
 *	Array length should be enough so that restart isn't necessary,
 *	but restart logic is included.  Does not scan processor runqs.
 *
 */

thread_t		stuck_threads[MAX_STUCK_THREADS];
int			stuck_count = 0;

/*
 *	do_runq_scan is the guts of pass 1.  It scans a runq for
 *	stuck threads.  A boolean is returned indicating whether
 *	it ran out of space.
 */

boolean_t
do_runq_scan(
	run_queue_t	runq)
{
	spl_t			s;
	register queue_t	q;
	register thread_t	thread;
	register int		count;

	s = splsched();
	simple_lock(&runq->lock);
	if((count = runq->count) > 0) {
	    q = runq->runq + runq->low;
	    while (count > 0) {
		queue_iterate(q, thread, thread_t, links) {
		    if ((thread->state & (TH_WAIT|TH_SUSP)) == 0 &&
			sched_tick - thread->sched_stamp > 1) {
			    /*
			     *	Stuck, save its id for later.
			     */
			    if (stuck_count == MAX_STUCK_THREADS) {
				/*
				 *	!@#$% No more room.
				 */
				simple_unlock(&runq->lock);
				splx(s);
				return(TRUE);
			    }
			    /*
			     *	Inline version of thread_reference
			     * XXX - lock ordering problem here:
			     * thread locks should be taken before runq
			     * locks: just try and get the thread's locks
			     * and ignore this thread if we fail, we might
			     * have better luck next time.
			     */
			    if (simple_lock_try(&thread->lock)) {
				    thread->ref_count++;
				    thread_unlock(thread);
				    stuck_threads[stuck_count++] = thread;
			    }
		    }
		    count--;
		}
		q++;
	    }
	}
	simple_unlock(&runq->lock);
	splx(s);

	return(FALSE);
}

void
do_thread_scan(void)
{
	spl_t			s;
	register boolean_t	restart_needed = 0;
	register thread_t	thread;
#if	MACH_HOST
	register processor_set_t	pset;
#endif	/* MACH_HOST */

	do {
#if	MACH_HOST
	    mutex_lock(&all_psets_lock);
	    queue_iterate(&all_psets, pset, processor_set_t, all_psets) {
		if (restart_needed = do_runq_scan(&pset->runq))
			break;
	    }
	    mutex_unlock(&all_psets_lock);
#else	/* MACH_HOST */
	    restart_needed = do_runq_scan(&default_pset.runq);
#endif	/* MACH_HOST */
#if	NCPUS > 1
	    if (!restart_needed)
	    	restart_needed = do_runq_scan(&master_processor->runq);
#endif	/*NCPUS > 1*/

	    /*
	     *	Ok, we now have a collection of candidates -- fix them.
	     */

	    while (stuck_count > 0) {
		thread = stuck_threads[--stuck_count];
		stuck_threads[stuck_count] = THREAD_NULL;
		s = splsched();
		thread_lock(thread);
		if ((thread->state & (TH_WAIT|TH_SUSP)) == 0 &&
		    sched_tick - thread->sched_stamp > 1) {
			update_priority(thread);
		}
		thread_unlock(thread);
		splx(s);
		thread_deallocate(thread);
	    }

	} while (restart_needed);
}
		
/*
 *	Just in case someone doesn't use the macro
 */
#undef	thread_wakeup
void
thread_wakeup(
	event_t		x);

void
thread_wakeup(
	event_t		x)
{
	thread_wakeup_with_result(x, THREAD_AWAKENED);
}

void
dump_processor_set(
	processor_set_t	ps)
{
    printf("processor_set: %p\n",ps);
    printf("idle_queue: %p %p, idle_count:      0x%x\n",
	ps->idle_queue.next,ps->idle_queue.prev,ps->idle_count);
    printf("processors: %p %p, processor_count: 0x%x, empty: %x\n",
	ps->processors.next,ps->processors.prev,ps->processor_count,ps->empty);
    printf("tasks:      %p %p, task_count:      0x%x\n",
	ps->tasks.next,ps->tasks.prev,ps->task_count);
    printf("threads:    %p %p, thread_count:    0x%x\n",
	ps->threads.next,ps->threads.prev,ps->thread_count);
    printf("ref_count: 0x%x, all_psets: %p %p, active: %x\n",
	ps->ref_count, ps->all_psets.next,ps->all_psets.prev,ps->active);
    printf("pset_self: %p, pset_name_self: %p\n",ps->pset_self, ps->pset_name_self);
    printf("max_priority: 0x%x, policies: 0x%x, set_quantum: 0x%x\n",
	ps->max_priority, ps->policies, ps->set_quantum);
}

#define processor_state(s) (((s)>PROCESSOR_VIDLE)?"*unknown*":states[s])

void
dump_processor(
	processor_t	p)
{
    char *states[]={"OFF_LINE","RUNNING","IDLE","DISPATCHING",
		   "ASSIGN","SHUTDOWN","VIDLE"};

    printf("processor: %p\n",p);
    printf("processor_queue: %p %p\n",
	p->processor_queue.next,p->processor_queue.prev);
    printf("state: %8s, next_thread: %p, idle_thread: %p\n",
	processor_state(p->state), p->next_thread, p->idle_thread);
    printf("quantum: %u, first_quantum: %x, last_quantum: %u\n",
	p->quantum, p->first_quantum, p->last_quantum);
    printf("processor_set: %p, processor_set_next: %p\n",
	p->processor_set, p->processor_set_next);
    printf("processors: %p %p\n", p->processors.next,p->processors.prev);
    printf("processor_self: %p, slot_num: 0x%x\n", p->processor_self, p->slot_num);
}

void
dump_run_queue_struct(
	run_queue_t	rq)
{
    char dump_buf[80];
    int i;

    for( i=0; i < NRQS; ) {
        int j;

	printf("%6s",(i==0)?"runq:":"");
	for( j=0; (j<8) && (i < NRQS); j++,i++ ) {
	    if( rq->runq[i].next == &rq->runq[i] )
		printf( " --------");
	    else
		printf(" %p",rq->runq[i].next);
	}
	printf("\n");
    }
    for( i=0; i < NRQBM; ) {
        register unsigned int mask;
	char *d=dump_buf;

	mask = ~0;
	mask ^= (mask>>1);

	do {
	    *d++ = ((rq->bitmap[i]&mask)?'r':'e');
	    mask >>=1;
	} while( mask );
	*d = '\0';
	printf("%8s%s\n",((i==0)?"bitmap:":""),dump_buf);
	i++;
    }	
    printf("low: 0x%x, count: %u\n",rq->low,rq->count);
}
 
void
dump_run_queues(
	run_queue_t	runq)
{
	register queue_t	q1;
	register int		i;
	register queue_entry_t	e;

	q1 = runq->runq;
	for (i = 0; i < NRQS; i++) {
	    if (q1->next != q1) {
		int t_cnt;

		printf("[%u]",i);
		for (t_cnt=0, e = q1->next; e != q1; e = e->next) {
		    printf("\t%p",e);
		    if( (t_cnt = (t_cnt + 1) % 4) == 0 )
			printf("\n");
		}
		if( t_cnt )
			printf("\n");
	    }
	    /* else
		printf("[%u]\t<empty>\n",i);
	     */
	    q1++;
	}
}

#if	DEBUG

void
checkrq(
	run_queue_t	rq,
	char		*msg)
{
	register queue_t	q1;
	register int		i, j;
	register queue_entry_t	e;
	register int		low;

	low = -1;
	j = 0;
	q1 = rq->runq;
	for (i = 0; i < NRQS; i++) {
	    if (q1->next == q1) {
		if (q1->prev != q1) {
		    panic("checkrq: empty at %s", msg);
	        }
	    }
	    else {
		if (low == -1)
		    low = i;
		
		for (e = q1->next; e != q1; e = e->next) {
		    j++;
		    if (e->next->prev != e)
			panic("checkrq-2 at %s", msg);
		    if (e->prev->next != e)
			panic("checkrq-3 at %s", msg);
		}
	    }
	    q1++;
	}
	if (j != rq->count)
	    panic("checkrq: count wrong at %s", msg);
	if (rq->count != 0 && low < rq->low)
	    panic("checkrq: low wrong at %s", msg);
}

void
thread_check(
	register thread_t	th,
	register run_queue_t	rq)
{
	register unsigned int 	whichq;

	whichq = th->sched_pri;
	if (whichq > MINPRI) {
		printf("thread_check: priority too high\n");
		whichq = MINPRI;
	}
	if ((th->links.next == &rq->runq[whichq]) &&
		(rq->runq[whichq].prev != (queue_entry_t)th))
			panic("thread_check");
}

#endif	/* DEBUG */

#if	MACH_KDB
#include <ddb/db_output.h>
#define	printf		kdbprintf
extern int		indent;
void			db_sched(void);

void
db_sched(void)
{
	iprintf("Scheduling Statistics:\n");
	indent += 2;
	iprintf("Thread invocations:  csw %d same %d\n",
		c_thread_invoke_csw, c_thread_invoke_same);
#if	MACH_COUNTERS
#if	FAST_IDLE
	iprintf("Fast Idle:  %s\n", fast_idle_enabled ? "ENABLED" : "disabled");
	iprintf("Thread block:  calls %d run hits %d misses %d fast rej %d\n",
		c_thread_block_calls, c_thread_block_run_hit,
		c_thread_block_run_miss, c_thread_block_fast_idle_rejected);
	iprintf("Idle thread:\n\tcall %d handoff %d block %d no_dispatch %d\n",
		c_idle_thread_loop_calls, c_idle_thread_handoff,
		c_idle_thread_block, no_dispatch_count);
	iprintf("\tfast %d thd_exits %d thd_state %d defrun %d myprocrunq %d\n",
		c_idle_thread_fast, c_idle_thread_fast_thd_exits,
		c_idle_thread_fast_thd_state_exits, c_idle_thread_fast_def_runq,
		c_idle_thread_fast_myproc_runq);
	iprintf("\tfast states:  run %d disp %d idle %d assgn %d shutdn %d\n",
		c_idle_thread_fast_proc_running,
		c_idle_thread_fast_proc_dispatch,
		c_idle_thread_fast_proc_idle,
		c_idle_thread_fast_proc_assign,
		c_idle_thread_fast_proc_shutdown);
#else	/* FAST_IDLE */
	iprintf("Thread block:  calls %d\n",
		c_thread_block_calls);
	iprintf("Idle thread:\n\thandoff %d block %d no_dispatch %d\n",
		c_idle_thread_handoff,
		c_idle_thread_block, no_dispatch_count);
#endif	/* FAST_IDLE */
	iprintf("Sched thread blocks:  %d\n", c_sched_thread_block);
#endif	/* MACH_COUNTERS */
	indent -= 2;
}
#endif	/* MACH_KDB */
