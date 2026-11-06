/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Taking a thread out of a call it is not going to finish (#475).
 *
 * A thread port is an ordinary right.  A task that holds one may terminate,
 * abort or suspend the thread it names, and it may do so while that thread is
 * stopped in the middle of a kernel call -- which is not an edge case but the
 * normal working of a debugger, a personality server, or a pthread
 * cancellation.  Every one of those crossings ends in special_handler(), and
 * on this machine special_handler() had never been reached: the panic in
 * act_machine_return() was found by #467's test doing the obvious thing in its
 * exception handler.
 *
 * ⚠️ So this is a denial of service before it is a gap.  Nothing here is
 * privileged; a program does this to its own threads.
 *
 * The three arms are the three ways in, and they are deliberately in this
 * order -- the one that panicked first, then the two that share its machinery
 * without ending in a death.
 *
 *   1. thread_terminate() on a thread stopped inside exception_raise()
 *   2. thread_abort()     on a thread asleep in mach_msg
 *   3. thread_suspend()/thread_resume() on a thread asleep in mach_msg
 *
 * And two that came later, for #603 -- suspending a thread that is in no call
 * at all, which is where a return to ring 3 has to take the AST by itself:
 *
 *   7. thread_suspend() on a thread that lives in page faults
 *   8. thread_suspend() on a thread that spins, at one processor
 *
 * And three for #687 -- the abort crossing the reply it was waiting for, the
 * abort that comes before the wait, and a lock the wait slept on:
 *
 *  10. thread_abort() on a thread in an RPC whose reply is being delivered
 *  11. thread_abort(), fired again and again, on a thread making RPCs
 *  12. mach_port_get_attributes() on the reply port of a thread making RPCs
 *
 * And one for #688 -- the space dying under a thread that is inside a trap:
 *
 *  14. task_terminate() on a child whose threads loop in traps on its space
 *
 * ⚠️ And the task is expected to SURVIVE all three and print a summary.  A
 * report that a thread died is worth nothing from a program that died with
 * it: the last line is as much of the test as the arms are.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/message.h>
#include <mach/exception.h>
#include <mach/task_special_ports.h>
#include <mach/vm_prot.h>
#include <mach/thread_status.h>	/* #474: x86_64_THREAD_STATE */

/*
 * ⚠️ mach_host.h and not mach.h: task_set_exception_ports is generated from
 * mach_host.defs, and the umbrella header does not pull that half in.  Without
 * this the call compiles as an implicit declaration returning int -- which on
 * this target means five arguments passed to a prototype nobody checked.
 */
#include <mach/mach_host.h>
#include <mach/host_info.h>	/* #603: how many processors arm eight has */
#include <mach/thread_info.h>	/* #603: arm seven's yardstick call */
#include <mach/processor_info.h> /* #660: arm nine's thread count */
#include <mach/thread_switch.h>	/* #687: arm eleven's yield */
#include <mach_debug/mach_debug.h>	/* #871: arm forty-three's zone counts */
#include <mach/bootstrap.h>	/* #660: the privileged host port it needs */
#include <mach/task_info.h>	/* #688: arm fourteen's question */
#include <mach/mach_syscalls.h>	/* #688: syscall_task_info() */
#include <mach/ndr.h>		/* #688: NDR_record, for its message */
#include <mach/mach_port.h>	/* #662: mach_port_names(), to count the space */
#include <mach/notify.h>	/* #843: arm thirty-five's no-senders */
#include <mach/rpc.h>		/* #869: struct rpc_subsystem */

#include <stdio.h>
#include <pthread.h>

#include "exc_server.h"

/*
 * Where arm one faults.
 *
 * The lower half is the task's to have and this task does not have this piece
 * of it, so the fault is a real KERN_INVALID_ADDRESS and EXC_BAD_ACCESS is
 * what the handler is told.  Not zero, which is where a hundred other defects
 * also land, and not an upper-half address, which the processor would refuse
 * before the kernel had an opinion.
 */
#define UNOWNED		((vm_offset_t) 0x0000300000000000ULL)

static mach_port_t	exc_port;
static mach_port_t	nap_port;

/*
 * How long this program is willing to wait for a thread to do something, in
 * tenths of a second, and it is a generous number on purpose.
 *
 * ⚠️ The x86-64 runs are TCG: an emulated processor executing a scheduler
 * decision takes a wall-clock time that has nothing to do with the kernel's.
 * A bound tight enough to be interesting on hardware would report a scheduler
 * defect here every time the host was busy, which is a measurement that says
 * more about the host than about the kernel.
 */
#define PATIENCE	100

/*
 * A receive buffer with room to spare, and the spare room is the point.
 *
 * The kernel appends a trailer to every message it delivers, so a buffer of
 * exactly sizeof(mach_msg_header_t) is too small for a bare header and the
 * receive fails with MACH_RCV_TOO_LARGE -- which arrives as "the thread never
 * got the message" and reads exactly like the kernel defect this program is
 * looking for.  It cost a run.
 */
struct roomy_msg {
	mach_msg_header_t	h;
	char			slack[256];
};

/*
 * Sleep, without a libc to ask.
 *
 * A receive that will never be satisfied, with a timeout: the thread is off
 * the run queue for the duration, which is what a sleep is, and the kernel
 * path it uses is the same one the arms below interrupt.  Spinning on a
 * volatile would work too and would be worse -- on one processor it would
 * spend the whole quantum denying the thread being waited for the very
 * processor it needs to make progress on.
 */
static kern_return_t
nap(int ms)
{
	mach_msg_header_t	junk;

	/*
	 * ⚠️ The result is returned rather than dropped, because a sleep that
	 * does not sleep is indistinguishable from a wait that expired -- both
	 * are a loop that ran out of turns.  MACH_RCV_TIMED_OUT is what a nap
	 * that worked answers; anything else means the caller's patience was
	 * spent in microseconds and its verdict is about nothing.
	 */
	return mach_msg(&junk, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			sizeof junk, nap_port, ms, MACH_PORT_NULL);
}

/*
 * ── Arm one: terminate a thread stopped inside exception_raise() ──────
 *
 * The thread faults on an address its task does not own.  The kernel composes
 * an exception message, sends it to the task's exception port, and BLOCKS the
 * faulting thread in mach_msg_rpc_from_kernel() waiting for the reply.  That
 * is the state the issue is about: a thread stopped in the middle of a kernel
 * call, with a return path through code it will now never take.
 *
 * The handler answers by killing it, which is what a debugger does with a
 * thread it has decided not to resume.  It does not have to guess the port:
 * exc.defs hands the handler the thread as its second argument, delivered by
 * the kernel, so what is terminated here is the thread the kernel named.
 */
static volatile int		arm_one_handler_ran;
static volatile kern_return_t	arm_one_kill_kr = -1;
static volatile kern_return_t	arm_one_after_kr = -1;
static volatile int		arm_one_thread_ran_on;

static void *
the_thread_that_gets_killed(void *arg)
{
	volatile unsigned long	*p = (volatile unsigned long *) UNOWNED;

	(void) arg;

	/*
	 * ⚠️ Nothing before the fault, and nothing that takes a lock.  This
	 * thread is about to be destroyed at an instruction boundary chosen by
	 * another thread; a printf here would leave the console lock held by a
	 * thread that no longer exists, and the arms after this one would
	 * report on that instead of on themselves.
	 */
	(void) *p;

	/*
	 * Not reached on a kernel that terminates the thread.  If it IS
	 * reached, that is the finding, and the flag is how the summary says
	 * so rather than the program hanging with nothing printed.
	 */
	arm_one_thread_ran_on = 1;
	return NULL;
}

/*
 * ⚠️ Arm six's state, up here because the handler below is shared and has to
 * be able to tell whose exception it is holding.  There is one exception port
 * for the task and MIG allows one routine of this name, so the arms take turns
 * on it rather than each having its own.
 */
static volatile mach_port_t	arm_six_thread;
static volatile int		arm_six_armed;
static volatile int		arm_six_exception = -1;
static volatile int		arm_six_code;
static volatile int		arm_six_named_the_victim;

/* #780: arms seventeen and eighteen, whose handler is the same as arm six's. */
#define	ARM17_ROUNDS	100

static volatile int		arm_seventeen_armed;
static volatile mach_port_t	arm_seventeen_thread;
static volatile int		arm_seventeen_seen, arm_seventeen_others;
static volatile int		arm_seventeen_exception, arm_seventeen_code;

static volatile int		arm_eighteen_armed;
static volatile mach_port_t	arm_eighteen_thread;
static volatile int		arm_eighteen_stop;
static volatile int		arm_eighteen_named_the_victim = -2;
static volatile int		arm_eighteen_exception, arm_eighteen_code;

static volatile int		arm_thirty_two_armed;
static volatile int		arm_forty_seven_armed;
static volatile int		arm_forty_seven_task_seen;
static volatile mach_port_t	arm_thirty_two_thread;
static volatile int		arm_thirty_two_task_seen;
static volatile int		arm_thirty_two_named_the_victim = -2;
static volatile int		arm_thirty_two_exception;

/*
 * What exc_server calls when the message arrives.  MIG demands this name and
 * this shape.
 */
kern_return_t
catch_exception_raise(mach_port_t exception_port, mach_port_t thread,
		      mach_port_t task, int exception,
		      exception_data_t code, mach_msg_type_number_t codeCnt)
{
	vm_address_t	fix = UNOWNED;

	(void) exception_port;

	/*
	 * ⚠️ Arm six first, and it does not fall through.  Its victim is a
	 * thread that cannot be repaired -- there is no page to allocate that
	 * makes a non-canonical address resumable -- so the only ending that
	 * lets this program keep running is to destroy it.
	 */
	if (arm_seventeen_armed) {
		if (thread == arm_seventeen_thread) {
			arm_seventeen_seen++;
			arm_seventeen_exception = exception;
			arm_seventeen_code = (codeCnt > 0) ? (int) code[0] : 0;
			/*
			 * "Resume", unchanged, until the last round: the
			 * thread comes back through the same tail each time.
			 */
			if (arm_seventeen_seen >= ARM17_ROUNDS)
				(void) thread_terminate(thread);
		} else {
			arm_seventeen_others++;
		}
		/*
		 * Each request brings a reference to both names, and an
		 * answer of KERN_SUCCESS keeps them (arm one's note above):
		 * a hundred rounds would leave a hundred behind.
		 */
		(void) mach_port_deallocate(mach_task_self(), thread);
		(void) mach_port_deallocate(mach_task_self(), task);
		return KERN_SUCCESS;
	}
	if (arm_eighteen_armed) {
		arm_eighteen_exception = exception;
		arm_eighteen_code = (codeCnt > 0) ? (int) code[0] : 0;
		arm_eighteen_named_the_victim = (thread == arm_eighteen_thread);
		(void) thread_terminate(thread);
		(void) mach_port_deallocate(mach_task_self(), thread);
		(void) mach_port_deallocate(mach_task_self(), task);
		return KERN_SUCCESS;
	}
	if (arm_forty_seven_armed) {
		/*
		 * The task's port, where the kernel goes when it could not
		 * read the state for the thread's port (#887).
		 */
		arm_forty_seven_task_seen = 1;
		(void) thread_terminate(thread);
		(void) mach_port_deallocate(mach_task_self(), thread);
		(void) mach_port_deallocate(mach_task_self(), task);
		return KERN_SUCCESS;
	}
	if (arm_thirty_two_armed) {
		/*
		 * The task's port, second: the thread's has had its answer, and
		 * this is where the kernel goes when that answer failed.
		 */
		arm_thirty_two_task_seen = 1;
		arm_thirty_two_exception = exception;
		arm_thirty_two_named_the_victim = (thread == arm_thirty_two_thread);
		(void) thread_terminate(thread);
		(void) mach_port_deallocate(mach_task_self(), thread);
		(void) mach_port_deallocate(mach_task_self(), task);
		return KERN_SUCCESS;
	}
	if (arm_six_armed) {
		arm_six_exception = exception;
		arm_six_code = (codeCnt > 0) ? (int) code[0] : 0;
		arm_six_named_the_victim = (thread == arm_six_thread);
		(void) thread_terminate(thread);
		return KERN_SUCCESS;
	}

	arm_one_handler_ran = 1;
	arm_one_kill_kr = thread_terminate(thread);

	/*
	 * ⚠️ And then ask the kernel about the thread again, at once and with
	 * the same name.
	 *
	 * Because "thread_terminate answered KERN_SUCCESS" is the kernel
	 * agreeing with itself, and #475 was precisely about a path where the
	 * answer and the state came apart.  thread_suspend() on a live
	 * activation returns KERN_SUCCESS and on a killed one something else,
	 * so the two outcomes are different VALUES rather than the presence or
	 * absence of one -- which is the only shape of observation that can
	 * tell the kill from a kill that did nothing.  Which something else,
	 * and why, is written where the answer is judged (#507).
	 *
	 * The name stays valid after this handler returns: exc_server passes
	 * it and deallocates nothing, and mach_msg_server_once destroys the
	 * request -- and the rights in it -- only when the handler answers
	 * something other than KERN_SUCCESS (libmach/mach_msg_server.c:100-115).
	 * It is asked here, at once, because "at once" is the question.
	 */
	arm_one_after_kr = thread_suspend(thread);

	/*
	 * If the kill did not take, do not answer KERN_SUCCESS on a thread
	 * that is still going to resume at the instruction that faulted: it
	 * would fault again, arrive here again, and the boot would spin
	 * printing nothing.  Repair the page instead, so the thread finishes
	 * and the summary below gets to report the failure in words.
	 */
	if (arm_one_kill_kr != KERN_SUCCESS)
		(void) vm_allocate(task, &fix, vm_page_size, FALSE);

	return KERN_SUCCESS;
}

kern_return_t
catch_exception_raise_state(mach_port_t exception_port, int exception,
			    exception_data_t code, mach_msg_type_number_t codeCnt,
			    int *flavor, thread_state_t old_state,
			    mach_msg_type_number_t old_stateCnt,
			    thread_state_t new_state,
			    mach_msg_type_number_t *new_stateCnt)
{
	(void) exception_port; (void) exception; (void) code; (void) codeCnt;
	(void) flavor; (void) old_state; (void) old_stateCnt;
	(void) new_state; (void) new_stateCnt;
	return KERN_FAILURE;
}

kern_return_t
catch_exception_raise_state_identity(mach_port_t exception_port,
				     mach_port_t thread, mach_port_t task,
				     int exception, exception_data_t code,
				     mach_msg_type_number_t codeCnt,
				     int *flavor, thread_state_t old_state,
				     mach_msg_type_number_t old_stateCnt,
				     thread_state_t new_state,
				     mach_msg_type_number_t *new_stateCnt)
{
	(void) exception_port; (void) thread; (void) task; (void) exception;
	(void) code; (void) codeCnt; (void) flavor; (void) old_state;
	(void) old_stateCnt; (void) new_state; (void) new_stateCnt;
	return KERN_FAILURE;
}

static int
arm_one_terminate_in_exception(void)
{
	pthread_t	victim;
	kern_return_t	kr;
	const char	*after;

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &exc_port);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [1] mach_port_allocate failed (%d) — WRONG\n",
		       kr);
		return 0;
	}
	kr = mach_port_insert_right(mach_task_self(), exc_port, exc_port,
				    MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [1] insert_right failed (%d) — WRONG\n", kr);
		return 0;
	}
	kr = task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS,
				      exc_port, EXCEPTION_DEFAULT,
				      THREAD_STATE_NONE);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [1] task_set_exception_ports failed (%d)"
		       " — WRONG\n", kr);
		return 0;
	}

	if (pthread_create(&victim, NULL, the_thread_that_gets_killed,
			   NULL) != 0) {
		printf("act_test: [1] pthread_create failed — WRONG\n");
		return 0;
	}

	/*
	 * One message.  mach_msg_server_once returns after demultiplexing
	 * exactly one, which is all this arm produces; a loop would leave the
	 * program with nothing to print.
	 */
	(void) mach_msg_server_once(exc_server, 4096, exc_port,
				    MACH_MSG_OPTION_NONE);

	if (!arm_one_handler_ran) {
		printf("act_test: [1] no exception arrived — WRONG\n");
		return 0;
	}
	if (arm_one_kill_kr != KERN_SUCCESS) {
		printf("act_test: [1] thread_terminate on a thread stopped in "
		       "exception_raise answered %d — WRONG\n",
		       (int) arm_one_kill_kr);
		return 0;
	}

	/*
	 * ── What the same name can answer after the kill, and in what order ──
	 *
	 * 🔴 THE PROPERTY, not the answers seen so far (#507): the kill took
	 * effect, so the name that answered KERN_SUCCESS to thread_terminate a
	 * moment ago no longer reaches a live activation.  Which answer says so
	 * depends only on how far the teardown the kill started has got when
	 * the question is asked, and the kernel's order is (K = mach_kernel/):
	 *
	 *   1. inside thread_terminate, in THIS thread: the port stops naming
	 *      the activation (K/kern/thread_act.c:298 -> kern/ipc_tt.c:395,
	 *      kobject IKOT_NONE), and only then is the activation marked
	 *      inactive (thread_act.c:317).  Both under the act lock, both done
	 *      before the call returns.  A question from here on converts the
	 *      name to no activation, and thread_suspend(THR_ACT_NULL) answers
	 *      KERN_INVALID_ARGUMENT (thread_act.c:404).
	 *   2. the activation is freed and its port destroyed with it
	 *      (act_free, thread_act.c:1205 -> ipc_tt.c:425), by whichever
	 *      thread drops the last reference to it.  Normally that is the
	 *      reaper, once the killed thread has run its own end -- which may
	 *      be before or after thread_terminate returns to user mode.  But
	 *      THIS thread holds a reference through the kill
	 *      (kern/ipc_mig.c:1931, dropped at :1936): if it is off the
	 *      processor between the kill and :1936 while the other two finish,
	 *      the free is its own.  On one processor only an interrupt can put
	 *      it there, because x86-64 preempts in the kernel only on an
	 *      interrupt's return; a preemption on the way out of the trap
	 *      comes after :1936 and leaves the free to the reaper.  On several
	 *      processors it can also block on the act lock.  Either way the
	 *      name is dead, and the send is refused at copyin:
	 *      MACH_SEND_INVALID_DEST (ipc/ipc_kmsg.c:1576).  One event, not two
	 *      -- there is no moment when the activation is gone and the port
	 *      still alive.
	 *   2'. the port dying between copyin and the kernel's dispatch drops the
	 *      request on a dead port, and the stub answers MIG_SERVER_DIED.
	 *      Read from the code; no boot has shown it.
	 *
	 * The order only goes forward, so a question sees the stage it reached
	 * and never an earlier one.  Asked at once it is nearly always stage 1;
	 * a clock tick or the killed thread's priority can let the teardown
	 * finish first, and then it is stage 2 -- once in the 144 one-processor
	 * runs here and in none of the 202 on two or four, and once in five in
	 * the issue's first campaign (TCG, one processor).
	 *
	 * ⚠️ KERN_TERMINATED is what an INACTIVE activation answers
	 * (thread_act.c:409), and this used to be the first of the two answers
	 * the arm accepted.  Step 1 makes it unreachable for a question asked
	 * after thread_terminate returns: by then the name reaches no
	 * activation, active or not.  So it is refused below as a CHANGE OF
	 * ORDER -- the kill still took effect, but the sequence written here is
	 * no longer the kernel's, and this is where that has to be noticed.
	 *
	 * KERN_SUCCESS is the one that would mean nothing happened, and it is
	 * the reason this is asked at all.
	 */
	switch (arm_one_after_kr) {
	case KERN_INVALID_ARGUMENT:
		after = "KERN_INVALID_ARGUMENT: the port no longer names an "
			"activation";
		break;
	case MACH_SEND_INVALID_DEST:
		after = "MACH_SEND_INVALID_DEST: the port itself is dead, the "
			"activation freed";
		break;
	case MIG_SERVER_DIED:
		after = "MIG_SERVER_DIED: the port died while the question was "
			"on its way";
		break;
	case KERN_TERMINATED:
		/* ⚠️ At most 256 bytes on the wire, CR and LF included: a longer
		 * line is written in two console holds and can be cut (#578). */
		printf("act_test: [1] the same name answered KERN_TERMINATED "
		       "after the kill: the kill took effect, but this test's "
		       "written order says no question asked after the kill can "
		       "reach an inactive activation — the order changed — "
		       "WRONG\n");
		return 0;
	default:
		printf("act_test: [1] after the kill the kernel still answered "
		       "%d for that thread — WRONG\n", (int) arm_one_after_kr);
		return 0;
	}

	/*
	 * ⚠️ No pthread_join.  The thread was destroyed by the kernel and
	 * libpthreads was never told, so its join word is never written and a
	 * join here would wait for it forever.  Reaping a Mach-terminated
	 * pthread is a libpthreads question, not this one.
	 *
	 * Nor is there a wait here for the thread to fail to run: whether it
	 * ever ran again is answered in main(), after the other two arms have
	 * given it several seconds of a live system to do it in.  A sleep
	 * spent watching for something that cannot happen is a sleep, not a
	 * measurement.
	 */
	printf("act_test: [1] a thread stopped inside exception_raise was "
	       "terminated, and the same name now answers %s — the task is "
	       "still here to say so\n", after);
	return 1;
}

/*
 * ── Arm two: abort a thread asleep in mach_msg ────────────────────────
 *
 * Same machinery, different ending.  thread_abort() installs the special
 * handler on an activation that is still ACTIVE, so special_handler() does not
 * reach act_machine_return() at all -- it clears TH_ABORT and returns, and the
 * interrupted call comes back to ring 3 with MACH_RCV_INTERRUPTED.
 *
 * Worth its own arm because that return is the half act_machine_return() does
 * not cover: the thread has to get out of the kernel through the return path
 * and be alive on the other side.  A machine that only ever killed the thread
 * it interrupted would pass arm one and fail this.
 */
static mach_port_t		arm_two_port;
static volatile mach_port_t	arm_two_thread;
static volatile int		arm_two_first_done;
static volatile int		arm_two_returned;
static volatile kern_return_t	arm_two_kr = -1;
static volatile kern_return_t	arm_two_again_kr = -1;

static void *
the_thread_that_gets_aborted(void *arg)
{
	struct roomy_msg	msg;

	(void) arg;

	arm_two_thread = mach_thread_self();

	/*
	 * No timeout, and nobody will ever send here: the only thing that can
	 * end this call is the abort.  A timeout would give the arm a second
	 * way to finish, and a test with two ways to pass reports on neither.
	 *
	 * ⚠️ MACH_RCV_INTERRUPT, and without it this arm reports a kernel
	 * defect that is not there.  libmach's mach_msg() RE-ISSUES the trap
	 * on MACH_RCV_INTERRUPTED -- see the loop in lib/libmach/mach_msg.c --
	 * so an aborted receive quietly becomes another receive and the thread
	 * is asleep again before the caller can see anything.  That is correct
	 * and deliberate: mach_msg() is defined to hide interruptions, and
	 * this flag is how a caller says it would rather be told.
	 *
	 * The first run of this test said "thread_abort never brought the
	 * thread out of mach_msg", which was true and was about the library.
	 */
	arm_two_kr = mach_msg(&msg.h, MACH_RCV_MSG | MACH_RCV_INTERRUPT, 0,
			      sizeof msg, arm_two_port, MACH_MSG_TIMEOUT_NONE,
			      MACH_PORT_NULL);

	/*
	 * ⚠️ Told to STOP ABORTING before the second receive, and this flag is
	 * the whole reason the two are separate.
	 *
	 * The retry loop in the arm below fires thread_abort() until the thread
	 * reports back.  With one flag set after both receives, that loop was
	 * still firing during the second one -- which then returned
	 * MACH_RCV_INTERRUPTED because it had genuinely been interrupted, and
	 * the test read that as TH_ABORT surviving.  A test that keeps doing
	 * the thing it is checking for cannot check for it.
	 */
	arm_two_first_done = 1;

	/*
	 * ⚠️ And then sleep ONCE MORE, which is the observation that names the
	 * cause rather than the symptom.
	 *
	 * Clearing TH_ABORT is the first thing special_handler() does, and
	 * special_handler() runs off an AST on the way back to ring 3.  So if
	 * the return path never takes its ASTs, the bit survives the abort --
	 * and a surviving TH_ABORT makes every LATER wait this thread performs
	 * return THREAD_INTERRUPTED at once, for ever.
	 *
	 * The two hypotheses therefore predict two different VALUES from the
	 * receive below, not the presence or absence of one: MACH_RCV_TIMED_OUT
	 * if the abort was consumed, MACH_RCV_INTERRUPTED if it was not.  A
	 * test that only checked the first receive could not tell them apart,
	 * because both of them pass it.
	 */
	arm_two_again_kr = mach_msg(&msg.h,
				    MACH_RCV_MSG | MACH_RCV_INTERRUPT
				    | MACH_RCV_TIMEOUT,
				    0, sizeof msg, arm_two_port, 300,
				    MACH_PORT_NULL);
	arm_two_returned = 1;
	return NULL;
}

static int
arm_two_abort_in_mach_msg(void)
{
	pthread_t	victim;
	kern_return_t	kr;
	int		i;

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &arm_two_port);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [2] mach_port_allocate failed (%d) — WRONG\n",
		       kr);
		return 0;
	}

	if (pthread_create(&victim, NULL, the_thread_that_gets_aborted,
			   NULL) != 0) {
		printf("act_test: [2] pthread_create failed — WRONG\n");
		return 0;
	}

	for (i = 0; i < PATIENCE && arm_two_thread == MACH_PORT_NULL; i++)
		nap(100);
	if (arm_two_thread == MACH_PORT_NULL) {
		printf("act_test: [2] the thread never published its port "
		       "— WRONG\n");
		return 0;
	}

	/*
	 * ⚠️ Asked more than once, and that is not impatience.
	 *
	 * thread_abort() interrupts a wait that is ALREADY outstanding: it
	 * calls clear_wait(), which does nothing at all to a thread that has
	 * published its port but has not reached assert_wait() yet.  A single
	 * abort fired into that window is lost and the thread sleeps forever
	 * -- a hang, which is the one failure a boot log cannot tell apart
	 * from a kernel that is merely slow.  Retrying costs a few messages
	 * and removes the race from the test rather than from the kernel,
	 * which is where it belongs: the kernel's contract here is "interrupt
	 * a waiting thread", not "remember that somebody wanted to".
	 */
	for (i = 0; i < PATIENCE && !arm_two_first_done; i++) {
		(void) thread_abort(arm_two_thread);
		nap(100);
	}

	for (i = 0; i < PATIENCE && !arm_two_returned; i++)
		nap(100);

	if (!arm_two_returned) {
		printf("act_test: [2] thread_abort never brought the thread "
		       "out of mach_msg — WRONG\n");
		return 0;
	}

	(void) pthread_join(victim, NULL);

	if (arm_two_kr != MACH_RCV_INTERRUPTED) {
		printf("act_test: [2] the aborted mach_msg returned 0x%x, not "
		       "MACH_RCV_INTERRUPTED — WRONG\n", (int) arm_two_kr);
		return 0;
	}

	if (arm_two_again_kr != MACH_RCV_TIMED_OUT) {
		printf("act_test: [2] the thread's NEXT receive answered 0x%x "
		       "instead of timing out (0x%x)%s — WRONG\n",
		       (int) arm_two_again_kr, MACH_RCV_TIMED_OUT,
		       (arm_two_again_kr == MACH_RCV_INTERRUPTED)
		       ? ": TH_ABORT outlived the abort, so the return path "
			 "never ran its handlers" : "");
		return 0;
	}

	printf("act_test: [2] a thread asleep in mach_msg was aborted, came "
	       "back with MACH_RCV_INTERRUPTED, and its next sleep timed out "
	       "normally — the abort was consumed, not left behind\n");
	return 1;
}

/*
 * ── Arm three: suspend one, then let it go ────────────────────────────
 *
 * The third route through special_handler(): the activation is active and not
 * aborted, but its suspend_count is up, so the handler parks the thread on
 * &suspend_count and does not return until somebody resumes it.
 *
 * What makes this arm a measurement rather than a formality is the message
 * sent WHILE the thread is suspended.  A receive that is merely slow and a
 * receive that is genuinely stopped look identical from outside -- unless
 * something the thread would certainly have taken is put in front of it and
 * it does not take it.  So: suspend, send, wait, and require silence; then
 * resume, and require the message.
 */
static mach_port_t		arm_three_port;
static volatile mach_port_t	arm_three_thread;
static volatile int		arm_three_got;
static volatile int		arm_three_ran;
static volatile kern_return_t	arm_three_kr = -1;

static void *
the_thread_that_gets_suspended(void *arg)
{
	struct roomy_msg	msg;

	(void) arg;

	/*
	 * ⚠️ Set BEFORE the port is asked for.  "The port never appeared" has
	 * two causes -- a thread that never ran, and a thread that ran and got
	 * MACH_PORT_NULL back -- and they are the same silence.  This flag is
	 * what makes them two different observations.
	 */
	arm_three_ran = 1;
	arm_three_thread = mach_thread_self();

	/*
	 * ⚠️ NOT MACH_RCV_INTERRUPT, unlike the arm above, and the difference
	 * is the question each one asks.  Arm two wants to see the
	 * interruption; this one wants the ordinary shape -- a thread in the
	 * receive libmach re-issues, which is what every server in the system
	 * is sitting in.  Being suspended must work on THAT thread, not only
	 * on one that asked to be told.
	 */
	arm_three_kr = mach_msg(&msg.h, MACH_RCV_MSG, 0, sizeof msg,
				arm_three_port, MACH_MSG_TIMEOUT_NONE,
				MACH_PORT_NULL);
	if (arm_three_kr == MACH_MSG_SUCCESS)
		arm_three_got = 1;
	return NULL;
}

static int
arm_three_suspend_in_mach_msg(void)
{
	pthread_t		victim;
	mach_msg_header_t	msg;
	mach_port_t		send;
	kern_return_t		kr;
	kern_return_t		slept = -1;
	int			i;
	int			slept_through_it;

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &arm_three_port);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [3] mach_port_allocate failed (%d) — WRONG\n",
		       kr);
		return 0;
	}
	kr = mach_port_insert_right(mach_task_self(), arm_three_port,
				    arm_three_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [3] insert_right failed (%d) — WRONG\n", kr);
		return 0;
	}
	send = arm_three_port;

	if (pthread_create(&victim, NULL, the_thread_that_gets_suspended,
			   NULL) != 0) {
		printf("act_test: [3] pthread_create failed — WRONG\n");
		return 0;
	}

	for (i = 0; i < PATIENCE && arm_three_thread == MACH_PORT_NULL; i++)
		slept = nap(100);
	if (arm_three_thread == MACH_PORT_NULL) {
		printf("act_test: [3] no port after %d naps: the thread %s, "
		       "and the last nap answered 0x%x (0x%x is a nap that "
		       "worked) — WRONG\n", i,
		       arm_three_ran ? "RAN" : "never ran",
		       (int) slept, MACH_RCV_TIMED_OUT);
		return 0;
	}

	/*
	 * Let it reach the receive.  Unlike the abort above, a suspend does
	 * not need the thread to be waiting already -- suspend_count is state,
	 * and the handler runs on the way to ring 3 whenever that is -- so one
	 * call is enough and no retry loop is needed.
	 */
	nap(300);

	kr = thread_suspend(arm_three_thread);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [3] thread_suspend answered %d — WRONG\n", kr);
		return 0;
	}

	msg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	msg.msgh_size = sizeof msg;
	msg.msgh_remote_port = send;
	msg.msgh_local_port = MACH_PORT_NULL;
	msg.msgh_id = 475;
	kr = mach_msg(&msg, MACH_SEND_MSG, sizeof msg, 0, MACH_PORT_NULL,
		      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS) {
		printf("act_test: [3] the send answered 0x%x — WRONG\n",
		       (int) kr);
		return 0;
	}

	/*
	 * The message is queued and the thread is the only receiver.  If it is
	 * really stopped it cannot have taken it; if thread_suspend answered
	 * KERN_SUCCESS on a thread that kept running, it has.
	 */
	nap(500);
	slept_through_it = !arm_three_got;

	kr = thread_resume(arm_three_thread);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [3] thread_resume answered %d — WRONG\n", kr);
		return 0;
	}

	for (i = 0; i < PATIENCE && !arm_three_got; i++)
		nap(100);

	if (!arm_three_got) {
		printf("act_test: [3] the resumed thread never took the "
		       "message, its receive standing at 0x%x — WRONG\n",
		       (int) arm_three_kr);
		return 0;
	}

	(void) pthread_join(victim, NULL);

	if (!slept_through_it) {
		printf("act_test: [3] the suspended thread took a message "
		       "while it was supposed to be stopped — WRONG\n");
		return 0;
	}

	printf("act_test: [3] a thread asleep in mach_msg was suspended, did "
	       "not take a message queued under it, and took it on resume\n");
	return 1;
}

/* ----------------------------------------------------------------
 * Arm five: what a Mach trap leaves in the registers it destroys (#474)
 * ---------------------------------------------------------------- */

/*
 * ⚠️ Arm four catches this by luck; this one catches it on purpose.
 *
 * The syscall contract declares rdi, rsi, rdx, rcx, r8, r9, r10, r11 and rax
 * destroyed, and the entry honours that by not SAVING them.  The way out owes
 * the other half: "destroyed" tells the CALLER it may not read them, it does
 * not license the kernel to leave its own values there -- and SYSRET hands
 * every one of them to ring 3.
 *
 * Three of the eight legitimately carry something out: rax is the answer, and
 * rcx and r11 are what SYSRET resumes from, both read back out of the frame
 * and so ring 3's own.  These six are the ones with nothing to say.
 *
 * 🔥 Arm four found this only when the leaked value happened to be a
 * high-half address AND survived in the register until a later trap saved it
 * into the frame -- one boot in thirty.  The leak itself is on EVERY return.
 * So this arm reads the six registers directly, immediately after a trap,
 * many times: a kernel that leaks cannot get through the loop.
 *
 * ⚠️ It tests for a KERNEL ADDRESS and not for zero.  Zero is how this kernel
 * happens to clean up today; a high-half value in a register a user program
 * just read is the finding, whatever the cleanup looks like.
 */
static int
arm_five_registers_a_trap_leaves(void)
{
	unsigned long long	r[6];
	unsigned long long	bad = 0;
	const char		*name = 0;
	static const char *const nm[6] = {
		"rdi", "rsi", "rdx", "r8", "r9", "r10"
	};
	int			i, j;

	for (i = 0; i < 128 && bad == 0; i++) {
		mach_port_t	self;

		/*
		 * A trap whose registers afterwards came from the kernel and
		 * not from the call.  The asm block reads them before anything
		 * else can touch them -- there is no C between the two.
		 *
		 * ⚠️ And the right is GIVEN BACK.  mach_thread_self() hands out
		 * a send right on every call; the first version of this arm
		 * took five hundred and twelve and released none, and the task
		 * that followed it in the bundle -- fault_test -- stopped
		 * finishing.  A test that damages the machine it is measuring
		 * is not measuring the machine.
		 */
		self = mach_thread_self();

		__asm__ __volatile__(
			"movq %%rdi, %0\n\t"
			"movq %%rsi, %1\n\t"
			"movq %%rdx, %2\n\t"
			"movq %%r8,  %3\n\t"
			"movq %%r9,  %4\n\t"
			"movq %%r10, %5"
			: "=m"(r[0]), "=m"(r[1]), "=m"(r[2]),
			  "=m"(r[3]), "=m"(r[4]), "=m"(r[5])
			:
			: "memory");

		for (j = 0; j < 6; j++)
			if ((r[j] >> 63) != 0) {
				bad = r[j];
				name = nm[j];
				break;
			}

		(void) mach_port_deallocate(mach_task_self(), self);
	}

	if (bad != 0) {
		printf("act_test: [5] %s came back from a Mach trap holding "
		       "0x%llx — a kernel address in a register ring 3 can "
		       "read — WRONG\n", name, bad);
		return 0;
	}

	printf("act_test: [5] 128 returns from a Mach trap and not one of "
	       "rdi/rsi/rdx/r8/r9/r10 came back holding a kernel address — "
	       "what the contract calls destroyed is not the kernel's to "
	       "leave behind\n");
	return 1;
}


/*
 * ── Arm four: ask a thread that is inside the kernel what it was doing ─
 *
 * A debugger stops a thread and reads its registers.  The thread it stops is
 * almost never in ring 3 at that moment -- it is asleep in mach_msg, which is
 * where a server thread spends its life -- so "the registers of a thread
 * inside a Mach trap" is the ordinary case rather than the awkward one.
 *
 * thread_get_state() answers out of `pcb->user', the frame reserved at the top
 * of the thread's kernel stack.  On this machine the SYSCALL entry does not
 * honour that reservation: it takes %rsp from the stack TOP and runs its C
 * frames straight down through the 176 bytes the frame occupies (#474).  So
 * the answer is kernel stack contents, reported with KERN_SUCCESS.
 *
 * ⚠️ What is checked is cs and ss, and not the general registers, because
 * only the selectors have an answer this test can know in advance.  A thread
 * in ring 3 has cs = 0x2b and ss = 0x23 -- imposed by the kernel, never the
 * thread's to choose -- so anything else is not a ring-3 context at all.
 * Checking rip or rsp would mean predicting where libmach's mach_msg sits.
 *
 * ⚠️ And it is a DISCLOSURE as much as a wrong answer: those bytes are the
 * kernel's own stack, handed to ring 3 through a call that reports success.
 */
static volatile mach_port_t	arm_four_thread;
static mach_port_t		arm_four_port;

static void *
the_thread_that_gets_inspected(void *arg)
{
	struct roomy_msg	msg;

	(void) arg;

	arm_four_thread = mach_thread_self();

	/*
	 * Parked here for the rest of the run.  Nothing sends to this port and
	 * nothing interrupts it: the arm wants a thread that is definitely
	 * inside the trap while it is being asked about, and a receive that
	 * could end on its own would be a thread that might not be.
	 */
	(void) mach_msg(&msg.h, MACH_RCV_MSG, 0, sizeof msg, arm_four_port,
			MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	return NULL;
}

static int
arm_four_state_of_a_thread_in_a_trap(void)
{
	pthread_t			victim;
	struct x86_64_thread_state	state;
	mach_msg_type_number_t		count = x86_64_THREAD_STATE_COUNT;
	kern_return_t			kr;
	int				i;

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &arm_four_port);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [4] mach_port_allocate failed (%d) — WRONG\n",
		       kr);
		return 0;
	}

	if (pthread_create(&victim, NULL, the_thread_that_gets_inspected,
			   NULL) != 0) {
		printf("act_test: [4] pthread_create failed — WRONG\n");
		return 0;
	}

	for (i = 0; i < PATIENCE && arm_four_thread == MACH_PORT_NULL; i++)
		nap(100);
	if (arm_four_thread == MACH_PORT_NULL) {
		printf("act_test: [4] the thread never published its port "
		       "— WRONG\n");
		return 0;
	}

	/* Let it reach the receive and settle inside the trap. */
	nap(400);

	/*
	 * Suspended first, because that is what a debugger does and because an
	 * unsuspended thread could in principle be anywhere.  thread_suspend()
	 * returns once the thread has stopped, so what is read below is a
	 * thread that is stopped inside mach_msg.
	 */
	kr = thread_suspend(arm_four_thread);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [4] thread_suspend answered %d — WRONG\n", kr);
		return 0;
	}

	kr = thread_get_state(arm_four_thread, x86_64_THREAD_STATE,
			      (thread_state_t) &state, &count);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [4] thread_get_state answered %d — WRONG\n",
		       kr);
		(void) thread_resume(arm_four_thread);
		return 0;
	}

	(void) thread_resume(arm_four_thread);

	if (state.cs != 0x2bULL || state.ss != 0x23ULL) {
		printf("act_test: [4] a thread stopped inside mach_msg reports "
		       "cs 0x%llx ss 0x%llx, not 0x2b/0x23 — its saved frame is "
		       "kernel stack, and rip reads 0x%llx rsp 0x%llx — WRONG\n",
		       (unsigned long long) state.cs,
		       (unsigned long long) state.ss,
		       (unsigned long long) state.rip,
		       (unsigned long long) state.rsp);
		return 0;
	}

	/*
	 * ⚠️ And no general register may hold a KERNEL address.
	 *
	 * The selectors above say the frame describes ring 3; this says the
	 * rest of it is not the kernel's own stack.  The syscall entry does not
	 * fill the fifteen general registers -- the contract declares them
	 * destroyed -- so they hold whatever was there before, and
	 * thread_get_state() hands that over with KERN_SUCCESS.
	 *
	 * ⚠️ Two different defects have made this arm fire, and they are worth
	 * keeping apart.  The first was the frame overlapping the syscall's own
	 * C frames, so the bytes were literally kernel stack; that is what #474
	 * was opened for.  The second was the RETURN path handing the kernel's
	 * registers back to ring 3, which then arrived here through a later
	 * trap that saved them faithfully -- a correct frame with one wrong
	 * register.  The fifth arm catches the second on purpose; this one
	 * caught it one boot in thirty.
	 *
	 * The test is the upper half of the address space rather than a
	 * particular value, because what the kernel puts there is not
	 * predictable and does not need to be: any canonical high-half address
	 * in a register a user program just read is a kernel address it now
	 * knows, and that is the whole of the finding.  Checking for zeroes
	 * instead would be checking how the kernel cleans up rather than
	 * whether anything leaked.
	 */
	{
		const unsigned long long	*r = (const unsigned long long *) &state;
		const char			*name[16] = {
			"rax","rbx","rcx","rdx","rdi","rsi","rbp","rsp",
			"r8","r9","r10","r11","r12","r13","r14","r15"
		};
		int	i;

		for (i = 0; i < 16; i++) {
			if (r[i] < 0xffff800000000000ULL)
				continue;
			printf("act_test: [4] %s holds 0x%llx — a kernel address, "
			       "read out of the thread's saved frame by a call "
			       "that succeeded — WRONG\n", name[i], r[i]);
			return 0;
		}
	}

	printf("act_test: [4] a thread stopped inside mach_msg reports its own "
	       "ring-3 context: cs 0x%llx ss 0x%llx rip 0x%llx rsp 0x%llx, and "
	       "no general register holds a kernel address\n",
	       (unsigned long long) state.cs, (unsigned long long) state.ss,
	       (unsigned long long) state.rip, (unsigned long long) state.rsp);
	return 1;
}

/* ----------------------------------------------------------------
 * Arm six: returning a thread to an address the machine cannot resume (#411)
 * ---------------------------------------------------------------- */

/*
 * 🔥 One bit past the top of the canonical lower half.
 *
 * Not a kernel address and not garbage: bit 47 set with bits 63:48 clear is
 * the first value the processor refuses to load into %rip, and it is refused
 * for what it IS rather than for what is mapped there.  A test that used a
 * plausible-looking pointer would be indistinguishable from an ordinary page
 * fault, which is a different arm's subject.
 */
#define NONCANONICAL	((unsigned long long) 0x0000800000000000ULL)

static mach_port_t		arm_six_port;

/*
 * 🔥 Which processor this is, because it decides what a pass MEANS.
 *
 * SYSRET's fault-at-CPL-0 behaviour is Intel's.  On an AMD part the same
 * instruction with the same non-canonical %rcx faults in ring 3, where a fault
 * is ordinary -- so on AMD this arm passes whether the guard in entry.S is
 * there or not.  Measured, not assumed: with ablations/411-canonical.patch the
 * arm still reported six of six on an AMD model, and the same ablated kernel under
 * `-cpu Skylake-Client' died in a double fault with `instruction: sysret' and
 * the kernel standing on a ring-3 stack.
 *
 * ⚠️ So the vendor goes in the verdict.  A green line that means two different
 * things depending on the host, without saying which, is how a guard gets
 * removed again -- which is exactly what happened to #474's.
 *
 * CPUID leaf 0 is unprivileged and needs no library.
 */
static void
cpu_vendor(char out[13])
{
	unsigned int	a, b, c, d;

	__asm__ __volatile__("cpuid"
			     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
			     : "a"(0));
	((unsigned int *) out)[0] = b;
	((unsigned int *) out)[1] = d;
	((unsigned int *) out)[2] = c;
	out[12] = '\0';
}

static int
is_intel(const char *vendor)
{
	int	i;

	for (i = 0; i < 12; i++)
		if (vendor[i] != "GenuineIntel"[i])
			return 0;
	return 1;
}

static void *
the_thread_that_returns_badly(void *arg)
{
	struct roomy_msg	msg;

	(void) arg;

	arm_six_thread = mach_thread_self();

	/*
	 * ⚠️ A receive that WILL be satisfied, unlike arm four's.  The point of
	 * this arm is the ordinary way out of a Mach trap -- the syscall return
	 * path, with the frame this thread arrived with -- so the trap has to
	 * end the way traps normally end.  thread_abort() would get the thread
	 * out too and would be testing the special handler instead (arm two).
	 */
	(void) mach_msg(&msg.h, MACH_RCV_MSG, 0, sizeof msg, arm_six_port,
			MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

	/*
	 * Not reached on any kernel: this thread was told to resume at an
	 * address that cannot be resumed at.  If it IS reached, the state was
	 * never installed and the arm is measuring nothing -- which the summary
	 * has to be able to say, so the flag is set rather than the thread
	 * quietly returning.
	 */
	arm_six_named_the_victim = -1;
	return NULL;
}

/*
 * 🔥 What a user program can do to the machine with one register.
 *
 * SYSRET on Intel parts faults when %rcx is not canonical -- AFTER loading the
 * user's stack pointer and BEFORE leaving ring 0, so the fault handler runs at
 * CPL 0 on a stack the caller chose.  That is CVE-2012-0217, and entry.S
 * guards it: the return address is checked and a bad one is sent to the IRETQ
 * path instead.
 *
 * ⚠️ The guard has never been executed.  Its own comment calls it "a branch
 * never taken" and that is literally true -- nothing in this program, or any
 * other on this target, has ever made the check fail.  Everything past that
 * `jne' is assembled, linked and unproven, which is the shape that cost #453
 * and #459 five defects each.
 *
 * So this arm makes the check fail, deliberately, through the plainest route
 * there is: suspend a thread that is inside a Mach trap, write a non-canonical
 * rip into the frame it will return through, and let the trap finish.
 * `thread_set_state' is an ordinary right on an ordinary thread -- a debugger
 * does this all day -- so nothing here is privileged.
 *
 * ⚠️ What is being tested is OUR branch and what follows it, not the
 * processor's misbehaviour.  The guard diverts before SYSRET ever runs, so the
 * arm does not need the vendor difference and does not run only on the machine
 * nobody has.
 *
 * 🔴 IT DID SAY "the outcome is the same under TCG and under KVM", AND THAT
 * WAS FALSE.  It was measured, and the two disagreed completely: under KVM the
 * kernel halted, and under TCG the arm reported a pass without the path ever
 * being reached, because TCG does not raise the #GP that IRETQ owes a
 * non-canonical target.  For as long as that sentence stood, nobody ran this
 * arm anywhere it could fail -- and the halt it was written to catch was
 * reachable by any task with a thread of its own (#518).
 *
 * 🔑 Which is this comment's own warning, three paragraphs up, landing on this
 * comment: everything past that `jne' was assembled, linked and unproven, and
 * the sentence claiming otherwise is what kept it that way.  A test's
 * description of where it has run is part of the test.
 *
 * 🔥 And the expected end is not "no fault".  IRETQ refuses that address too;
 * the difference the mitigation buys is WHERE -- kernel stack still in place
 * rather than the caller's.  What has to be true is that the fault belongs to
 * the thread that asked for it: an exception delivered to the task, and a
 * machine still running afterwards.
 */
static int
arm_six_non_canonical_return(void)
{
	pthread_t			victim;
	struct x86_64_thread_state	state;
	mach_msg_type_number_t		count = x86_64_THREAD_STATE_COUNT;
	kern_return_t			kr;
	struct roomy_msg		wake;
	union {
		mach_msg_header_t	h;
		char			room[4096];
	} req, rep;
	char				vendor[13];
	int				i;

	cpu_vendor(vendor);

	if (exc_port == MACH_PORT_NULL) {
		printf("act_test: [6] no exception port — arm one did not get "
		       "far enough for this arm to mean anything — WRONG\n");
		return 0;
	}

	/*
	 * ⚠️ BAD_INSTRUCTION and not BAD_ACCESS: a general protection is what
	 * the processor raises here, and the kernel maps that vector to
	 * EXC_BAD_INSTRUCTION with EXC_X86_64_GPFLT.  Arm one's BAD_ACCESS
	 * registration is left alone -- this adds a mask, it does not replace
	 * the port.
	 */
	kr = task_set_exception_ports(mach_task_self(),
				      EXC_MASK_BAD_INSTRUCTION, exc_port,
				      EXCEPTION_DEFAULT, THREAD_STATE_NONE);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [6] task_set_exception_ports failed (%d)"
		       " — WRONG\n", kr);
		return 0;
	}

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&arm_six_port);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [6] mach_port_allocate failed (%d) — WRONG\n",
		       kr);
		return 0;
	}
	kr = mach_port_insert_right(mach_task_self(), arm_six_port,
				    arm_six_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [6] insert_right failed (%d) — WRONG\n", kr);
		return 0;
	}

	if (pthread_create(&victim, NULL, the_thread_that_returns_badly,
			   NULL) != 0) {
		printf("act_test: [6] pthread_create failed — WRONG\n");
		return 0;
	}

	for (i = 0; i < PATIENCE && arm_six_thread == MACH_PORT_NULL; i++)
		nap(100);
	if (arm_six_thread == MACH_PORT_NULL) {
		printf("act_test: [6] the thread never published its port "
		       "— WRONG\n");
		return 0;
	}
	nap(400);

	kr = thread_suspend(arm_six_thread);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [6] thread_suspend answered %d — WRONG\n", kr);
		return 0;
	}

	kr = thread_get_state(arm_six_thread, x86_64_THREAD_STATE,
			      (thread_state_t) &state, &count);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [6] thread_get_state answered %d — WRONG\n",
		       kr);
		(void) thread_resume(arm_six_thread);
		return 0;
	}

	/*
	 * ⚠️ Asked before poisoning it.  If the frame did not describe ring 3
	 * to begin with, everything below is being done to something other than
	 * a user context and a passing result would mean nothing.  Arm four
	 * makes this its whole subject; here it is the precondition.
	 */
	if (state.cs != 0x2bULL || state.ss != 0x23ULL) {
		printf("act_test: [6] the thread's frame is not a ring-3 one "
		       "(cs 0x%llx ss 0x%llx) — nothing to poison — WRONG\n",
		       (unsigned long long) state.cs,
		       (unsigned long long) state.ss);
		(void) thread_resume(arm_six_thread);
		return 0;
	}

	state.rip = NONCANONICAL;
	kr = thread_set_state(arm_six_thread, x86_64_THREAD_STATE,
			      (thread_state_t) &state, count);
	if (kr != KERN_SUCCESS) {
		/*
		 * ⚠️ Not a pass.  Refusing the state IS a defensible answer to
		 * this hazard -- but it is a different one from the guard in
		 * entry.S, it would leave that guard still unexecuted, and it
		 * is not what this kernel claims to do.  Reported as itself.
		 */
		printf("act_test: [6] thread_set_state refused a non-canonical "
		       "rip with %d — the guard in entry.S is still unproven "
		       "— WRONG\n", kr);
		(void) thread_resume(arm_six_thread);
		return 0;
	}

	arm_six_armed = 1;

	/*
	 * 🔥 Announced BEFORE it is provoked, and this line is not decoration.
	 *
	 * If the kernel takes the general protection on its own iretq and stops
	 * the machine, nothing after this point prints -- not this arm's
	 * verdict, not the summary, not the tasks that come after this one in
	 * the bundle.  A boot log ending here says exactly what was being done
	 * when it ended.  A boot log ending in silence would have cost a day.
	 */
	printf("act_test: [6] resuming a thread whose frame says rip 0x%llx "
	       "(cpu %s); if the machine stops here, that is the answer\n",
	       NONCANONICAL, vendor);

	/*
	 * The receive is satisfied rather than interrupted, so the thread
	 * leaves the trap the way every thread leaves a trap.
	 */
	wake.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	wake.h.msgh_size = sizeof wake.h;
	wake.h.msgh_remote_port = arm_six_port;
	wake.h.msgh_local_port = MACH_PORT_NULL;
	wake.h.msgh_id = 0;
	kr = mach_msg(&wake.h, MACH_SEND_MSG, sizeof wake.h, 0,
		      MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [6] could not wake the thread (%d) — WRONG\n",
		       kr);
		(void) thread_resume(arm_six_thread);
		return 0;
	}

	(void) thread_resume(arm_six_thread);

	/*
	 * ⚠️ A receive with a bound, not mach_msg_server_once.
	 *
	 * The server helper has no timeout, so on a kernel that delivers
	 * nothing this arm would wait forever and take the summary of the five
	 * arms before it down with it.  A new arm must not be able to hide the
	 * ones that already passed.
	 */
	kr = mach_msg(&req.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof req,
		      exc_port, 4000, MACH_PORT_NULL);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [6] no exception arrived within four seconds "
		       "(%d) — the thread was resumed at an address that cannot "
		       "be resumed at and the kernel said nothing — WRONG\n", kr);
		return 0;
	}

	(void) exc_server(&req.h, &rep.h);
	if (rep.h.msgh_remote_port != MACH_PORT_NULL)
		(void) mach_msg(&rep.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
				rep.h.msgh_size, 0, MACH_PORT_NULL, 1000,
				MACH_PORT_NULL);

	(void) mach_port_destroy(mach_task_self(), arm_six_port);
	(void) mach_port_deallocate(mach_task_self(), arm_six_thread);

	if (arm_six_named_the_victim == -1) {
		printf("act_test: [6] the thread returned from its trap and ran "
		       "on — the frame it resumed through was not the one that "
		       "was written — WRONG\n");
		return 0;
	}
	if (!arm_six_named_the_victim) {
		printf("act_test: [6] the exception named a thread that is not "
		       "the one that was poisoned — WRONG\n");
		return 0;
	}
	if (arm_six_exception != EXC_BAD_INSTRUCTION) {
		printf("act_test: [6] the exception was %d, not "
		       "EXC_BAD_INSTRUCTION (%d) — WRONG\n",
		       arm_six_exception, EXC_BAD_INSTRUCTION);
		return 0;
	}

	printf("act_test: [6] a thread told to resume at 0x%llx raised "
	       "EXC_BAD_INSTRUCTION code %d against itself, and the machine is "
	       "still running to say so — cpu %s%s\n",
	       NONCANONICAL, arm_six_code, vendor,
	       is_intel(vendor) ? ", where an unguarded sysret would have died"
			        : ", where sysret does not fault at CPL 0: this "
				  "shows the outcome and not the guard");
	return 1;
}

/* ----------------------------------------------------------------
 * Arm seven: suspend a thread that lives in page faults (#603)
 * ----------------------------------------------------------------
 *
 * The arms above stop a thread inside a system call or an exception, and both
 * of those leave the kernel through a path that takes pending ASTs.  A page
 * fault the kernel resolves never reaches the task: vm_fault() maps the page
 * and the instruction runs again -- and on x86-64 that return did not look at
 * ASTs.  A thread suspended while inside one went back to its own code and ran
 * on, to the next tick taken in ring 3 or its next system call, with
 * thread_suspend() long since answered.
 *
 * The thread below does nothing but take such faults: it touches a fresh page,
 * counts it, touches the next, a megabyte at a time, allocated and given back
 * with a system call at each end.  A system call's own check is therefore at
 * most ARM7_PAGES touches away.
 *
 * ⚠️ The count inside the call includes the time before the request reaches
 * the thread -- the system call into the kernel, and the interrupt to the
 * other processor -- in which it is entitled to run.  At several processors
 * under TCG that was up to 49 pages with every fix in, against a threshold of
 * 16, and a 256-page batch let a thread that ran on show no more than 256.
 * So the batch is 4096 pages and the threshold 256: what a thread that runs
 * on reaches is bounded by the batch, what one that stops reaches is bounded
 * by time, and a larger batch moves only the first.  And the same count is
 * taken around a call of the same shape that stops nothing -- thread_info()
 * on the same thread -- and printed beside it, so the line says how much of
 * what it counted was that time.
 *
 * ⚠️ Counted DURING thread_suspend(), from just before the call to its
 * answer -- not after it.  thread_suspend() ends in thread_wait(), which
 * returns only once the thread is no longer running, so a count taken after
 * the answer is zero whether the thread stopped at once or ran on to the end
 * of its megabyte first: the first version of this arm counted there, and
 * passed on the kernel it was written to fail.  What the defect costs is the
 * user code run between nudge() and the parking -- nudge() promises none --
 * and that is inside the call.  One that stops touches at most the page it
 * was in the middle of; one that runs on reaches the end of its megabyte.  And
 * after the answer, nothing at all, whichever it was.
 *
 * 🔴 (07/10) BUT THE COUNT INSIDE THE CALL IS NOT THE STOP'S LATENCY.  It
 * also holds the time before the stop is asked: the call's way into the
 * kernel, and whatever keeps the asker from getting there -- a processor
 * the host stops under TCG, or a quantum given to another thread.  With a
 * 20 ms wait put in thread_suspend() before the asking, the stop path
 * untouched, this arm failed in 4 boots of 4 at up to 4096 pages, and the
 * kernel saw every stop within two faults of its asking; three boots under
 * TCG on GitHub had failed it at 258 to 301 pages, about a tick of faults.
 * So the count is printed, and the stop is judged by the kernel, which
 * counts from the asking: the thread_suspend line of the special handler in
 * kern/thread_act.c, WRONG past two faults.  What is still judged here is the
 * promise thread_suspend() makes to its caller: nothing after its answer.
 *
 * ⚠️ And this arm sees that promise broken only when the stop is late as
 * well.  With thread_wait() taken out of thread_suspend() and the stop as
 * prompt as the fix makes it, the thread stopped within 1 to 4 pages of the
 * asking, before this thread read its count, in 4 boots of 4; with the stop
 * held late too, the arm said WRONG in 18 to 20 rounds of 20, in 4 boots of
 * 4 (ablations/603-suspend-no-wait.patch).
 */
#define	ARM7_PAGES	4096
#define	ARM7_ROUNDS	20
#define	ARM7_RAN_ON	256	/* rounds past it are counted and printed */

static volatile mach_port_t	arm_seven_thread;
static volatile unsigned long	arm_seven_touches;
static volatile int		arm_seven_stop;
static volatile kern_return_t	arm_seven_kr = KERN_SUCCESS;

static void *
the_thread_that_lives_in_page_faults(void *arg)
{
	(void) arg;

	arm_seven_thread = mach_thread_self();
	while (!arm_seven_stop) {
		vm_address_t	base = 0;
		unsigned	i;

		arm_seven_kr = vm_allocate(mach_task_self(), &base,
					   ARM7_PAGES * vm_page_size, TRUE);
		if (arm_seven_kr != KERN_SUCCESS)
			break;
		for (i = 0; i < ARM7_PAGES && !arm_seven_stop; i++) {
			((volatile char *) base)[i * vm_page_size] = 1;
			arm_seven_touches++;
		}
		(void) vm_deallocate(mach_task_self(), base,
				     ARM7_PAGES * vm_page_size);
	}
	return NULL;
}

static int
arm_seven_suspend_in_page_faults(void)
{
	pthread_t	victim;
	unsigned long	asked, answered, later, most = 0, yardstick = 0;
	int		i, round, ran_on = 0, ran_after = 0;
	kern_return_t	kr;

	if (pthread_create(&victim, NULL, the_thread_that_lives_in_page_faults,
			   NULL) != 0) {
		printf("act_test: [7] pthread_create failed — WRONG\n");
		return 0;
	}
	for (i = 0; i < PATIENCE && arm_seven_touches == 0; i++)
		nap(100);
	if (arm_seven_touches == 0) {
		printf("act_test: [7] the thread never touched a page, its "
		       "vm_allocate answering %d — WRONG\n", (int) arm_seven_kr);
		arm_seven_stop = 1;
		return 0;
	}

	for (round = 0; round < ARM7_ROUNDS; round++) {
		struct thread_basic_info	info;
		mach_msg_type_number_t		count = THREAD_BASIC_INFO_COUNT;

		asked = arm_seven_touches;
		(void) thread_info(arm_seven_thread, THREAD_BASIC_INFO,
				   (thread_info_t) &info, &count);
		answered = arm_seven_touches;
		if (answered - asked > yardstick)
			yardstick = answered - asked;

		asked = arm_seven_touches;
		kr = thread_suspend(arm_seven_thread);
		answered = arm_seven_touches;
		if (kr != KERN_SUCCESS) {
			printf("act_test: [7] thread_suspend answered %d — "
			       "WRONG\n", kr);
			arm_seven_stop = 1;
			(void) thread_resume(arm_seven_thread);
			return 0;
		}
		nap(30);
		later = arm_seven_touches;
		if (answered - asked > most)
			most = answered - asked;
		if (answered - asked > ARM7_RAN_ON)
			ran_on++;
		if (later != answered)
			ran_after++;
		(void) thread_resume(arm_seven_thread);
		nap(10);		/* back into its faults */
	}
	arm_seven_stop = 1;
	(void) pthread_join(victim, NULL);

	if (ran_after != 0) {
		printf("act_test: [7] in %d of %d rounds the thread touched "
		       "pages after thread_suspend had answered — WRONG (#603)\n",
		       ran_after, ARM7_ROUNDS);
		return 0;
	}
	/*
	 * ⚠️ Under 256 bytes: a longer line leaves in two writes, and another
	 * task's line landed between them once (#578).  The kernel's
	 * thread_suspend line is the one that judges the stop.
	 */
	printf("act_test: [7] a thread that lives in page faults stopped when "
	       "asked: %d rounds, none after thread_suspend answered; inside "
	       "it at most %lu pages (%lu in thread_info), past %u in %d, the "
	       "wait for the asking included (#603)\n", ARM7_ROUNDS, most,
	       yardstick, ARM7_RAN_ON, ran_on);
	return 1;
}

/* ----------------------------------------------------------------
 * Arm eight: suspend a thread that spins, at one processor (#603)
 * ----------------------------------------------------------------
 *
 * The same promise, broken the other way.  A thread that only spins leaves the
 * processor at a tick, from ring 3, blocked inside ast_taken() -- whose
 * SAFE_EXCEPTION_RETURN is a sentinel and not a continuation, so the thread
 * resumes right there.  A thread_suspend() issued while it was off the
 * processor is set on its processor when it comes back, and x86-64 took ASTs
 * once on that return where i386 loops back into the check: the thread spun
 * on to the next tick.
 *
 * At one processor the answer is exact.  The suspender runs only while the
 * spinner is off the processor, so the count it reads just before
 * thread_suspend() is the spinner's last; a spinner that stops at its first
 * return never adds to it, and one that goes back to ring 3 spins until its
 * next tick while thread_suspend() -- which ends in thread_wait() -- waits
 * for it to stop.  Counted inside the call, for arm seven's reason.  At more
 * than one processor the suspend reaches a running spinner by an interrupt,
 * whose return takes the AST even in one pass: the arm asks nothing there,
 * and says so.
 */
#define	ARM8_ROUNDS	10
#define	ARM8_NOT_ASKED	(-1)

static volatile mach_port_t	arm_eight_thread;
static volatile unsigned long	arm_eight_spins;
static volatile int		arm_eight_stop;

static void *
the_thread_that_spins(void *arg)
{
	(void) arg;

	arm_eight_thread = mach_thread_self();
	while (!arm_eight_stop)
		arm_eight_spins++;
	return NULL;
}

static unsigned
processors(void)
{
	host_basic_info_data_t	hi;
	mach_msg_type_number_t	c = HOST_BASIC_INFO_COUNT;

	if (host_info(mach_host_self(), HOST_BASIC_INFO, (host_info_t) &hi,
		      &c) != KERN_SUCCESS)
		return 0;
	return hi.avail_cpus > 0 ? (unsigned) hi.avail_cpus : 0;
}

static int
arm_eight_suspend_a_spinner(void)
{
	pthread_t	spinner;
	unsigned long	asked, answered, later, most = 0;
	unsigned	ncpu = processors();
	int		i, round, ran_on = 0, ran_after = 0;
	kern_return_t	kr;

	if (ncpu != 1) {
		printf("act_test: [8] NOT ASKED — %u processors: a suspend "
		       "reaches a running spinner by an interrupt, whose return "
		       "takes the AST in one pass, and the question is about a "
		       "spinner that was off its processor (#603)\n", ncpu);
		return ARM8_NOT_ASKED;
	}

	if (pthread_create(&spinner, NULL, the_thread_that_spins, NULL) != 0) {
		printf("act_test: [8] pthread_create failed — WRONG\n");
		return 0;
	}
	for (i = 0; i < PATIENCE && arm_eight_spins == 0; i++)
		nap(100);
	if (arm_eight_spins == 0) {
		printf("act_test: [8] the spinner never ran — WRONG\n");
		arm_eight_stop = 1;
		return 0;
	}

	for (round = 0; round < ARM8_ROUNDS; round++) {
		nap(20);		/* it runs; this comes back at a tick */
		asked = arm_eight_spins;
		kr = thread_suspend(arm_eight_thread);
		answered = arm_eight_spins;
		if (kr != KERN_SUCCESS) {
			printf("act_test: [8] thread_suspend answered %d — "
			       "WRONG\n", kr);
			arm_eight_stop = 1;
			(void) thread_resume(arm_eight_thread);
			return 0;
		}
		nap(30);
		later = arm_eight_spins;
		if (answered - asked > most)
			most = answered - asked;
		if (answered != asked)
			ran_on++;
		if (later != answered)
			ran_after++;
		(void) thread_resume(arm_eight_thread);
	}
	arm_eight_stop = 1;
	(void) pthread_join(spinner, NULL);

	if (ran_after != 0) {
		printf("act_test: [8] in %d of %d rounds the spinner turned "
		       "after thread_suspend had answered — WRONG (#603)\n",
		       ran_after, ARM8_ROUNDS);
		return 0;
	}
	if (ran_on != 0) {
		printf("act_test: [8] in %d of %d rounds a spinner asked to stop "
		       "while it was off its processor spun on: up to %lu turns "
		       "inside thread_suspend — WRONG (#603)\n",
		       ran_on, ARM8_ROUNDS, most);
		return 0;
	}
	printf("act_test: [8] a spinner asked to stop while it was off its "
	       "processor never turned again: %d rounds at one processor "
	       "(#603)\n", ARM8_ROUNDS);
	return 1;
}

/*
 * ── Arm nine: the threads a refused thread_create_running() made are freed ─
 *
 * thread_create_running() makes the thread first and sets its state second, so
 * a flavor that does not exist is refused by a thread that already exists, and
 * the kernel ends it before it answers.  Until #660 that end left the
 * activation the reference thread_create() had given its creator: the thread,
 * its activation and its task all outlived the task, 200 of them in every
 * boot, one for each refused call in pthread_test's sweep.
 *
 * A thread leaves the processor set's count only when its last reference goes,
 * which is where the leak was.  So the count is read, ARM9_CALLS refused calls
 * are made in a child task, and the count must come back.  The other programs
 * of the bundle make and end threads meanwhile, so it must come back to within
 * half of what was made, not to the thread: a leak keeps all of them.
 */
#define ARM9_CALLS	300
#define ARM9_WAIT_MS	2000

static kern_return_t
arm_nine_thread_count(mach_port_t pset, int *count)
{
	struct processor_set_load_info	li;
	mach_msg_type_number_t		n = PROCESSOR_SET_LOAD_INFO_COUNT;
	kern_return_t			kr;

	kr = processor_set_statistics(pset, PROCESSOR_SET_LOAD_INFO,
				      (processor_set_info_t) &li, &n);
	if (kr == KERN_SUCCESS)
		*count = li.thread_count;
	return kr;
}

static int
arm_nine_refused_create_running(void)
{
	mach_port_t	me = mach_task_self();
	mach_port_t	host = MACH_PORT_NULL, host_priv = MACH_PORT_NULL;
	mach_port_t	device = MACH_PORT_NULL, lw = MACH_PORT_NULL;
	mach_port_t	lp = MACH_PORT_NULL, sec = MACH_PORT_NULL;
	mach_port_t	name = MACH_PORT_NULL, pset = MACH_PORT_NULL;
	mach_port_t	child = MACH_PORT_NULL;
	natural_t	st[4] = { 0, 0, 0, 0 };
	int		before = 0, after = 0, refused = 0, waited = 0, i;
	int		passed = 0;
	kern_return_t	kr;

	kr = bootstrap_ports(bootstrap_port, &host_priv, &device, &lw, &lp,
			     &sec);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [9] no privileged host port from bootstrap "
		       "(%d) — WRONG\n", kr);
		return 0;
	}
	host = mach_host_self();
	kr = processor_set_default(host, &name);
	if (kr == KERN_SUCCESS)
		kr = host_processor_set_priv(host_priv, name, &pset);
	if (kr == KERN_SUCCESS)
		kr = task_create(me, NULL, 0, FALSE, &child);
	if (kr == KERN_SUCCESS)
		kr = arm_nine_thread_count(pset, &before);
	if (kr != KERN_SUCCESS)
		printf("act_test: [9] no processor set or child task to count "
		       "with (%d) — WRONG\n", kr);
	if (kr == KERN_SUCCESS) {
		for (i = 0; i < ARM9_CALLS; i++) {
			mach_port_t	th = MACH_PORT_NULL;

			kr = thread_create_running(child, 0x7fffffff, st, 4,
						   &th);
			if (kr != KERN_SUCCESS) {
				refused++;
				continue;
			}
			(void) thread_terminate(th);
			(void) mach_port_deallocate(me, th);
		}

		/*
		 * The reaper frees each thread once it has stopped running,
		 * so the count is asked again until it comes back or the
		 * patience runs out.  A nap that does not sleep would spend
		 * that patience in microseconds, so it is checked.
		 */
		kr = arm_nine_thread_count(pset, &after);
		while (kr == KERN_SUCCESS && after - before >= ARM9_CALLS / 2 &&
		       waited < ARM9_WAIT_MS) {
			if (nap(10) != MACH_RCV_TIMED_OUT)
				break;
			waited += 10;
			kr = arm_nine_thread_count(pset, &after);
		}

		if (refused != ARM9_CALLS)
			printf("act_test: [9] a flavor that does not exist was "
			       "accepted by %d of %d thread_create_running "
			       "calls — WRONG (#660)\n", ARM9_CALLS - refused,
			       ARM9_CALLS);
		else if (kr != KERN_SUCCESS)
			printf("act_test: [9] the processor set's count could "
			       "not be read again (%d) — WRONG (#660)\n", kr);
		else if (after - before >= ARM9_CALLS / 2)
			printf("act_test: [9] %d refused thread_create_running "
			       "calls: the processor set held %d threads before "
			       "and %d after %d ms, so the threads they made are "
			       "still held — WRONG (#660)\n", ARM9_CALLS, before,
			       after, waited);
		else {
			printf("act_test: [9] %d refused thread_create_running "
			       "calls: the processor set held %d threads before "
			       "and %d after %d ms, so the threads they made were "
			       "freed (#660)\n", ARM9_CALLS, before, after,
			       waited);
			passed = 1;
		}
	}

	if (child != MACH_PORT_NULL) {
		(void) task_terminate(child);
		(void) mach_port_deallocate(me, child);
	}
	if (pset != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, pset);
	if (name != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, name);
	if (host != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, host);
	if (host_priv != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, host_priv);
	if (device != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, device);
	if (lw != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, lw);
	if (lp != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, lp);
	if (sec != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, sec);
	return passed;
}

/*
 * ── Arm ten: abort an RPC whose reply is already on its way (#687) ─────
 *
 * A thread in an RPC that took the hot path is parked by mach_msg itself, on
 * its reply port's queue, and its reply comes one of two ways: the server's own
 * RPC hands it over directly, or a plain send goes through ipc_mqueue_deliver(),
 * which takes the thread off the queue and writes MACH_MSG_SUCCESS under the
 * queue's lock, and wakes it after.  An abort ends the same wait under
 * thread_lock alone.  When the two cross, the thread runs with the message
 * already handed to it and its wakeup still owed -- and that wakeup, when it
 * comes, ends whatever wait the thread is in by then.  #687 is that wakeup
 * ending the thread's NEXT RPC: "mmot_hotpath: bad wait result 0 (ith_state
 * 10004001)", awake with nothing delivered.
 *
 * So the arm makes the two cross and goes straight into a second RPC.  The
 * first server takes the request and holds it; the thread is aborted; the
 * server is told to answer at once, with a plain send; and the thread, back
 * from the first RPC however it came back, asks a second server, which sits
 * on its request for ARM10_HOLD_MS.  Each RPC must end with its own reply,
 * once.
 *
 * ⚠️ The window is a few instructions wide, so a boot almost never crosses the
 * two on its own and the arm passes either way: what makes it a test is
 * ablations/687-widen-aborted-rpc.patch, which holds both sides of the window
 * open.  Without it, this is the abort-meets-reply path of #387 asked once a
 * boot, which nothing else in the bundle asks.
 */
#define ARM10_REQUEST_ID	687
#define ARM10_REPLY_ID		688
#define ARM10_GO_ID		689
#define ARM10_SECOND_ID		690
#define ARM10_SECOND_REPLY_ID	691

/*
 * Long enough that the thread is still parked in its second RPC when a wakeup
 * the widen holds open arrives: the widen holds it 300 ms after the reply.
 */
#define ARM10_HOLD_MS		500

static mach_port_t		arm_ten_server_port;	/* the first RPC asks here */
static mach_port_t		arm_ten_go_port;	/* and its server waits here */
static mach_port_t		arm_ten_second_port;	/* the second RPC asks here */
static mach_port_t		arm_ten_reply_port;
static mach_port_t		arm_ten_second_reply_port;
static volatile mach_port_t	arm_ten_thread;
static volatile int		arm_ten_held;
static volatile int		arm_ten_done;
static volatile kern_return_t	arm_ten_first_kr = -1;
static volatile int		arm_ten_first_id;
static volatile kern_return_t	arm_ten_after_kr = -1;	/* after an interruption */
static volatile int		arm_ten_after_id;
static volatile kern_return_t	arm_ten_second_kr = -1;
static volatile int		arm_ten_second_id;
static volatile kern_return_t	arm_ten_answer_kr = -1;
static volatile kern_return_t	arm_ten_second_answer_kr = -1;

static kern_return_t
arm_ten_port(mach_port_t *name, int with_send)
{
	kern_return_t	kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				name);
	if (kr == KERN_SUCCESS && with_send)
		kr = mach_port_insert_right(mach_task_self(), *name, *name,
					    MACH_MSG_TYPE_MAKE_SEND);
	return kr;
}

/* A request shaped the way the hot path takes it: an RPC as MIG makes one. */
static void
arm_ten_request(mach_msg_header_t *h, mach_port_t to, mach_port_t reply,
		int id)
{
	h->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
				      MACH_MSG_TYPE_MAKE_SEND_ONCE);
	h->msgh_size = sizeof *h;
	h->msgh_remote_port = to;
	h->msgh_local_port = reply;
	h->msgh_reserved = 0;
	h->msgh_id = id;
}

/*
 * An answer by plain send: never the hot path, which only an RPC takes, so
 * always the delivery that takes the thread off the queue and wakes it after.
 */
static kern_return_t
arm_ten_answer(mach_msg_header_t *req, int id)
{
	mach_msg_header_t	rep;

	rep.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
	rep.msgh_size = sizeof rep;
	rep.msgh_remote_port = req->msgh_remote_port;
	rep.msgh_local_port = MACH_PORT_NULL;
	rep.msgh_reserved = 0;
	rep.msgh_id = id;
	return mach_msg(&rep, MACH_SEND_MSG, sizeof rep, 0, MACH_PORT_NULL,
			MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

static void *
the_server_that_answers_when_told(void *arg)
{
	struct roomy_msg	req, go;

	(void) arg;

	if (mach_msg(&req.h, MACH_RCV_MSG, 0, sizeof req, arm_ten_server_port,
		     MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != MACH_MSG_SUCCESS)
		return NULL;
	arm_ten_held = 1;

	if (mach_msg(&go.h, MACH_RCV_MSG, 0, sizeof go, arm_ten_go_port,
		     MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != MACH_MSG_SUCCESS)
		return NULL;
	arm_ten_answer_kr = arm_ten_answer(&req.h, ARM10_REPLY_ID);
	return NULL;
}

static void *
the_server_that_sits_on_its_request(void *arg)
{
	struct roomy_msg	req;

	(void) arg;

	if (mach_msg(&req.h, MACH_RCV_MSG, 0, sizeof req, arm_ten_second_port,
		     MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != MACH_MSG_SUCCESS)
		return NULL;
	nap(ARM10_HOLD_MS);
	arm_ten_second_answer_kr = arm_ten_answer(&req.h,
						  ARM10_SECOND_REPLY_ID);
	return NULL;
}

static void *
the_thread_whose_rpc_is_aborted(void *arg)
{
	struct roomy_msg	m;

	(void) arg;

	arm_ten_thread = mach_thread_self();

	/*
	 * ⚠️ MACH_RCV_INTERRUPT is libmach's, and it keeps the bit from the
	 * kernel (LIBMACH_OPTIONS), so the trap is still the plain RPC the hot
	 * path takes.  What the bit buys is the answer: an RPC the abort
	 * interrupted comes back MACH_RCV_INTERRUPTED instead of being
	 * re-issued, and then the reply is still to come and is waited for
	 * here.
	 */
	arm_ten_request(&m.h, arm_ten_server_port, arm_ten_reply_port,
			ARM10_REQUEST_ID);
	arm_ten_first_kr = mach_msg(&m.h, MACH_SEND_MSG | MACH_RCV_MSG
				    | MACH_RCV_INTERRUPT, sizeof m.h, sizeof m,
				    arm_ten_reply_port, MACH_MSG_TIMEOUT_NONE,
				    MACH_PORT_NULL);
	if (arm_ten_first_kr == MACH_MSG_SUCCESS)
		arm_ten_first_id = m.h.msgh_id;
	else if (arm_ten_first_kr == MACH_RCV_INTERRUPTED) {
		arm_ten_after_kr = mach_msg(&m.h, MACH_RCV_MSG
					    | MACH_RCV_TIMEOUT, 0, sizeof m,
					    arm_ten_reply_port, PATIENCE * 100,
					    MACH_PORT_NULL);
		if (arm_ten_after_kr == MACH_MSG_SUCCESS)
			arm_ten_after_id = m.h.msgh_id;
	}

	/*
	 * 🔴 STRAIGHT INTO THE NEXT RPC, and that is the arm.  A wakeup still
	 * owed for the first reply ends whatever wait this thread is in when
	 * it comes, and this is the wait #687 saw it end.
	 */
	arm_ten_request(&m.h, arm_ten_second_port, arm_ten_second_reply_port,
			ARM10_SECOND_ID);
	arm_ten_second_kr = mach_msg(&m.h, MACH_SEND_MSG | MACH_RCV_MSG,
				     sizeof m.h, sizeof m,
				     arm_ten_second_reply_port,
				     MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (arm_ten_second_kr == MACH_MSG_SUCCESS)
		arm_ten_second_id = m.h.msgh_id;
	arm_ten_done = 1;
	return NULL;
}

static int
arm_ten_abort_an_rpc_with_its_reply_on_the_way(void)
{
	pthread_t		server, second, victim;
	mach_msg_header_t	go;
	struct roomy_msg	extra;
	kern_return_t		kr, abort_kr, extra_kr;
	const char		*how;
	int			i;

	kr = arm_ten_port(&arm_ten_server_port, 1);
	if (kr == KERN_SUCCESS)
		kr = arm_ten_port(&arm_ten_go_port, 1);
	if (kr == KERN_SUCCESS)
		kr = arm_ten_port(&arm_ten_second_port, 1);
	if (kr == KERN_SUCCESS)
		kr = arm_ten_port(&arm_ten_reply_port, 0);
	if (kr == KERN_SUCCESS)
		kr = arm_ten_port(&arm_ten_second_reply_port, 0);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [10] the ports for two RPCs could not be made "
		       "(%d) — WRONG\n", kr);
		return 0;
	}

	if (pthread_create(&server, NULL, the_server_that_answers_when_told,
			   NULL) != 0 ||
	    pthread_create(&second, NULL, the_server_that_sits_on_its_request,
			   NULL) != 0) {
		printf("act_test: [10] pthread_create failed for a server "
		       "— WRONG\n");
		return 0;
	}

	/*
	 * Let both servers reach their receives.  A request that finds no
	 * server waiting is queued, and the RPC then waits on the slow path,
	 * which reads its reply under the queue's lock: this arm would ask
	 * nothing there.
	 */
	nap(300);

	if (pthread_create(&victim, NULL, the_thread_whose_rpc_is_aborted,
			   NULL) != 0) {
		printf("act_test: [10] pthread_create failed for the thread "
		       "— WRONG\n");
		return 0;
	}

	/*
	 * The server holds the request, so the thread is parked: the hot path
	 * parks it on the reply port before it switches to the server, and
	 * the server cannot have the request before that switch.
	 */
	for (i = 0; i < PATIENCE && !arm_ten_held; i++)
		nap(100);
	if (!arm_ten_held) {
		printf("act_test: [10] the first server never got the request "
		       "— WRONG\n");
		return 0;
	}

	abort_kr = thread_abort(arm_ten_thread);

	go.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	go.msgh_size = sizeof go;
	go.msgh_remote_port = arm_ten_go_port;
	go.msgh_local_port = MACH_PORT_NULL;
	go.msgh_reserved = 0;
	go.msgh_id = ARM10_GO_ID;
	kr = mach_msg(&go, MACH_SEND_MSG, sizeof go, 0, MACH_PORT_NULL,
		      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS) {
		printf("act_test: [10] the first server could not be told to "
		       "answer (0x%x) — WRONG\n", (int) kr);
		return 0;
	}

	/* Twice the patience: the thread may itself wait PATIENCE for a reply. */
	for (i = 0; i < 2 * PATIENCE && !arm_ten_done; i++)
		nap(100);
	if (!arm_ten_done) {
		printf("act_test: [10] the aborted thread never came back from "
		       "its two RPCs: the first answered 0x%x, the second 0x%x "
		       "— WRONG\n", (int) arm_ten_first_kr,
		       (int) arm_ten_second_kr);
		return 0;
	}
	(void) pthread_join(victim, NULL);
	(void) pthread_join(server, NULL);
	(void) pthread_join(second, NULL);

	if (abort_kr != KERN_SUCCESS) {
		printf("act_test: [10] thread_abort answered %d on a thread "
		       "parked in an RPC — WRONG\n", abort_kr);
		return 0;
	}
	if (arm_ten_answer_kr != MACH_MSG_SUCCESS ||
	    arm_ten_second_answer_kr != MACH_MSG_SUCCESS) {
		printf("act_test: [10] a server's answer was refused: the first "
		       "0x%x, the second 0x%x — WRONG\n",
		       (int) arm_ten_answer_kr,
		       (int) arm_ten_second_answer_kr);
		return 0;
	}

	if (arm_ten_first_kr == MACH_MSG_SUCCESS &&
	    arm_ten_first_id == ARM10_REPLY_ID)
		how = "took the reply";
	else if (arm_ten_first_kr == MACH_RCV_INTERRUPTED &&
		 arm_ten_after_kr == MACH_MSG_SUCCESS &&
		 arm_ten_after_id == ARM10_REPLY_ID)
		how = "came back interrupted, and the reply was there after";
	else {
		printf("act_test: [10] the aborted RPC answered 0x%x with id %d, "
		       "and a receive after it 0x%x with id %d, instead of its "
		       "reply (id %d) — WRONG\n", (int) arm_ten_first_kr,
		       arm_ten_first_id, (int) arm_ten_after_kr,
		       arm_ten_after_id, ARM10_REPLY_ID);
		return 0;
	}

	if (arm_ten_second_kr != MACH_MSG_SUCCESS ||
	    arm_ten_second_id != ARM10_SECOND_REPLY_ID) {
		printf("act_test: [10] the RPC after the aborted one answered "
		       "0x%x with id %d instead of its own reply (id %d) "
		       "— WRONG\n", (int) arm_ten_second_kr,
		       arm_ten_second_id, ARM10_SECOND_REPLY_ID);
		return 0;
	}

	/* Once: nothing more may be waiting on the first reply port. */
	extra_kr = mach_msg(&extra.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			    sizeof extra, arm_ten_reply_port, 0,
			    MACH_PORT_NULL);
	if (extra_kr != MACH_RCV_TIMED_OUT) {
		printf("act_test: [10] after the aborted RPC had its reply, its "
		       "reply port still answered 0x%x — WRONG\n",
		       (int) extra_kr);
		return 0;
	}

	(void) mach_port_destroy(mach_task_self(), arm_ten_server_port);
	(void) mach_port_destroy(mach_task_self(), arm_ten_go_port);
	(void) mach_port_destroy(mach_task_self(), arm_ten_second_port);
	(void) mach_port_destroy(mach_task_self(), arm_ten_reply_port);
	(void) mach_port_destroy(mach_task_self(), arm_ten_second_reply_port);

	printf("act_test: [10] an RPC aborted with its reply on the way %s, "
	       "and the RPC after it got its own reply (#687)\n", how);
	return 1;
}

/*
 * ── Arm eleven: abort a thread that never stops asking (#687) ──────────
 *
 * thread_abort() interrupts a wait that is outstanding, and for a thread that
 * is not waiting yet it leaves TH_ABORT, so that the next interruptible wait
 * ends at once -- thread_block_reason() sees to that.  The hot path parks a
 * thread itself, without thread_block(), and it asserted that no abort was
 * pending: any task could make that false by aborting one of its own threads
 * in the middle of an RPC, and the kernel's own assert stopped the machine.
 *
 * Arm two aborts a thread that is certainly asleep.  This one aborts, for as
 * long as it lasts, a thread that mostly is NOT, while it makes ARM11_RPCS RPCs
 * to a server that answers each in the same call that waits for the next, so
 * that the hot path is taken both ways; and every RPC must still end with its
 * own reply.  An abort that lands on the thread while it is parked sends the
 * server's answer the slow way, so arm ten's crossing is asked here too, by
 * chance and many times.
 */
#define ARM11_RPCS		2000

/*
 * A bound and not a target: the firing stops when the thread is done.  It
 * was a target of 1000 with a yield after each abort, and under KVM the 2000
 * RPCs were over after as few as 2 of them -- the yield gave the processor to
 * the thread and its server for a whole quantum each time, and with the fix
 * taken out the arm passed 6 KVM boots of 6, while TCG caught 6 of 6.
 */
#define ARM11_ABORTS		1000000

/* Looks at the count of RPCs begun before a thread that begins none is let be. */
#define ARM11_IDLE		100000000UL
#define ARM11_REQUEST_ID	1100
#define ARM11_REPLY_ID		1101
#define ARM11_STOP_ID		1102

struct arm11_msg {
	mach_msg_header_t	h;
	natural_t		seq;
};

/* Room for the trailer the kernel appends, as struct roomy_msg explains. */
struct arm11_buf {
	struct arm11_msg	m;
	char			slack[64];
};

/*
 * A thread that keeps asking and the server that answers it, as arms eleven
 * and twelve use them: the server answers each request in the same call that
 * waits for the next, so that the hot path is taken both ways, and the thread
 * makes `rpcs' RPCs, each numbered, once `go' is set.
 */
struct rpc_pair {
	const char		*arm;		/* "[11]", in what is said */
	natural_t		rpcs;
	int			request_id;
	int			reply_id;
	int			stop_id;
	mach_port_t		port;		/* the server receives here */
	mach_port_t		reply_port;	/* and the thread here */
	volatile mach_port_t	thread;
	volatile int		go;
	volatile natural_t	calls;		/* RPCs begun */
	volatile int		done;
	volatile natural_t	answered;	/* each with its own reply */
	volatile kern_return_t	bad_kr;
	volatile int		bad_id;
	volatile natural_t	bad_seq;
	volatile kern_return_t	server_kr;
	volatile int		server_id;
};

static void *
the_server_that_answers_every_time(void *arg)
{
	struct rpc_pair		*p = arg;
	struct arm11_buf	b;
	mach_msg_option_t	option = MACH_RCV_MSG;
	mach_msg_size_t		send_size = 0;
	kern_return_t		kr;

	for (;;) {
		kr = mach_msg(&b.m.h, option, send_size, sizeof b,
			      p->port, MACH_MSG_TIMEOUT_NONE,
			      MACH_PORT_NULL);
		if (kr != MACH_MSG_SUCCESS ||
		    b.m.h.msgh_id != p->request_id)
			break;

		/* The answer goes in the call that waits for the next request. */
		b.m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE,
						 0);
		b.m.h.msgh_size = sizeof b.m;
		b.m.h.msgh_local_port = MACH_PORT_NULL;
		b.m.h.msgh_reserved = 0;
		b.m.h.msgh_id = p->reply_id;
		option = MACH_SEND_MSG | MACH_RCV_MSG;
		send_size = sizeof b.m;
	}
	p->server_kr = kr;
	p->server_id = b.m.h.msgh_id;
	return NULL;
}

static void *
the_thread_that_keeps_asking(void *arg)
{
	struct rpc_pair		*p = arg;
	struct arm11_buf	b;
	kern_return_t		kr;
	natural_t		i;

	p->thread = mach_thread_self();

	/*
	 * ⚠️ Not a single RPC before the arm starts: on a fast machine the
	 * whole run could otherwise be over before the arm's first abort or
	 * look, and an arm that did nothing would have asked nothing.
	 */
	while (!p->go)
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0);

	/*
	 * No MACH_RCV_INTERRUPT: libmach re-issues an interrupted receive, so
	 * an RPC an abort interrupts still ends with its reply, and the
	 * numbers say whether it was the right one.
	 */
	for (i = 0; i < p->rpcs; i++) {
		arm_ten_request(&b.m.h, p->port, p->reply_port,
				p->request_id);
		b.m.h.msgh_size = sizeof b.m;
		b.m.seq = i;
		p->calls = i + 1;	/* where arm eleven takes its aim */
		kr = mach_msg(&b.m.h, MACH_SEND_MSG | MACH_RCV_MSG,
			      sizeof b.m, sizeof b, p->reply_port,
			      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		if (kr != MACH_MSG_SUCCESS ||
		    b.m.h.msgh_id != p->reply_id || b.m.seq != i) {
			p->bad_kr = kr;
			p->bad_id = b.m.h.msgh_id;
			p->bad_seq = b.m.seq;
			break;
		}
	}
	p->answered = i;
	p->done = 1;
	return NULL;
}

/*
 * The two ports, the server in its receive, and the thread, which has said
 * who it is and waits for `go'.  What failed is said.
 */
static int
rpc_pair_start(struct rpc_pair *p, pthread_t *server, pthread_t *asker)
{
	kern_return_t	kr;
	int		i;

	kr = arm_ten_port(&p->port, 1);
	if (kr == KERN_SUCCESS)
		kr = arm_ten_port(&p->reply_port, 0);
	if (kr != KERN_SUCCESS) {
		printf("act_test: %s the ports for the RPCs could not be made "
		       "(%d) — WRONG\n", p->arm, kr);
		return 0;
	}

	if (pthread_create(server, NULL, the_server_that_answers_every_time,
			   p) != 0) {
		printf("act_test: %s pthread_create failed for the server "
		       "— WRONG\n", p->arm);
		return 0;
	}
	nap(100);	/* the server in its receive, as in arm ten */

	if (pthread_create(asker, NULL, the_thread_that_keeps_asking,
			   p) != 0) {
		printf("act_test: %s pthread_create failed for the thread "
		       "— WRONG\n", p->arm);
		return 0;
	}
	for (i = 0; i < PATIENCE && p->thread == MACH_PORT_NULL; i++)
		nap(10);
	if (p->thread == MACH_PORT_NULL) {
		printf("act_test: %s the thread never published its port "
		       "— WRONG\n", p->arm);
		return 0;
	}
	return 1;
}

/*
 * Waits for the thread, stops the server, and says whether every RPC ended
 * with its own reply.  `n' and `what' say what the arm did to the thread
 * meanwhile: "after 12 aborts".
 */
static int
rpc_pair_finish(struct rpc_pair *p, pthread_t server, pthread_t asker,
		int n, const char *what)
{
	mach_msg_header_t	stop;
	int			i;

	for (i = 0; i < 2 * PATIENCE && !p->done; i++)
		nap(100);

	stop.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	stop.msgh_size = sizeof stop;
	stop.msgh_remote_port = p->port;
	stop.msgh_local_port = MACH_PORT_NULL;
	stop.msgh_reserved = 0;
	stop.msgh_id = p->stop_id;
	(void) mach_msg(&stop, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof stop,
			0, MACH_PORT_NULL, 1000, MACH_PORT_NULL);

	if (!p->done) {
		printf("act_test: %s the thread asking never finished: %u of "
		       "%u RPCs after %d %s — WRONG\n", p->arm,
		       (unsigned) p->answered, (unsigned) p->rpcs, n, what);
		return 0;
	}
	(void) pthread_join(asker, NULL);
	(void) pthread_join(server, NULL);

	if (p->answered != p->rpcs) {
		printf("act_test: %s RPC %u of %u answered 0x%x with id %d and "
		       "number %u, after %d %s — WRONG\n", p->arm,
		       (unsigned) p->answered, (unsigned) p->rpcs,
		       (int) p->bad_kr, p->bad_id, (unsigned) p->bad_seq, n,
		       what);
		return 0;
	}
	if (p->server_kr != MACH_MSG_SUCCESS || p->server_id != p->stop_id) {
		printf("act_test: %s the server stopped on 0x%x with id %d "
		       "instead of the stop message — WRONG\n", p->arm,
		       (int) p->server_kr, p->server_id);
		return 0;
	}
	return 1;
}

static struct rpc_pair	arm_eleven = {
	.arm = "[11]",
	.rpcs = ARM11_RPCS,
	.request_id = ARM11_REQUEST_ID,
	.reply_id = ARM11_REPLY_ID,
	.stop_id = ARM11_STOP_ID,
	.bad_kr = MACH_MSG_SUCCESS,
	.server_kr = -1,
};

static int
arm_eleven_abort_a_thread_that_keeps_asking(void)
{
	pthread_t		server, victim;
	kern_return_t		refused = KERN_SUCCESS;
	int			aborts = 0, alone;
	natural_t		seen;
	unsigned long		idle;

	if (!rpc_pair_start(&arm_eleven, &server, &victim))
		return 0;

	/*
	 * 🔴 AIMED, at more than one processor: one abort as the thread enters
	 * each RPC, which is when it is between its trap and the park.  Fired
	 * at random they were too few to land there under KVM: as few as 4 in
	 * a whole run, with this processor never yielded, and the ablated arm
	 * passed one KVM boot of three.  At one processor a yield follows
	 * each, so that the thread gets to run into them.  The first is fired
	 * before the thread can have made an RPC, so a run with no abort in it
	 * is a refusal, and the firing stops at any refusal, or when the
	 * thread has begun no RPC for ARM11_IDLE looks: until then it is alive.
	 */
	alone = processors() <= 1;
	arm_eleven.go = 1;
	seen = 0;
	idle = 0;
	while (!arm_eleven.done && aborts < ARM11_ABORTS) {
		if (aborts > 0 && !alone) {
			if (arm_eleven.calls == seen) {
				if (++idle > ARM11_IDLE)
					break;
				continue;
			}
			seen = arm_eleven.calls;
			idle = 0;
		}
		refused = thread_abort(arm_eleven.thread);
		if (refused != KERN_SUCCESS)
			break;
		aborts++;
		if (alone)
			(void) thread_switch(MACH_PORT_NULL,
					     SWITCH_OPTION_NONE, 0);
	}

	if (!rpc_pair_finish(&arm_eleven, server, victim, aborts, "aborts"))
		return 0;
	if (aborts == 0) {
		printf("act_test: [11] thread_abort refused the first abort "
		       "(%d) on a thread alive and about to ask — WRONG\n",
		       refused);
		return 0;
	}

	(void) mach_port_destroy(mach_task_self(), arm_eleven.port);
	(void) mach_port_destroy(mach_task_self(), arm_eleven.reply_port);

	printf("act_test: [11] %d RPCs taking the hot path both ways, with %d "
	       "aborts fired at the thread making them: each ended with its "
	       "own reply (#687)\n", ARM11_RPCS, aborts);
	return 1;
}

/*
 * ── Arm twelve: look at the reply port of a thread that keeps asking (#687) ─
 *
 * The hot path parks the thread on its reply port's queue and marks it
 * TH_WAIT, and it then gave back the reference the trap had taken on that
 * port -- under the port's own lock, which is a mutex.  When another thread
 * held that lock, the parked thread slept on it, inside its own park, and the
 * mutex's wakeup ended the wait the thread had declared for its reply: back
 * in the trap with nothing delivered, "mmot_hotpath: bad wait result 0
 * (ith_state 10004001)".
 *
 * A port's status is read under its lock and then its queue's, and the queue
 * is the parked thread's until it has parked: a status call that comes while
 * the RPC is parking waits for the queue holding the port, and holds it still
 * when the parked thread comes to give its reference back.  So this arm asks
 * for the status of the reply port of a thread making ARM12_RPCS RPCs, for as
 * long as they last, and every RPC must end with its own reply.
 *
 * ⚠️ The window is a few instructions wide, so a boot rarely meets it: what
 * makes this a test is ablations/687-widen-port-held.patch, which holds the
 * port longer whenever a thread is parked on it and not yet asleep, and says
 * so.  At one processor the status call cannot run while the RPC parks, and
 * a yield follows each, so that the RPCs get to run at all.
 */
#define ARM12_RPCS		2000
#define ARM12_REQUEST_ID	1200
#define ARM12_REPLY_ID		1201
#define ARM12_STOP_ID		1202

static struct rpc_pair	arm_twelve = {
	.arm = "[12]",
	.rpcs = ARM12_RPCS,
	.request_id = ARM12_REQUEST_ID,
	.reply_id = ARM12_REPLY_ID,
	.stop_id = ARM12_STOP_ID,
	.bad_kr = MACH_MSG_SUCCESS,
	.server_kr = -1,
};

static int
arm_twelve_look_at_the_reply_port(void)
{
	pthread_t		server, asker;
	mach_port_status_t	status;
	mach_msg_type_number_t	count;
	kern_return_t		kr = KERN_SUCCESS;
	int			looks = 0, alone;

	if (!rpc_pair_start(&arm_twelve, &server, &asker))
		return 0;

	alone = processors() <= 1;
	arm_twelve.go = 1;
	while (!arm_twelve.done) {
		count = MACH_PORT_RECEIVE_STATUS_COUNT;
		kr = mach_port_get_attributes(mach_task_self(),
					      arm_twelve.reply_port,
					      MACH_PORT_RECEIVE_STATUS,
					      (mach_port_info_t) &status,
					      &count);
		if (kr != KERN_SUCCESS)
			break;
		looks++;
		if (alone)
			(void) thread_switch(MACH_PORT_NULL,
					     SWITCH_OPTION_NONE, 0);
	}

	if (!rpc_pair_finish(&arm_twelve, server, asker, looks,
			     "looks at the reply port"))
		return 0;
	if (kr != KERN_SUCCESS) {
		printf("act_test: [12] the reply port's status was refused (%d) "
		       "after %d answers — WRONG\n", kr, looks);
		return 0;
	}
	if (looks == 0) {
		printf("act_test: [12] the RPCs were over before one look at "
		       "their reply port, which asks nothing — WRONG\n");
		return 0;
	}

	(void) mach_port_destroy(mach_task_self(), arm_twelve.port);
	(void) mach_port_destroy(mach_task_self(), arm_twelve.reply_port);

	printf("act_test: [12] %d RPCs shaped for the hot path both ways, with "
	       "their reply port's status read %d times while they were made: "
	       "each ended with its own reply (#687)\n", ARM12_RPCS, looks);
	return 1;
}

/* ----------------------------------------------------------------
 * Arm thirteen: a message to a receiver thread_get_state() holds (#709)
 * ---------------------------------------------------------------- */

/*
 * thread_get_state() stops its thread with thread_stop_wait() and reads it as
 * stopped until thread_unstop().  A receiver asleep in mach_msg counts as
 * stopped, having blocked interruptibly, and a message delivered to it then
 * must leave it off the run queues: wake_dequeued() does, and thread_unstop()
 * restarts it.  thread_go(), which every delivery uses, put it back on a run
 * queue whatever TH_SUSP said, and it ran -- its copyout among other things --
 * while its state was being read.  It cannot reach its own code meanwhile:
 * thread_get_state() also holds it, and the hold stops it on the way to ring 3.
 * So what this arm looks at is the copyout, in the receiver's buffer, which
 * any thread of the task can read.
 *
 * The receiver asks for ARM13_SIZE, a size nothing else asks for, and
 * ablations/709-widen-get-state.patch holds thread_get_state() 100 ms between
 * stopping such a receiver and letting it go.  A third thread sends it a
 * message 20 ms into that and looks at its buffer 30 ms later: a message there
 * is a receiver that ran while it was stopped.  Without the widen
 * thread_get_state() is over before the look, and the arm says it did not ask.
 */
#define	ARM13_SIZE		0x7090
#define	ARM13_ID		709
#define	ARM13_NOT_ASKED		(-1)

static mach_port_t		arm13_port;
static mach_port_t		arm13_thread = MACH_PORT_NULL;
static volatile int		arm13_parked;
static volatile int		arm13_calling;
static volatile int		arm13_returned;
static volatile int		arm13_returned_at_look = -1;
static volatile int		arm13_seen;
static volatile kern_return_t	arm13_sent = -1;
static volatile kern_return_t	arm13_kr = -1;
static union {
	mach_msg_header_t	h;
	char			bytes[ARM13_SIZE];
} arm13_buf;

static void *
arm13_receiver(void *arg)
{
	(void) arg;
	arm13_thread = mach_thread_self();
	arm13_buf.h.msgh_id = 0;
	arm13_parked = 1;
	arm13_kr = mach_msg(&arm13_buf.h, MACH_RCV_MSG, 0, ARM13_SIZE,
			    arm13_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	return NULL;
}

static void *
arm13_sender(void *arg)
{
	mach_msg_header_t	msg;
	int			i;

	(void) arg;
	for (i = 0; i < PATIENCE * 10 && !arm13_calling; i++)
		nap(1);
	nap(20);
	msg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	msg.msgh_size = sizeof msg;
	msg.msgh_remote_port = arm13_port;
	msg.msgh_local_port = MACH_PORT_NULL;
	msg.msgh_id = ARM13_ID;
	arm13_sent = mach_msg(&msg, MACH_SEND_MSG, sizeof msg, 0,
			      MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
			      MACH_PORT_NULL);
	nap(30);
	arm13_returned_at_look = arm13_returned;
	arm13_seen = ((volatile mach_msg_header_t *) &arm13_buf.h)->msgh_id
		     == ARM13_ID;
	return NULL;
}

static int
arm_thirteen_message_to_a_receiver_being_read(void)
{
	pthread_t			receiver, sender;
	struct x86_64_thread_state	state;
	mach_msg_type_number_t		count = x86_64_THREAD_STATE_COUNT;
	kern_return_t			kr;
	int				i;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&arm13_port);
	if (kr == KERN_SUCCESS)
		kr = mach_port_insert_right(mach_task_self(), arm13_port,
					    arm13_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [13] the port was not made (%d) — WRONG\n",
		       kr);
		return 0;
	}
	if (pthread_create(&receiver, NULL, arm13_receiver, NULL) != 0) {
		printf("act_test: [13] pthread_create failed — WRONG\n");
		return 0;
	}
	for (i = 0; i < PATIENCE && !arm13_parked; i++)
		nap(100);
	nap(300);			/* into the receive, like arm three */
	if (pthread_create(&sender, NULL, arm13_sender, NULL) != 0) {
		printf("act_test: [13] pthread_create failed — WRONG\n");
		return 0;
	}

	arm13_calling = 1;
	kr = thread_get_state(arm13_thread, x86_64_THREAD_STATE,
			      (thread_state_t) &state, &count);
	arm13_returned = 1;
	(void) pthread_join(sender, NULL);
	for (i = 0; i < PATIENCE && arm13_kr == -1; i++)
		nap(100);
	if (arm13_kr != -1)
		(void) pthread_join(receiver, NULL);
	(void) mach_port_destroy(mach_task_self(), arm13_port);

	if (kr != KERN_SUCCESS) {
		printf("act_test: [13] thread_get_state answered %d — WRONG\n",
		       kr);
		return 0;
	}
	if (arm13_sent != MACH_MSG_SUCCESS) {
		printf("act_test: [13] the send answered 0x%x — WRONG\n",
		       (int) arm13_sent);
		return 0;
	}
	if (arm13_kr != MACH_MSG_SUCCESS) {
		printf("act_test: [13] the receiver's mach_msg answered 0x%x "
		       "— WRONG\n", (int) arm13_kr);
		return 0;
	}
	if (arm13_returned_at_look) {
		printf("act_test: [13] NOT ASKED — thread_get_state() had let "
		       "the receiver go before its buffer was looked at; "
		       "ablations/709-widen-get-state.patch holds it (#709)\n");
		return ARM13_NOT_ASKED;
	}
	if (arm13_seen) {
		printf("act_test: [13] the receiver took a message while "
		       "thread_get_state() held it stopped: a delivery put it "
		       "back on a run queue (#709) — WRONG\n");
		return 0;
	}
	printf("act_test: [13] a message delivered to a receiver "
	       "thread_get_state() held stopped waited in the kernel, and was "
	       "taken once thread_get_state() let it go (#709)\n");
	return 1;
}

/*
 * ── Arm fourteen: a task terminated while its threads are inside traps on its
 *    own space (#688) ──────────────────────────────────────────────────────
 *
 * task_terminate() takes a task's threads off it and then destroys its IPC
 * space.  thread_terminate() stops each thread wherever it is and lets it go
 * again to die on its own, so a thread stopped inside a trap goes on with it.
 * syscall_task_info() on its own task is where that happens: the task's lock
 * is held by task_terminate(), ref_task_port_locked() sleeps a tick in
 * mutex_pause() and port_name_to_task() looks the name up again, after the
 * space has died.  fast_send_right_lookup(), which it and four other traps
 * go through, asserted that the space was alive, and so did mach_msg()'s
 * three locked fast paths.  taskpanic_load met the first assert in 6 boots
 * of 8, when -t did not ask and its thirty seconds ran out with two threads
 * in syscall_task_info().
 *
 * Each round makes a child with this task's memory and starts three threads
 * in it: two call syscall_task_info() on their own task for ever, and one
 * asks the kernel the same through mach_msg(), as task_info()'s message.  The
 * threads use traps only.  Nothing of this task's libmach or libpthreads is
 * set up in the child, and mig_get_reply_port() would look there for a thread
 * that does not exist.  After ARM14_RUN_MS the child is terminated from here.
 * The kernel must come through every round, and the child's port must be a
 * dead name after each one.
 *
 * ⚠️ From here, nothing shows whether a thread was inside the window when its
 * space died, since the thread dies on its way out.  The ablations put the
 * asserts back, and this arm is what stops the kernel on them:
 * 688-lookup-asserts-live as it is, and 688-copyin-asserts-live over
 * 688-widen-copyin-late, which puts the third thread to sleep where the
 * first two sleep by themselves.  mach_msg() does not sleep before its
 * lookup, so without the widen a thread is stopped there only by a
 * preemption that lands in those few instructions.
 */
#define	ARM14_ROUNDS		50
#define	ARM14_STACK		(16 * 1024)
#define	ARM14_RUN_MS		5
#define	ARM14_TASK_INFO_ID	2098	/* task_info()'s msgh_id in mach.defs,
					   as the generated mach_user.c sends it */

static void
arm_fourteen_trap_loop(void)
{
	mach_port_t		self = (mach_task_self)();	/* the trap */
	struct task_basic_info	info;
	mach_msg_type_number_t	n;

	for (;;) {
		n = TASK_BASIC_INFO_COUNT;
		(void) syscall_task_info(self, TASK_BASIC_INFO,
					 (task_info_t) &info, &n);
	}
}

union arm14_msg {
	struct {
		mach_msg_header_t	head;
		NDR_record_t		ndr;
		task_flavor_t		flavor;
		mach_msg_type_number_t	count;
	}	in;
	struct {
		mach_msg_header_t	head;
		NDR_record_t		ndr;
		kern_return_t		ret;
	}	out;
	char	room[256];
};

/* task_info()'s message to `task', its reply received on `reply'. */
static mach_msg_return_t
arm_fourteen_ask(mach_port_t task, mach_port_t reply, union arm14_msg *m)
{
	m->in.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
					      MACH_MSG_TYPE_MAKE_SEND_ONCE);
	m->in.head.msgh_size = sizeof(m->in);
	m->in.head.msgh_remote_port = task;
	m->in.head.msgh_local_port = reply;
	m->in.head.msgh_id = ARM14_TASK_INFO_ID;
	m->in.ndr = NDR_record;
	m->in.flavor = TASK_BASIC_INFO;
	m->in.count = TASK_BASIC_INFO_COUNT;
	return mach_msg_overwrite_trap(&m->in.head,
				       MACH_SEND_MSG | MACH_RCV_MSG,
				       sizeof(m->in), sizeof(*m), reply,
				       MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL,
				       (mach_msg_header_t *) 0, 0);
}

static void
arm_fourteen_rpc_loop(void)
{
	mach_port_t	self = (mach_task_self)();	/* the trap */
	mach_port_t	reply = mach_reply_port();
	union arm14_msg	m;

	for (;;)
		(void) arm_fourteen_ask(self, reply, &m);
}

/* A thread of the child, started at `fn' on a stack of its own. */
static kern_return_t
arm_fourteen_start(mach_port_t child, void (*fn)(void))
{
	struct x86_64_thread_state	state;
	mach_msg_type_number_t		count = x86_64_THREAD_STATE_COUNT;
	vm_address_t			stack = 0;
	mach_port_t			th = MACH_PORT_NULL;
	kern_return_t			kr;

	kr = vm_allocate(child, &stack, ARM14_STACK, TRUE);
	if (kr == KERN_SUCCESS)
		kr = thread_create(child, &th);
	if (kr == KERN_SUCCESS)
		kr = thread_get_state(th, x86_64_THREAD_STATE,
				      (thread_state_t) &state, &count);
	if (kr == KERN_SUCCESS) {
		/* As a call leaves it: the return address's eight bytes below
		 * a 16-byte boundary. */
		state.rip = (uint64_t) (uintptr_t) fn;
		state.rsp = (uint64_t) (stack + ARM14_STACK - 8);
		kr = thread_set_state(th, x86_64_THREAD_STATE,
				      (thread_state_t) &state, count);
	}
	if (kr == KERN_SUCCESS)
		kr = thread_resume(th);
	if (th != MACH_PORT_NULL)
		(void) mach_port_deallocate(mach_task_self(), th);
	return kr;
}

static int
arm_fourteen_terminate_inside_traps(void)
{
	mach_port_t		me = mach_task_self();
	mach_port_t		child;
	mach_port_type_t	type;
	kern_return_t		kr;
	int			round;

	/*
	 * The third thread's question, asked here first: a message the kernel
	 * did not answer would leave that thread looping on a refusal, and
	 * the arm would pass without having asked anything through mach_msg().
	 */
	{
		mach_port_t		reply = mach_reply_port();
		union arm14_msg		m;
		mach_msg_return_t	mr = arm_fourteen_ask(me, reply, &m);

		(void) mach_port_mod_refs(me, reply, MACH_PORT_RIGHT_RECEIVE,
					  -1);
		if (mr != MACH_MSG_SUCCESS || m.out.ret != KERN_SUCCESS) {
			printf("act_test: [14] task_info()'s message, sent from "
			       "here to this task, came back 0x%x with %d: the "
			       "third thread's question is not one the kernel "
			       "answers — WRONG\n", mr,
			       mr == MACH_MSG_SUCCESS ? m.out.ret : 0);
			return 0;
		}
	}

	for (round = 0; round < ARM14_ROUNDS; round++) {
		child = MACH_PORT_NULL;
		kr = task_create(me, NULL, 0, TRUE, &child);
		if (kr == KERN_SUCCESS)
			kr = arm_fourteen_start(child, arm_fourteen_trap_loop);
		if (kr == KERN_SUCCESS)
			kr = arm_fourteen_start(child, arm_fourteen_trap_loop);
		if (kr == KERN_SUCCESS)
			kr = arm_fourteen_start(child, arm_fourteen_rpc_loop);
		if (kr != KERN_SUCCESS) {
			printf("act_test: [14] round %d: a child with three "
			       "threads could not be started (%d) — WRONG\n",
			       round, kr);
			if (child != MACH_PORT_NULL) {
				(void) task_terminate(child);
				(void) mach_port_deallocate(me, child);
			}
			return 0;
		}

		nap(ARM14_RUN_MS);

		kr = task_terminate(child);
		if (kr == KERN_SUCCESS)
			kr = mach_port_type(me, child, &type);
		(void) mach_port_deallocate(me, child);
		if (kr != KERN_SUCCESS) {
			printf("act_test: [14] round %d: terminating the child, or "
			       "asking about its port after, answered %d — "
			       "WRONG\n", round, kr);
			return 0;
		}
		if (!(type & MACH_PORT_TYPE_DEAD_NAME)) {
			printf("act_test: [14] round %d: the terminated child's "
			       "port is not a dead name (type 0x%x) — WRONG\n",
			       round, type);
			return 0;
		}
	}

	printf("act_test: [14] %d children terminated, each with two threads "
	       "in syscall_task_info() and one in task_info()'s message on its "
	       "own task, and the kernel came through: every child's port "
	       "was a dead name after (#688)\n", ARM14_ROUNDS);
	return 1;
}

/*
 * Arm twenty: thread_activation_create() makes nothing (#783).
 *
 * It made an empty activation -- no thread behind it -- in a port's thread
 * pool, the waiting room of OSF's migrating RPC, whose traps 35 and 36 are
 * not_implemented: nothing could ever come to one, and any task could make
 * one.  thread_policy() read through the missing thread to address 0, and a
 * zero user_stack was asked of by an assert.  The kernel refuses now, with
 * KERN_NOT_SUPPORTED, before it looks at what it is given, and both calls
 * here must be told that: the first offers what OSF wanted, a receive right
 * and a stack, the second a zero stack.  If the first makes one all the same
 * -- the ablation that puts the old body back -- thread_policy() is asked of
 * it, and must refuse an activation with no thread rather than read it.
 * Returns 1 when refused, 0 when not, -1 when it could not be asked.
 */
static int
arm_twenty_no_empty_activation(void)
{
	static char		stack[4096];	/* offered, never run on */
	mach_port_t		port, act = MACH_PORT_NULL;
	policy_timeshare_base_data_t ts;
	kern_return_t		kr, kr0, krp;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&port);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [20] NOT ASKED — no receive right to offer: "
		       "0x%x\n", kr);
		return -1;
	}

	kr = thread_activation_create(mach_task_self(), port,
				      (vm_offset_t) (stack + sizeof stack),
				      sizeof stack, &act);
	if (kr == KERN_SUCCESS) {
		ts.base_priority = 12;
		krp = thread_policy(act, POLICY_TIMESHARE, (integer_t *) &ts,
				    POLICY_TIMESHARE_BASE_COUNT, FALSE);
		printf("act_test: [20] thread_activation_create() made an "
		       "activation with no thread, and thread_policy() on it "
		       "answered 0x%x — WRONG, it makes none (#783)\n", krp);
		(void) mach_port_deallocate(mach_task_self(), act);
	}
	kr0 = thread_activation_create(mach_task_self(), port, 0, 0, &act);
	(void) mach_port_mod_refs(mach_task_self(), port,
				  MACH_PORT_RIGHT_RECEIVE, -1);

	if (kr != KERN_NOT_SUPPORTED || kr0 != KERN_NOT_SUPPORTED) {
		printf("act_test: [20] thread_activation_create() answered 0x%x "
		       "with a stack and 0x%x without one, where it refuses with "
		       "KERN_NOT_SUPPORTED, 0x%x — WRONG (#783)\n",
		       kr, kr0, KERN_NOT_SUPPORTED);
		return 0;
	}
	printf("act_test: [20] thread_activation_create() refused with a stack "
	       "and without one, KERN_NOT_SUPPORTED both times: no activation "
	       "with no thread behind it can be made (#783)\n");
	return 1;
}

/*
 * Arm twenty-one: set_limit lowers a limit and never raises it (#786).
 *
 * thread_policy() with set_limit made the base it was given the thread's
 * limit, comparing it with nothing, so a task put a thread of its own at
 * priority 0, above every kernel thread; task_policy() did the same for a
 * task.  A thread made here and never started asks for priority 0 with
 * set_limit, and must be refused with KERN_POLICY_LIMIT, its policy, limit
 * and base read back as they were; a task made here asks the same and must be
 * refused too -- a child, so that a kernel without the fix changes nobody
 * act_test needs.  And a lower priority with set_limit stays granted: an
 * arm that passed by refusing everything would prove nothing.
 * Timesharing, the one policy every task may ask for: FIFO, which this arm
 * asked first, is refused before any limit is looked at since FIFO and RR
 * became the processor set's control port's to give (#786, arm 25).
 * Returns 1 when all three answers are right, 0 when not, -1 when it could
 * not be asked.
 */
static int
arm_twenty_one_set_limit_never_raises(void)
{
	thread_act_t			t;
	mach_port_t			child;
	kern_return_t			kr, krf, krt, krl;
	policy_timeshare_base_data_t	zero, ts;
	struct policy_timeshare_info	before, after;
	struct thread_basic_info	basic;
	mach_msg_type_number_t		n;

	if (thread_create(mach_task_self(), &t) != KERN_SUCCESS) {
		printf("act_test: [21] NOT ASKED — no thread to ask with\n");
		return -1;
	}
	n = POLICY_TIMESHARE_INFO_COUNT;
	if (thread_info(t, THREAD_SCHED_TIMESHARE_INFO, (thread_info_t) &before,
			&n) != KERN_SUCCESS) {
		printf("act_test: [21] NOT ASKED — the new thread is not "
		       "timesharing\n");
		(void) thread_terminate(t);
		(void) mach_port_deallocate(mach_task_self(), t);
		return -1;
	}

	zero.base_priority = 0;
	krf = thread_policy(t, POLICY_TIMESHARE, (integer_t *) &zero,
			    POLICY_TIMESHARE_BASE_COUNT, TRUE);
	n = THREAD_BASIC_INFO_COUNT;
	basic.policy = -1;
	(void) thread_info(t, THREAD_BASIC_INFO, (thread_info_t) &basic, &n);
	n = POLICY_TIMESHARE_INFO_COUNT;
	after.max_priority = after.base_priority = -1;
	kr = thread_info(t, THREAD_SCHED_TIMESHARE_INFO, (thread_info_t) &after,
			 &n);

	ts.base_priority = before.max_priority + 4;
	krl = thread_policy(t, POLICY_TIMESHARE, (integer_t *) &ts,
			    POLICY_TIMESHARE_BASE_COUNT, TRUE);
	(void) thread_terminate(t);
	(void) mach_port_deallocate(mach_task_self(), t);

	krt = KERN_FAILURE;
	if (task_create(mach_task_self(), (ledger_port_array_t) 0, 0, FALSE,
			&child) == KERN_SUCCESS) {
		krt = task_policy(child, POLICY_TIMESHARE, (integer_t *) &zero,
				  POLICY_TIMESHARE_BASE_COUNT, TRUE, FALSE);
		(void) task_terminate(child);
		(void) mach_port_deallocate(mach_task_self(), child);
	}

	if (krf != KERN_POLICY_LIMIT || krt != KERN_POLICY_LIMIT ||
	    krl != KERN_SUCCESS || kr != KERN_SUCCESS ||
	    basic.policy != POLICY_TIMESHARE ||
	    after.max_priority != before.max_priority ||
	    after.base_priority != before.base_priority) {
		printf("act_test: [21] asked for priority 0 with "
		       "set_limit: the thread answered 0x%x (policy %d, limit %d, "
		       "base %d, where they were %d and %d), a task 0x%x, and a "
		       "lower priority 0x%x — WRONG, a limit is lowered and never "
		       "raised (#786)\n", krf, (int) basic.policy,
		       after.max_priority, after.base_priority,
		       before.max_priority, before.base_priority, krt, krl);
		return 0;
	}
	printf("act_test: [21] a thread and a task asked for priority 0 "
	       "with set_limit: both refused with KERN_POLICY_LIMIT, the "
	       "thread's limit and base still %d and %d; a lower one, %d, "
	       "still granted (#786)\n", before.max_priority,
	       before.base_priority, ts.base_priority);
	return 1;
}

/*
 * Arm twenty-three (#786): the floating-point state thread_set_state() writes
 * is either refused or is what the thread finds.  Three answers:
 *  - a new thread's state reads back as the initial one: x87 control 037FH,
 *    every x87 register empty -- the ABRIDGED tag byte, 0 -- and MXCSR 1F80H;
 *  - an image whose MXCSR sets the lowest bit this processor's mask does not
 *    allow is refused, and the state read again is the one before;
 *  - XMM0 and MXCSR written into a thread that has never used SSE are the
 *    ones it finds when it starts.  With the header still saying SSE was
 *    initial, the restore gave it zeros.
 * The mask comes from an FXSAVE of this program's own: a thread that has
 * never been switched out has none in its image yet.
 * ⚠️ The refused image is never run.  On a kernel that accepted it, the next
 * one overwrites it before the thread starts, so the arm says WRONG and the
 * machine runs on.
 * Returns 1 when all three answers are right, 0 when not, -1 when it could
 * not be asked.
 */
#define	ARM23_WAIT_MS	2000
#define	ARM23_XMM0_LO	0x0123456789abcdefULL
#define	ARM23_XMM0_HI	0xfedcba9876543210ULL
#define	ARM23_MXCSR	0x3f80U		/* every exception masked, round down */

static volatile uint64_t	arm23_xmm0[2];
static volatile uint32_t	arm23_mxcsr;
static volatile int		arm23_looked;

/* What the started thread finds, read before anything else can touch it. */
static void
arm_twenty_three_look(void)
{
	uint64_t	x[2];
	uint32_t	m;

	__asm__ volatile("movdqu %%xmm0, %0\n\tstmxcsr %1"
			 : "=m" (x), "=m" (m));
	arm23_xmm0[0] = x[0];
	arm23_xmm0[1] = x[1];
	arm23_mxcsr = m;
	arm23_looked = 1;
	for (;;)
		;
}

static int
arm_twenty_three_float_state_is_what_the_thread_finds(void)
{
	static uint8_t			fx[512] __attribute__((aligned(16)));
	struct x86_64_float_state	first, again, bad, good;
	struct x86_64_thread_state	state;
	mach_msg_type_number_t		n;
	thread_act_t			t;
	vm_address_t			stack = 0;
	kern_return_t			kr_bad, kr_good, kr_regs, kr_go;
	uint32_t			mask, bit;
	unsigned			i;
	int				waited = 0, init_ok, unchanged = 1;

	__asm__ volatile("fxsave %0" : "=m" (fx));
	mask = *(uint32_t *) (fx + 28);
	if (mask == 0)
		mask = 0x0000ffbfU;	/* SDM Vol. 1 §11.6.6: what zero means */
	bit = ~mask & (0U - ~mask);

	if (vm_allocate(mach_task_self(), &stack, vm_page_size, TRUE) !=
	    KERN_SUCCESS) {
		printf("act_test: [23] NOT ASKED — no page for a stack\n");
		return -1;
	}
	if (thread_create(mach_task_self(), &t) != KERN_SUCCESS) {
		printf("act_test: [23] NOT ASKED — no thread to ask with\n");
		(void) vm_deallocate(mach_task_self(), stack, vm_page_size);
		return -1;
	}
	n = x86_64_FLOAT_STATE_COUNT;
	if (thread_get_state(t, x86_64_FLOAT_STATE, (thread_state_t) &first,
			     &n) != KERN_SUCCESS) {
		printf("act_test: [23] NOT ASKED — no floating-point state to "
		       "read\n");
		(void) thread_terminate(t);
		(void) mach_port_deallocate(mach_task_self(), t);
		(void) vm_deallocate(mach_task_self(), stack, vm_page_size);
		return -1;
	}
	init_ok = *(uint16_t *) (first.fx_image + 0) == 0x037f &&
		  first.fx_image[4] == 0 &&
		  *(uint32_t *) (first.fx_image + 24) == 0x1f80;

	bad = first;
	*(uint32_t *) (bad.fx_image + 24) |= bit;
	kr_bad = thread_set_state(t, x86_64_FLOAT_STATE, (thread_state_t) &bad,
				  x86_64_FLOAT_STATE_COUNT);
	n = x86_64_FLOAT_STATE_COUNT;
	if (thread_get_state(t, x86_64_FLOAT_STATE, (thread_state_t) &again,
			     &n) != KERN_SUCCESS)
		unchanged = 0;
	for (i = 0; unchanged && i < sizeof(first.fx_image); i++)
		if (again.fx_image[i] != first.fx_image[i])
			unchanged = 0;

	good = first;
	*(uint64_t *) (good.fx_image + 160) = ARM23_XMM0_LO;
	*(uint64_t *) (good.fx_image + 168) = ARM23_XMM0_HI;
	*(uint32_t *) (good.fx_image + 24) = ARM23_MXCSR;
	kr_good = thread_set_state(t, x86_64_FLOAT_STATE,
				   (thread_state_t) &good,
				   x86_64_FLOAT_STATE_COUNT);

	for (i = 0; i < sizeof state; i++)
		((volatile char *) &state)[i] = 0;
	state.rip = (uint64_t) (uintptr_t) arm_twenty_three_look;
	state.rsp = stack + vm_page_size - 8;	/* as a call leaves it */
	state.rflags = 0x202;
	kr_regs = thread_set_state(t, x86_64_THREAD_STATE,
				   (thread_state_t) &state,
				   x86_64_THREAD_STATE_COUNT);
	kr_go = KERN_FAILURE;
	if (kr_good == KERN_SUCCESS && kr_regs == KERN_SUCCESS)
		kr_go = thread_resume(t);
	while (kr_go == KERN_SUCCESS && !arm23_looked &&
	       waited < ARM23_WAIT_MS) {
		if (nap(10) != MACH_RCV_TIMED_OUT)
			break;
		waited += 10;
	}
	(void) thread_terminate(t);
	(void) mach_port_deallocate(mach_task_self(), t);
	(void) vm_deallocate(mach_task_self(), stack, vm_page_size);

	if (!init_ok || kr_bad == KERN_SUCCESS || !unchanged ||
	    kr_good != KERN_SUCCESS || kr_regs != KERN_SUCCESS ||
	    kr_go != KERN_SUCCESS || !arm23_looked ||
	    arm23_xmm0[0] != ARM23_XMM0_LO || arm23_xmm0[1] != ARM23_XMM0_HI ||
	    arm23_mxcsr != ARM23_MXCSR) {
		printf("act_test: [23] a new thread's state: x87 control 0x%x, "
		       "tag byte 0x%x, MXCSR 0x%x; an MXCSR with 0x%x set, "
		       "outside the mask 0x%x, answered 0x%x and left the state "
		       "%s; XMM0 0x%016llx%016llx and MXCSR 0x%x written answered "
		       "0x%x, 0x%x, 0x%x, and the thread %s XMM0 "
		       "0x%016llx%016llx and MXCSR 0x%x — WRONG, written is "
		       "refused or found (#786)\n",
		       *(uint16_t *) (first.fx_image + 0), first.fx_image[4],
		       *(uint32_t *) (first.fx_image + 24), bit, mask, kr_bad,
		       unchanged ? "as it was" : "CHANGED",
		       (unsigned long long) ARM23_XMM0_HI,
		       (unsigned long long) ARM23_XMM0_LO, ARM23_MXCSR,
		       kr_good, kr_regs, kr_go,
		       arm23_looked ? "found" : "NEVER LOOKED, so not",
		       (unsigned long long) arm23_xmm0[1],
		       (unsigned long long) arm23_xmm0[0], arm23_mxcsr);
		return 0;
	}
	printf("act_test: [23] a new thread's state read back as the initial "
	       "one, x87 control 0x%x, tag byte 0x%x, MXCSR 0x%x; an MXCSR "
	       "with 0x%x set, outside the mask 0x%x, refused with 0x%x and "
	       "nothing changed; XMM0 0x%016llx%016llx and MXCSR 0x%x written, "
	       "and the thread found both (#786)\n",
	       *(uint16_t *) (first.fx_image + 0), first.fx_image[4],
	       *(uint32_t *) (first.fx_image + 24), bit, mask, kr_bad,
	       (unsigned long long) arm23_xmm0[1],
	       (unsigned long long) arm23_xmm0[0], arm23_mxcsr);
	return 1;
}

/*
 * Arm twenty-four (#786): a component the XSAVE header calls initial reads
 * as initial.  The thread puts ones in XMM0 and is stopped, so its area holds
 * them with SSE marked in use; then it runs VZEROALL and is stopped again.  A
 * processor that counts SSE initial after VZEROALL makes that second save
 * skip the XMM registers and clear the bit, and the ones stay in the area:
 * the read must give zero, which is what the restore would load.
 * Asked only where the processor says so, through XGETBV(1): TCG never
 * does, and there the second save writes the zeros itself.
 * Returns 1 when right, 0 when not, -1 when it could not be asked.
 */
#define	ARM24_WAIT_MS	2000

static volatile int		arm24_step;	/* 1 ones, 2 go, 3 zeroed */
static volatile uint64_t	arm24_xinuse;

static void
arm_twenty_four_zero_the_vectors(void)
{
	uint32_t	a, d;

	__asm__ volatile("pcmpeqd %%xmm0, %%xmm0" ::: "xmm0");
	arm24_step = 1;
	while (arm24_step != 2)
		;
	__asm__ volatile("vzeroall" ::: "xmm0", "xmm1", "xmm2", "xmm3",
			 "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9",
			 "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
	__asm__ volatile("xgetbv" : "=a" (a), "=d" (d) : "c" (1));
	arm24_xinuse = ((uint64_t) d << 32) | a;
	arm24_step = 3;
	for (;;)
		;
}

/* Wait for the thread to reach a step; 1 when it did. */
static int
arm_twenty_four_wait(int step)
{
	int	waited = 0;

	while (arm24_step != step && waited < ARM24_WAIT_MS) {
		if (nap(10) != MACH_RCV_TIMED_OUT)
			break;
		waited += 10;
	}
	return arm24_step == step;
}

/* Whether this processor has VZEROALL and XGETBV(1) for a ring-3 program. */
static int
arm_twenty_four_can_ask(void)
{
	uint32_t	a, b, c, d, lo, hi;

	__asm__ volatile("cpuid" : "=a" (a), "=b" (b), "=c" (c), "=d" (d)
			 : "a" (1), "c" (0));
	if (!(c & (1U << 27)) || !(c & (1U << 28)))	/* OSXSAVE, AVX */
		return 0;
	__asm__ volatile("xgetbv" : "=a" (lo), "=d" (hi) : "c" (0));
	if ((lo & 0x6) != 0x6)				/* XMM and YMM on */
		return 0;
	__asm__ volatile("cpuid" : "=a" (a), "=b" (b), "=c" (c), "=d" (d)
			 : "a" (0xd), "c" (1));
	return (a & (1U << 2)) != 0;			/* XGETBV with ECX=1 */
}

static int
arm_twenty_four_initial_reads_initial(void)
{
	struct x86_64_float_state	ones, zeroed;
	struct x86_64_thread_state	state;
	mach_msg_type_number_t		n;
	thread_act_t			t;
	vm_address_t			stack = 0;
	kern_return_t			kr1 = KERN_FAILURE, kr2 = KERN_FAILURE;
	unsigned			i, ones_seen = 0, nonzero = 0;
	int				passed = 0;

	if (!arm_twenty_four_can_ask()) {
		printf("act_test: [24] NOT ASKED — no VZEROALL or no XGETBV(1) "
		       "here, so nothing asks the processor to count SSE "
		       "initial (#786)\n");
		return -1;
	}
	if (vm_allocate(mach_task_self(), &stack, vm_page_size, TRUE) !=
	    KERN_SUCCESS || thread_create(mach_task_self(), &t) !=
	    KERN_SUCCESS) {
		printf("act_test: [24] NOT ASKED — no stack or no thread\n");
		if (stack != 0)
			(void) vm_deallocate(mach_task_self(), stack,
					     vm_page_size);
		return -1;
	}

	for (i = 0; i < sizeof state; i++)
		((volatile char *) &state)[i] = 0;
	state.rip = (uint64_t) (uintptr_t) arm_twenty_four_zero_the_vectors;
	state.rsp = stack + vm_page_size - 8;	/* as a call leaves it */
	state.rflags = 0x202;
	arm24_step = 0;
	if (thread_set_state(t, x86_64_THREAD_STATE, (thread_state_t) &state,
			     x86_64_THREAD_STATE_COUNT) == KERN_SUCCESS &&
	    thread_resume(t) == KERN_SUCCESS && arm_twenty_four_wait(1)) {
		n = x86_64_FLOAT_STATE_COUNT;
		kr1 = thread_get_state(t, x86_64_FLOAT_STATE,
				       (thread_state_t) &ones, &n);
		for (i = 160; kr1 == KERN_SUCCESS && i < 176; i++)
			ones_seen += ones.fx_image[i] == 0xff;
		arm24_step = 2;
		if (arm_twenty_four_wait(3) && (arm24_xinuse & 2) == 0) {
			n = x86_64_FLOAT_STATE_COUNT;
			kr2 = thread_get_state(t, x86_64_FLOAT_STATE,
					       (thread_state_t) &zeroed, &n);
			for (i = 160; kr2 == KERN_SUCCESS && i < 416; i++)
				nonzero += zeroed.fx_image[i] != 0;
		}
	}
	(void) thread_terminate(t);
	(void) mach_port_deallocate(mach_task_self(), t);
	(void) vm_deallocate(mach_task_self(), stack, vm_page_size);

	if (arm24_step == 3 && (arm24_xinuse & 2) != 0) {
		printf("act_test: [24] NOT ASKED — after VZEROALL this processor "
		       "still counts SSE in use (XINUSE 0x%llx), so its saves "
		       "write the XMM registers either way (#786)\n",
		       (unsigned long long) arm24_xinuse);
		return -1;
	}
	if (kr1 != KERN_SUCCESS || ones_seen != 16 || kr2 != KERN_SUCCESS ||
	    nonzero != 0) {
		printf("act_test: [24] the thread got to step %d; the first read "
		       "answered 0x%x with %u of XMM0's 16 bytes all ones, and "
		       "after VZEROALL, with SSE initial (XINUSE 0x%llx), the "
		       "second answered 0x%x with %u nonzero bytes in XMM0-XMM15 "
		       "— WRONG, a component the header calls initial reads as "
		       "initial (#786)\n", arm24_step, kr1, ones_seen,
		       (unsigned long long) arm24_xinuse, kr2, nonzero);
		return 0;
	}
	passed = 1;
	printf("act_test: [24] XMM0 read back all ones, then after VZEROALL, "
	       "with the processor counting SSE initial (XINUSE 0x%llx), every "
	       "XMM register read back zero, what the restore would load "
	       "(#786)\n", (unsigned long long) arm24_xinuse);
	return passed;
}

/*
 * Arm twenty-five (#786): FIFO and RR are the processor set's control port's
 * to give.  Without it they are refused -- thread_policy() for a thread,
 * task_policy() for a task -- and with it thread_set_policy() still gives
 * them, or an arm that passed by refusing everything would prove nothing.
 * And a thread that asked for FIFO and was refused, started spinning, does not
 * hold the processor: this one naps and wakes beside it, which at one
 * processor is the whole point.
 * Returns 1 when all of it holds, 0 when not, -1 when it could not be asked.
 */
#define	ARM25_NAPS	5

static void
arm_twenty_five_spin(void)
{
	for (;;)
		;
}

static int
arm_twenty_five_fifo_rr_need_the_set(void)
{
	mach_port_t			me = mach_task_self();
	mach_port_t			host = mach_host_self();
	mach_port_t			host_priv = MACH_PORT_NULL;
	mach_port_t			device = MACH_PORT_NULL, lw = MACH_PORT_NULL;
	mach_port_t			lp = MACH_PORT_NULL, sec = MACH_PORT_NULL;
	mach_port_t			name = MACH_PORT_NULL, pset = MACH_PORT_NULL;
	mach_port_t			child = MACH_PORT_NULL;
	thread_act_t			quiet = MACH_PORT_NULL;
	thread_act_t			spinner = MACH_PORT_NULL;
	vm_address_t			stack = 0;
	policy_fifo_base_data_t		fifo;
	policy_fifo_limit_data_t	fifo_limit;
	policy_rr_base_data_t		rr;
	struct policy_timeshare_info	ts;
	struct thread_basic_info	basic;
	struct x86_64_thread_state	state;
	host_basic_info_data_t		hi;
	mach_msg_type_number_t		n;
	kern_return_t			kr_tf = KERN_FAILURE, kr_tr = KERN_FAILURE;
	kern_return_t			kr_task = KERN_FAILURE, kr_set = KERN_FAILURE;
	kern_return_t			kr_spin = KERN_FAILURE;
	int				naps = 0, cpus = 0, passed = -1;
	unsigned			i;

	n = POLICY_TIMESHARE_INFO_COUNT;
	if (bootstrap_ports(bootstrap_port, &host_priv, &device, &lw, &lp,
			    &sec) != KERN_SUCCESS ||
	    processor_set_default(host, &name) != KERN_SUCCESS ||
	    host_processor_set_priv(host_priv, name, &pset) != KERN_SUCCESS ||
	    thread_create(me, &quiet) != KERN_SUCCESS ||
	    thread_create(me, &spinner) != KERN_SUCCESS ||
	    vm_allocate(me, &stack, vm_page_size, TRUE) != KERN_SUCCESS ||
	    thread_info(quiet, THREAD_SCHED_TIMESHARE_INFO,
			(thread_info_t) &ts, &n) != KERN_SUCCESS)
		printf("act_test: [25] NOT ASKED — no privileged host port, "
		       "processor set, threads or stack to ask with\n");
	else {
		/* At the threads' own base: only the policy is in question. */
		fifo.base_priority = ts.base_priority;
		fifo_limit.max_priority = ts.base_priority;
		rr.quantum = 0;
		rr.base_priority = ts.base_priority;

		kr_tf = thread_policy(quiet, POLICY_FIFO, (integer_t *) &fifo,
				      POLICY_FIFO_BASE_COUNT, FALSE);
		kr_tr = thread_policy(quiet, POLICY_RR, (integer_t *) &rr,
				      POLICY_RR_BASE_COUNT, FALSE);
		if (task_create(me, (ledger_port_array_t) 0, 0, FALSE,
				&child) == KERN_SUCCESS) {
			kr_task = task_policy(child, POLICY_FIFO,
					      (integer_t *) &fifo,
					      POLICY_FIFO_BASE_COUNT, FALSE,
					      FALSE);
			(void) task_terminate(child);
		}

		kr_set = thread_set_policy(quiet, pset, POLICY_FIFO,
					   (integer_t *) &fifo,
					   POLICY_FIFO_BASE_COUNT,
					   (integer_t *) &fifo_limit,
					   POLICY_FIFO_LIMIT_COUNT);
		n = THREAD_BASIC_INFO_COUNT;
		basic.policy = -1;
		(void) thread_info(quiet, THREAD_BASIC_INFO,
				   (thread_info_t) &basic, &n);

		/* The spinner: refused, then started. */
		kr_spin = thread_policy(spinner, POLICY_FIFO,
					(integer_t *) &fifo,
					POLICY_FIFO_BASE_COUNT, FALSE);
		for (i = 0; i < sizeof state; i++)
			((volatile char *) &state)[i] = 0;
		state.rip = (uint64_t) (uintptr_t) arm_twenty_five_spin;
		state.rsp = stack + vm_page_size - 8;	/* as a call leaves it */
		state.rflags = 0x202;
		if (thread_set_state(spinner, x86_64_THREAD_STATE,
				     (thread_state_t) &state,
				     x86_64_THREAD_STATE_COUNT) == KERN_SUCCESS &&
		    thread_resume(spinner) == KERN_SUCCESS)
			for (i = 0; i < ARM25_NAPS; i++)
				if (nap(10) == MACH_RCV_TIMED_OUT)
					naps++;

		n = HOST_BASIC_INFO_COUNT;
		if (host_info(host, HOST_BASIC_INFO, (host_info_t) &hi,
			      &n) == KERN_SUCCESS)
			cpus = hi.avail_cpus;

		if (kr_tf != KERN_INVALID_POLICY ||
		    kr_tr != KERN_INVALID_POLICY ||
		    kr_task != KERN_INVALID_POLICY || kr_set != KERN_SUCCESS ||
		    basic.policy != POLICY_FIFO ||
		    kr_spin != KERN_INVALID_POLICY || naps != ARM25_NAPS) {
			printf("act_test: [25] without the set's port FIFO "
			       "answered 0x%x, RR 0x%x, a task's FIFO 0x%x; with "
			       "it 0x%x, policy %d; the spinner's FIFO 0x%x, and "
			       "%d of %d naps beside it at %d processors — WRONG, "
			       "FIFO and RR are the set's control port's to give "
			       "(#786)\n", kr_tf, kr_tr, kr_task, kr_set,
			       (int) basic.policy, kr_spin, naps, ARM25_NAPS,
			       cpus);
			passed = 0;
		} else {
			printf("act_test: [25] without the set's port a thread's "
			       "FIFO and RR and a task's FIFO were refused with "
			       "0x%x; with it FIFO was given, policy %d; a thread "
			       "refused FIFO spun, and this one woke from %d of %d "
			       "naps beside it at %d processors (#786)\n", kr_tf,
			       (int) basic.policy, naps, ARM25_NAPS, cpus);
			passed = 1;
		}
	}

	if (spinner != MACH_PORT_NULL) {
		(void) thread_terminate(spinner);
		(void) mach_port_deallocate(me, spinner);
	}
	if (quiet != MACH_PORT_NULL) {
		(void) thread_terminate(quiet);
		(void) mach_port_deallocate(me, quiet);
	}
	if (stack != 0)
		(void) vm_deallocate(me, stack, vm_page_size);
	if (child != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, child);
	if (pset != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, pset);
	if (name != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, name);
	if (host_priv != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, host_priv);
	if (device != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, device);
	if (lw != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, lw);
	if (lp != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, lp);
	if (sec != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, sec);
	(void) mach_port_deallocate(me, host);
	return passed;
}

/*
 * Arm twenty-two (#786): a processor's control port is the privileged host
 * port's to give.  host_processors() gave them for the name port every task
 * has, and with one processor_exit() was queued, and the action thread ended
 * in switch_to_shutdown_context(), which x86-64 panics in (#453).  Five
 * answers, each needed:
 *  - host_processors() by the name port is refused;
 *  - host_processors() by the privileged port still answers -- the same
 *    call, so the port is the only thing the refusal can be about;
 *  - host_processor_info() by the name port answers the information alone,
 *    one record per processor, as many as the privileged call lists;
 *  - processor_get_assignment() of each says the default set;
 *  - processor_exit() of the last one listed is refused with
 *    KERN_NOT_SUPPORTED, and this program runs on to say so.
 * ⚠️ At one processor the one asked is the only one, the last of its set,
 * and a kernel that queued it would first stop every thread in the set: a
 * machine that stops, not one that panics.  The ablations run at four, where
 * the last one listed is not the one that booted.
 * Returns 1 when all five answers are right, 0 when not, -1 when it could not
 * be asked.
 */
static int
arm_twenty_two_processor_ports_are_privileged(void)
{
	mach_port_t		me = mach_task_self();
	mach_port_t		host = mach_host_self();
	mach_port_t		host_priv = MACH_PORT_NULL, device = MACH_PORT_NULL;
	mach_port_t		lw = MACH_PORT_NULL, lp = MACH_PORT_NULL;
	mach_port_t		sec = MACH_PORT_NULL, dflt = MACH_PORT_NULL;
	processor_port_array_t	by_name = NULL, list = NULL;
	mach_msg_type_number_t	n_by_name = 0, n = 0, ints;
	integer_t		info[PROCESSOR_INFO_ARRAY_MAX];
	natural_t		answered = 0;
	kern_return_t		kr_name, kr_priv, kr_info, kr_exit = KERN_FAILURE;
	unsigned		i, in_default = 0;
	int			passed = -1;

	if (bootstrap_ports(bootstrap_port, &host_priv, &device, &lw, &lp,
			    &sec) != KERN_SUCCESS ||
	    processor_set_default(host, &dflt) != KERN_SUCCESS)
		printf("act_test: [22] NOT ASKED — no privileged host port or "
		       "no default processor set\n");
	else {
		kr_name = host_processors(host, &by_name, &n_by_name);
		kr_priv = host_processors(host_priv, &list, &n);
		ints = PROCESSOR_INFO_ARRAY_MAX;
		kr_info = host_processor_info(host, PROCESSOR_CPU_LOAD_INFO,
					      &answered, info, &ints);

		for (i = 0; kr_priv == KERN_SUCCESS && i < n; i++) {
			mach_port_t	assigned = MACH_PORT_NULL;

			/* The same port, so the same name in this space. */
			if (processor_get_assignment(list[i], &assigned) ==
			    KERN_SUCCESS) {
				if (assigned == dflt)
					in_default++;
				(void) mach_port_deallocate(me, assigned);
			}
		}
		if (kr_priv == KERN_SUCCESS && n > 0)
			kr_exit = processor_exit(list[n - 1]);

		if (kr_name == KERN_SUCCESS || kr_priv != KERN_SUCCESS ||
		    n == 0 || kr_info != KERN_SUCCESS || answered != n ||
		    ints != answered * PROCESSOR_CPU_LOAD_INFO_COUNT ||
		    in_default != n || kr_exit != KERN_NOT_SUPPORTED) {
			printf("act_test: [22] host_processors answered the name "
			       "port 0x%x and the privileged one 0x%x, %u "
			       "processors; host_processor_info 0x%x, %u "
			       "processors in %u ints; %u in the default set; "
			       "processor_exit 0x%x — WRONG, a processor's port "
			       "is the privileged host port's to give (#786)\n",
			       kr_name, kr_priv, (unsigned) n, kr_info,
			       (unsigned) answered, (unsigned) ints, in_default,
			       kr_exit);
			passed = 0;
		} else {
			printf("act_test: [22] host_processors refused the name "
			       "port with 0x%x and gave the privileged one %u "
			       "processors, every one in the default set; "
			       "host_processor_info answered the name port %u "
			       "processors in %u ints; processor_exit of the last "
			       "one refused with 0x%x, and the machine runs on "
			       "(#786)\n", kr_name, (unsigned) n,
			       (unsigned) answered, (unsigned) ints, kr_exit);
			passed = 1;
		}
	}

	for (i = 0; i < n_by_name; i++)
		(void) mach_port_deallocate(me, by_name[i]);
	if (by_name != NULL)
		(void) vm_deallocate(me, (vm_address_t) by_name,
				     n_by_name * sizeof(*by_name));
	for (i = 0; i < n; i++)
		(void) mach_port_deallocate(me, list[i]);
	if (list != NULL)
		(void) vm_deallocate(me, (vm_address_t) list,
				     n * sizeof(*list));
	if (dflt != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, dflt);
	if (host_priv != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, host_priv);
	if (device != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, device);
	if (lw != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, lw);
	if (lp != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, lp);
	if (sec != MACH_PORT_NULL)
		(void) mach_port_deallocate(me, sec);
	(void) mach_port_deallocate(me, host);
	return passed;
}

/* ----------------------------------------------------------------
 * Arm twenty-six: what a receive that slept gives back in the six
 * registers the contract preserves (#771)
 * ---------------------------------------------------------------- */

/*
 * Arm five's other half.  <syscall/syscall.h> says rbx, rbp and r12-r15 come
 * back from a trap as they went in, and a trap that returns the way it came,
 * through SYSRET, keeps them for nothing: the kernel's compiled C has kept
 * them.  A receive that SLEEPS returns another way once continuations run
 * (#771): it gives up the frames it was called through and comes back through
 * the frame the entry wrote, so the six are there only if the entry put them
 * there.  Until #771 nothing came back that way and the entry never wrote
 * them -- and #771's first version woke every server that slept in a receive
 * with the rbp, r14 and r15 of that server's last trap, or the zeros its
 * frame began with.
 *
 * So the call is made the way a program makes it, through libmach's stub,
 * with a pattern in each of the six (preserved.S: C cannot hold a value in a
 * named register across a call), and it is made to sleep.  The message comes
 * from a thread of this program, sent only once the kernel reports the
 * receiver WAITING -- which it reports after thread_dispatch() has cleared
 * TH_RUN, so the receiver has blocked and been switched away from.  That says
 * it sleeps, not where; the first call below is what makes it the receive.
 * Without the entry's six stores this arm read rbp back as a user stack
 * address, the last trap's, on the first TCG boot it was asked.  The stub
 * keeps rbx, r12 and r13 itself, around the three arguments it carries in
 * them: on this trap, rbp, r14 and r15 are the kernel's to keep.
 *
 * ⚠️ A receive that did not sleep asked nothing -- it came back through
 * SYSRET whatever the entry writes -- so a sender that never saw the receiver
 * waiting makes this arm NOT ASKED, not passed.  The first call, with a zero
 * timeout, is not the question either: it brings the helper's pages in before
 * the receiver can be seen WAITING, since a thread asleep in a page fault on
 * its way into the receive reports the same.  It is checked all the same, as
 * SYSRET's half of the one promise.
 */
extern mach_msg_return_t		arm26_trap_with_patterns(
					    const unsigned long long *args,
					    unsigned long long *got);
extern const unsigned long long		arm26_patterns[6];

static mach_port_t		arm_26_port;
static volatile mach_port_t	arm_26_receiver;
static volatile int		arm_26_ready;
static volatile int		arm_26_saw_waiting;
static volatile kern_return_t	arm_26_send_kr = -1;

static void *
arm_26_sender(void *arg)
{
	mach_msg_header_t	msg;
	int			i;

	(void) arg;

	for (i = 0; i < PATIENCE && !arm_26_ready; i++)
		nap(100);

	for (i = 0; i < 10 * PATIENCE && arm_26_ready; i++) {
		struct thread_basic_info	basic;
		mach_msg_type_number_t		n = THREAD_BASIC_INFO_COUNT;

		basic.run_state = 0;
		(void) thread_info(arm_26_receiver, THREAD_BASIC_INFO,
				   (thread_info_t) &basic, &n);
		if (basic.run_state == TH_STATE_WAITING) {
			arm_26_saw_waiting = 1;
			break;
		}
		nap(10);
	}

	/*
	 * Sent whether or not the receiver was seen waiting: its receive has no
	 * timeout, and this is the only thing that ends it.
	 */
	msg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
	msg.msgh_size = sizeof msg;
	msg.msgh_remote_port = arm_26_port;
	msg.msgh_local_port = MACH_PORT_NULL;
	msg.msgh_id = 771;
	arm_26_send_kr = mach_msg(&msg, MACH_SEND_MSG, sizeof msg, 0,
				  MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
				  MACH_PORT_NULL);
	return NULL;
}

/* The first of the six that is not its pattern, by name; 0 if none is. */
static const char *
arm_26_lost(const unsigned long long *got, unsigned long long *bad)
{
	static const char *const nm[6] = {
		"rbx", "rbp", "r12", "r13", "r14", "r15"
	};
	int	j;

	for (j = 0; j < 6; j++)
		if (got[j] != arm26_patterns[j]) {
			*bad = got[j];
			return nm[j];
		}
	return 0;
}

/* The port given back on every way out, and the verdict passed through. */
static int
arm_26_done(int verdict)
{
	(void) mach_port_destroy(mach_task_self(), arm_26_port);
	return verdict;
}

static int
arm_twenty_six_a_receive_that_slept_keeps_six(void)
{
	pthread_t		sender;
	struct roomy_msg	msg;
	unsigned long long	args[9], got[6], bad = 0;
	mach_msg_return_t	mr;
	kern_return_t		kr;
	const char		*lost;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&arm_26_port);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [26] mach_port_allocate failed (%d) — WRONG\n",
		       kr);
		return 0;
	}

	/* mach_msg_overwrite_trap()'s nine, as libmach's mach_msg() passes them. */
	args[0] = (unsigned long long) (uintptr_t) &msg.h;
	args[1] = MACH_RCV_MSG | MACH_RCV_TIMEOUT;
	args[2] = 0;
	args[3] = sizeof msg;
	args[4] = arm_26_port;
	args[5] = 0;				/* the first call waits for nothing */
	args[6] = MACH_PORT_NULL;
	args[7] = 0;				/* MACH_MSG_NULL: into msg */
	args[8] = 0;

	mr = arm26_trap_with_patterns(args, got);
	if (mr != MACH_RCV_TIMED_OUT) {
		printf("act_test: [26] a receive with a zero timeout on an empty "
		       "port answered 0x%x, not MACH_RCV_TIMED_OUT — WRONG\n", mr);
		return arm_26_done(0);
	}
	lost = arm_26_lost(got, &bad);
	if (lost != 0) {
		printf("act_test: [26] %s came back through SYSRET holding 0x%llx, "
		       "not the pattern it went in with, and the contract "
		       "preserves it — WRONG\n", lost, bad);
		return arm_26_done(0);
	}

	arm_26_receiver = mach_thread_self();
	if (pthread_create(&sender, NULL, arm_26_sender, NULL) != 0) {
		printf("act_test: [26] pthread_create failed — WRONG\n");
		(void) mach_port_deallocate(mach_task_self(), arm_26_receiver);
		return arm_26_done(0);
	}

	args[1] = MACH_RCV_MSG;
	args[5] = MACH_MSG_TIMEOUT_NONE;
	arm_26_ready = 1;
	mr = arm26_trap_with_patterns(args, got);
	arm_26_ready = 0;
	(void) pthread_join(sender, NULL);
	(void) mach_port_deallocate(mach_task_self(), arm_26_receiver);

	if (mr != MACH_MSG_SUCCESS || arm_26_send_kr != MACH_MSG_SUCCESS) {
		printf("act_test: [26] the receive answered 0x%x and the send "
		       "0x%x — WRONG\n", mr, arm_26_send_kr);
		return arm_26_done(0);
	}
	if (!arm_26_saw_waiting) {
		printf("act_test: [26] NOT ASKED — the sender never saw the "
		       "receiver WAITING, so the receive may not have slept and "
		       "came back through SYSRET (#771)\n");
		return arm_26_done(-1);
	}
	lost = arm_26_lost(got, &bad);
	if (lost != 0) {
		printf("act_test: [26] %s came back from a receive that slept "
		       "holding 0x%llx, not the pattern it went in with, and the "
		       "contract preserves it (#771) — WRONG\n", lost, bad);
		return arm_26_done(0);
	}

	printf("act_test: [26] a receive the kernel reported WAITING came back "
	       "with rbx, rbp and r12-r15 as they went in: rbp 0x%llx, r14 "
	       "0x%llx, r15 0x%llx, read back (#771)\n", got[1], got[4], got[5]);
	return arm_26_done(1);
}

/*
 * Arm twenty-seven (#808): a send whose size cannot take the trailer is
 * refused, cleanly, before the kernel adds the trailer to it.
 *
 * ipc_kmsg_get() sizes the kernel buffer by `send_size + MAX_TRAILER_SIZE' in
 * mach_msg_size_t, which is 32 bits, and then copies `send_size' bytes into
 * it.  A send_size near 2^32 wraps the sum to a small buffer and the copy runs
 * past it -- the kernel heap overrun of #808.  The fix refuses such a size up
 * front with MACH_SEND_TOO_LARGE, before any buffer or copy, so on a healthy
 * tree this arm overruns nothing: the trap returns at its first test.
 *
 * The message is a bare header with a real send right as its destination, so
 * nothing but its size is wrong; the size check precedes the port work, so the
 * destination only keeps the trap on the ordinary send path.
 */
static int
arm_twenty_seven_send_size_cannot_take_the_trailer(void)
{
	mach_port_t		port;
	mach_msg_header_t	hdr;
	mach_msg_return_t	mr;
	kern_return_t		kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&port);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [27] mach_port_allocate failed (%d) — "
		       "WRONG\n", kr);
		return 0;
	}
	kr = mach_port_insert_right(mach_task_self(), port, port,
				    MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [27] insert_right failed (%d) — WRONG\n", kr);
		(void) mach_port_destroy(mach_task_self(), port);
		return 0;
	}

	hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	hdr.msgh_size = sizeof hdr;
	hdr.msgh_remote_port = port;
	hdr.msgh_local_port = MACH_PORT_NULL;
	hdr.msgh_reserved = 0;
	hdr.msgh_id = 0;

	/*
	 * A multiple of four, at least a header, and larger than any size the
	 * trailer fits after: the value the kernel must refuse rather than add
	 * to.  The buffer handed over is one header; the fix never reads past
	 * it.
	 */
	mr = mach_msg(&hdr, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
		      (mach_msg_size_t) 0xFFFFFFFCu, 0, MACH_PORT_NULL,
		      0, MACH_PORT_NULL);

	(void) mach_port_destroy(mach_task_self(), port);

	if (mr != MACH_SEND_TOO_LARGE) {
		printf("act_test: [27] a send of 0xFFFFFFFC bytes answered "
		       "0x%x, not MACH_SEND_TOO_LARGE — WRONG (#808)\n",
		       (int) mr);
		return 0;
	}

	printf("act_test: [27] a send whose size cannot take the trailer was "
	       "refused with MACH_SEND_TOO_LARGE, before the buffer (#808)\n");
	return 1;
}

/*
 * A receive right with a send right beside it, for the arms below that only
 * need a port of their own.  MACH_PORT_NULL, said, when either step fails.
 */
static mach_port_t
arm_own_port(int arm)
{
	mach_port_t	port;
	kern_return_t	kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&port);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [%d] mach_port_allocate failed (%d) — "
		       "WRONG\n", arm, kr);
		return MACH_PORT_NULL;
	}
	kr = mach_port_insert_right(mach_task_self(), port, port,
				    MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [%d] insert_right failed (%d) — WRONG\n",
		       arm, kr);
		(void) mach_port_destroy(mach_task_self(), port);
		return MACH_PORT_NULL;
	}
	return port;
}

/*
 * Arm twenty-eight (#808): a send past the largest message the kernel takes
 * inline is refused as too large, not left to fail for want of a buffer.
 *
 * The kernel's buffer for a message comes from kalloc, which has nothing past
 * kalloc_map -- 8 MiB -- to give.  Since #808 a send past IKM_MAX_MSG_SIZE, a
 * little less than that, is answered MACH_SEND_TOO_LARGE before any buffer is
 * asked for; before it the size passed the only bound there was, the
 * allocation failed, and the answer was MACH_SEND_NO_BUFFER.  The message is a
 * bare header at the start of 8 MiB and a page of mapped memory, so a kernel
 * that tried to copy it in would find every byte it asked for.
 */
#define	A28_SIZE	((mach_msg_size_t) (8u * 1024 * 1024 + 4))

static int
arm_twenty_eight_send_past_the_inline_limit(void)
{
	vm_address_t		buf = 0;
	vm_size_t		len = A28_SIZE + vm_page_size;
	mach_msg_header_t	*hdr;
	mach_msg_return_t	mr;
	mach_port_t		port;
	kern_return_t		kr;

	kr = vm_allocate(mach_task_self(), &buf, len, TRUE);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [28] vm_allocate of %lu bytes failed (%d) — "
		       "WRONG\n", (unsigned long) len, kr);
		return 0;
	}
	port = arm_own_port(28);
	if (port == MACH_PORT_NULL) {
		(void) vm_deallocate(mach_task_self(), buf, len);
		return 0;
	}

	hdr = (mach_msg_header_t *) buf;
	hdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	hdr->msgh_size = A28_SIZE;
	hdr->msgh_remote_port = port;
	hdr->msgh_local_port = MACH_PORT_NULL;
	hdr->msgh_reserved = 0;
	hdr->msgh_id = 0;

	mr = mach_msg(hdr, MACH_SEND_MSG | MACH_SEND_TIMEOUT, A28_SIZE, 0,
		      MACH_PORT_NULL, 0, MACH_PORT_NULL);

	(void) mach_port_destroy(mach_task_self(), port);
	(void) vm_deallocate(mach_task_self(), buf, len);

	if (mr != MACH_SEND_TOO_LARGE) {
		printf("act_test: [28] a send of %u bytes, past the largest "
		       "inline message, answered 0x%x%s, not MACH_SEND_TOO_LARGE "
		       "— WRONG (#808)\n", (unsigned) A28_SIZE, (int) mr,
		       mr == MACH_SEND_NO_BUFFER
		       ? " (MACH_SEND_NO_BUFFER: the kernel tried to allocate it)"
		       : "");
		return 0;
	}

	printf("act_test: [28] a send of %u bytes, past the largest inline "
	       "message, was refused with MACH_SEND_TOO_LARGE before any "
	       "buffer (#808)\n", (unsigned) A28_SIZE);
	return 1;
}

/*
 * Arm twenty-nine (#808): a receive is measured with the trailer it asks for.
 *
 * The queue compared the message with rcv_size less the smallest trailer,
 * because mach_msg_receive() handed it the timeout bit and nothing else of
 * the option, while the copyout then wrote the trailer the option asked for:
 * a message that fitted with the smallest trailer and not with the sender's
 * was taken, and written past rcv_size into the receiver's own memory.  Here
 * the buffer is rcv_size bytes with canaries after them, rcv_size holding the
 * message and the smallest trailer only; the receive must answer
 * MACH_RCV_TOO_LARGE and leave every canary.
 */
#define	A29_CANARY	0xa5

static int
arm_twenty_nine_receive_measured_with_its_trailer(void)
{
	struct {
		mach_msg_header_t	h;
		unsigned int		body[6];
	} out;
	union {
		mach_msg_header_t	h;
		unsigned char		bytes[256];
	} in;
	mach_msg_size_t		rcv_size = sizeof out + sizeof (mach_msg_trailer_t);
	mach_msg_return_t	mr;
	mach_port_t		port;
	unsigned		i, hit = 0;

	port = arm_own_port(29);
	if (port == MACH_PORT_NULL)
		return 0;

	out.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	out.h.msgh_size = sizeof out;
	out.h.msgh_remote_port = port;
	out.h.msgh_local_port = MACH_PORT_NULL;
	out.h.msgh_reserved = 0;
	out.h.msgh_id = 29;
	for (i = 0; i < 6; i++)
		out.body[i] = i;

	mr = mach_msg(&out.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof out, 0,
		      MACH_PORT_NULL, 0, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS) {
		printf("act_test: [29] the send answered 0x%x — WRONG\n",
		       (int) mr);
		(void) mach_port_destroy(mach_task_self(), port);
		return 0;
	}

	for (i = 0; i < sizeof in.bytes; i++)
		in.bytes[i] = A29_CANARY;

	mr = mach_msg(&in.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT |
		      MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
		      MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_SENDER),
		      0, rcv_size, port, 0, MACH_PORT_NULL);

	for (i = rcv_size; i < sizeof in.bytes; i++)
		if (in.bytes[i] != A29_CANARY)
			hit++;

	(void) mach_port_destroy(mach_task_self(), port);

	if (mr != MACH_RCV_TOO_LARGE || hit != 0) {
		printf("act_test: [29] a %u-byte message received into %u bytes "
		       "with the sender's trailer asked for answered 0x%x, and %u "
		       "bytes past rcv_size changed — WRONG, expected "
		       "MACH_RCV_TOO_LARGE and none (#808)\n",
		       (unsigned) sizeof out, (unsigned) rcv_size, (int) mr, hit);
		return 0;
	}

	printf("act_test: [29] a %u-byte message received into %u bytes with the "
	       "sender's trailer asked for was MACH_RCV_TOO_LARGE, and nothing "
	       "past rcv_size changed (#808)\n", (unsigned) sizeof out,
	       (unsigned) rcv_size);
	return 1;
}

/*
 * Arm thirty (#808): a scatter list is measured with its body's own size.
 *
 * With MACH_RCV_OVERWRITE the receive buffer, past its header, is a list of
 * descriptors: a body, then as many descriptors as the body says.  The check
 * added the count's 4 bytes for the body, which on x86-64 is 8, so a list
 * that ended 4 bytes short of its last descriptor passed it.  Here the list
 * is a body that says one descriptor and room for 4 bytes less than that,
 * and no message is queued: the receive must answer MACH_RCV_INVALID_TYPE,
 * from the list, before it looks at the queue -- a kernel that let the list
 * through answers MACH_RCV_TIMED_OUT instead.
 */
static int
arm_thirty_scatter_list_one_word_short(void)
{
	union {
		mach_msg_header_t	h;
		unsigned char		bytes[128];
	} buf;
	mach_msg_body_t		*body = (mach_msg_body_t *) (&buf.h + 1);
	mach_msg_size_t		scatter_size;
	mach_msg_return_t	mr;
	mach_port_t		port;
	unsigned		i;

	scatter_size = sizeof (mach_msg_header_t) + 4
		       + sizeof (mach_msg_descriptor_t);

	port = arm_own_port(30);
	if (port == MACH_PORT_NULL)
		return 0;

	for (i = 0; i < sizeof buf.bytes; i++)
		buf.bytes[i] = 0;
	body->msgh_descriptor_count = 1;

	mr = mach_msg_overwrite(&buf.h, MACH_RCV_MSG | MACH_RCV_OVERWRITE |
				MACH_RCV_TIMEOUT, 0, sizeof buf, port, 0,
				MACH_PORT_NULL, &buf.h, scatter_size);

	(void) mach_port_destroy(mach_task_self(), port);

	if (mr != MACH_RCV_INVALID_TYPE) {
		printf("act_test: [30] a scatter list of %u bytes for one "
		       "%u-byte descriptor after an %u-byte body answered 0x%x, "
		       "not MACH_RCV_INVALID_TYPE — WRONG (#808)\n",
		       (unsigned) (scatter_size - sizeof (mach_msg_header_t)),
		       (unsigned) sizeof (mach_msg_descriptor_t),
		       (unsigned) sizeof (mach_msg_body_t), (int) mr);
		return 0;
	}

	printf("act_test: [30] a scatter list of %u bytes for one %u-byte "
	       "descriptor after an %u-byte body was refused with "
	       "MACH_RCV_INVALID_TYPE (#808)\n",
	       (unsigned) (scatter_size - sizeof (mach_msg_header_t)),
	       (unsigned) sizeof (mach_msg_descriptor_t),
	       (unsigned) sizeof (mach_msg_body_t));
	return 1;
}

/*
 * Arm thirty-one (#840): a complex message's descriptor count is bounded by
 * the message's size in bytes.
 *
 * ipc_kmsg_copyin_body() compared the descriptors' end with
 * `&kmsg->ikm_header + msgh_size', a sum that moves msgh_size headers of 24
 * bytes: a count of up to 24 times what the message holds passed, and the
 * descriptors past its end were taken as its own.  Here a message holds one
 * port descriptor and says two, then fifty, and each send must answer
 * MACH_SEND_MSG_TOO_SMALL.  One too many goes first: on a kernel without the
 * bound the second descriptor is read from just past the message, where the
 * kernel's own trailer lies, and the send answers something else -- fifty
 * would read whatever follows the buffer, so it is not asked after a WRONG.
 */
static int
arm_thirty_one_descriptor_count_past_the_message(void)
{
	struct {
		mach_msg_header_t		h;
		mach_msg_body_t			b;
		mach_msg_port_descriptor_t	d;
	} m;
	static const unsigned	says[] = { 2, 50 };
	mach_msg_return_t	mr;
	mach_port_t		port;
	unsigned		i, j;

	port = arm_own_port(31);
	if (port == MACH_PORT_NULL)
		return 0;

	for (i = 0; i < sizeof says / sizeof says[0]; i++) {
		for (j = 0; j < sizeof m; j++)
			((unsigned char *) &m)[j] = 0;
		m.h.msgh_bits = MACH_MSGH_BITS_COMPLEX
				| MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		m.h.msgh_size = sizeof m;
		m.h.msgh_remote_port = port;
		m.h.msgh_local_port = MACH_PORT_NULL;
		m.h.msgh_id = 31;
		m.b.msgh_descriptor_count = says[i];
		m.d.name = MACH_PORT_NULL;
		m.d.disposition = MACH_MSG_TYPE_COPY_SEND;
		m.d.type = MACH_MSG_PORT_DESCRIPTOR;

		mr = mach_msg(&m.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m,
			      0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
		if (mr != MACH_SEND_MSG_TOO_SMALL) {
			printf("act_test: [31] a %u-byte message holding one "
			       "descriptor and saying %u answered 0x%x, not "
			       "MACH_SEND_MSG_TOO_SMALL — WRONG (#840)\n",
			       (unsigned) sizeof m, says[i], (int) mr);
			(void) mach_port_destroy(mach_task_self(), port);
			return 0;
		}
	}

	(void) mach_port_destroy(mach_task_self(), port);
	printf("act_test: [31] a %u-byte message holding one descriptor and "
	       "saying 2, then 50, was refused with MACH_SEND_MSG_TOO_SMALL "
	       "both times (#840)\n", (unsigned) sizeof m);
	return 1;
}

/*
 * Arm thirty-two (#841): an exception answered with more than the kernel's
 * stub asked for is a failed answer, and nothing more.
 *
 * The kernel raises an exception with mach_msg_rpc_from_kernel(), receiving
 * the answer into the generated stub's union of request and reply, on its own
 * stack, with rcv_size the reply it expects -- 36 bytes for exception_raise.
 * The size of the answer is the answerer's.  When it was larger, the kernel
 * said MACH_RCV_TOO_LARGE, but copied all of it into that union first.
 *
 * Here a thread's own exception port gets the exception and is answered with
 * 4096 bytes of zeroes, then the task's port gets it, as it must when the
 * thread's answer failed, and the thread is ended there.  A kernel that copies
 * the 4096 bytes writes them over its own stack, and the returns that follow
 * come back to nothing.  The address the thread loads from is mapped by no arm.
 */
#define	A32_PROBE	((vm_offset_t) 0x0000500000000000ULL)
#define	A32_ANSWER	4096
#define	A32_RAISE_ID	2401	/* exception_raise, exc.defs */

static mach_port_t		arm_thirty_two_port;
static volatile kern_return_t	arm_thirty_two_set_kr = -1;

static void *
arm_thirty_two_victim(void *arg)
{
	(void) arg;
	arm_thirty_two_thread = mach_thread_self();
	arm_thirty_two_set_kr =
	    thread_set_exception_ports(arm_thirty_two_thread,
				       EXC_MASK_BAD_ACCESS, arm_thirty_two_port,
				       EXCEPTION_DEFAULT, THREAD_STATE_NONE);
	if (arm_thirty_two_set_kr == KERN_SUCCESS)
		(void) *(volatile unsigned long *) A32_PROBE;
	return NULL;
}

static int
arm_thirty_two_exception_answered_too_large(void)
{
	union {
		mach_msg_header_t	h;
		unsigned char		bytes[1024];
	} req, kreq, kans;
	static union {
		mach_msg_header_t	h;
		unsigned char		bytes[A32_ANSWER];
	} ans;
	mach_port_t		tport, kport;
	mach_msg_return_t	mr, sent = MACH_MSG_SUCCESS;
	kern_return_t		kr;
	pthread_t		victim;
	unsigned		i;
	int			ok = 0;

	tport = arm_own_port(32);
	if (tport == MACH_PORT_NULL)
		return 0;
	kport = arm_own_port(32);
	if (kport == MACH_PORT_NULL) {
		(void) mach_port_destroy(mach_task_self(), tport);
		return 0;
	}
	kr = task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS,
				      kport, EXCEPTION_DEFAULT,
				      THREAD_STATE_NONE);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [32] task_set_exception_ports failed (%d) — "
		       "WRONG\n", kr);
		(void) mach_port_destroy(mach_task_self(), tport);
		(void) mach_port_destroy(mach_task_self(), kport);
		return 0;
	}

	arm_thirty_two_port = tport;
	arm_thirty_two_armed = 1;
	if (pthread_create(&victim, NULL, arm_thirty_two_victim, NULL) != 0) {
		printf("act_test: [32] pthread_create failed — WRONG\n");
		arm_thirty_two_armed = 0;
		ok = -1;
	}

	/* The thread's port first: the request, and an answer far too large. */
	if (ok == 0) {
		mr = mach_msg(&req.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			      sizeof req, tport, 5000, MACH_PORT_NULL);
		if (mr != MACH_MSG_SUCCESS || req.h.msgh_id != A32_RAISE_ID) {
			printf("act_test: [32] the thread's port got 0x%x, id %d, "
			       "not exception_raise (the set answered %d) — "
			       "WRONG\n", (int) mr,
			       mr == MACH_MSG_SUCCESS ? req.h.msgh_id : -1,
			       (int) arm_thirty_two_set_kr);
			ok = -1;
		}
	}
	if (ok == 0) {
		for (i = 0; i < sizeof ans.bytes; i++)
			ans.bytes[i] = 0;
		ans.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
		ans.h.msgh_size = A32_ANSWER;
		ans.h.msgh_remote_port = req.h.msgh_remote_port;
		ans.h.msgh_local_port = MACH_PORT_NULL;
		ans.h.msgh_id = req.h.msgh_id + 100;
		sent = mach_msg(&ans.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
				A32_ANSWER, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);

		/*
		 * The request was received raw, not through exc_server, so its
		 * two rights -- the thread's and the task's -- are this arm's
		 * to give back.
		 */
		{
			struct {
				mach_msg_header_t		h;
				mach_msg_body_t			b;
				mach_msg_port_descriptor_t	thread, task;
			} *rq = (void *) &req;

			if ((req.h.msgh_bits & MACH_MSGH_BITS_COMPLEX)
			    && rq->b.msgh_descriptor_count >= 2) {
				(void) mach_port_deallocate(mach_task_self(),
							    rq->thread.name);
				(void) mach_port_deallocate(mach_task_self(),
							    rq->task.name);
			}
		}

		/* Then the task's port, answered by catch_exception_raise(). */
		mr = mach_msg(&kreq.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			      sizeof kreq, kport, 5000, MACH_PORT_NULL);
		if (mr == MACH_MSG_SUCCESS) {
			(void) exc_server(&kreq.h, &kans.h);
			(void) mach_msg(&kans.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
					kans.h.msgh_size, 0, MACH_PORT_NULL, 0,
					MACH_PORT_NULL);
		}
	}

	arm_thirty_two_armed = 0;
	(void) task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS,
					MACH_PORT_NULL, EXCEPTION_DEFAULT,
					THREAD_STATE_NONE);
	if (arm_thirty_two_thread != MACH_PORT_NULL)
		(void) mach_port_deallocate(mach_task_self(),
					    arm_thirty_two_thread);
	(void) mach_port_destroy(mach_task_self(), tport);
	(void) mach_port_destroy(mach_task_self(), kport);

	if (ok != 0)
		return 0;
	if (sent != MACH_MSG_SUCCESS || !arm_thirty_two_task_seen
	    || arm_thirty_two_exception != EXC_BAD_ACCESS
	    || arm_thirty_two_named_the_victim != 1) {
		printf("act_test: [32] after a %d-byte answer at the thread's "
		       "port (sent 0x%x), the task's port %s exception %d, "
		       "naming the thread %d — WRONG, expected EXC_BAD_ACCESS "
		       "there, for the same thread (#841)\n", A32_ANSWER,
		       (int) sent, arm_thirty_two_task_seen ? "got"
						     : "never got",
		       arm_thirty_two_exception,
		       arm_thirty_two_named_the_victim);
		return 0;
	}

	printf("act_test: [32] an exception answered at the thread's port with "
	       "%d bytes, where the kernel's stub reads 36, was taken as a "
	       "failed answer and went on to the task's port (#841)\n",
	       A32_ANSWER);
	return 1;
}

/*
 * Arm thirty-three (#842): a receive asking for a trailer this kernel does not
 * have is refused before anything is received.
 *
 * The elements a receive asks for are four bits of its option, and indexed a
 * table of three trailer sizes with nothing to stop them: past it, a value
 * read from beyond the table became the trailer's size.  Here three receives
 * ask for elements 3 and 15 and for trailer format 1, on a port with nothing
 * queued and a timeout of zero.  Each must answer MACH_RCV_INVALID_TRAILER; a
 * kernel that does not look answers MACH_RCV_TIMED_OUT, before it gets as far
 * as the table, so asking is safe there too.
 */
static int
arm_thirty_three_trailer_not_there(void)
{
	static const struct {
		mach_msg_option_t	trailer;
		const char		*what;
	} ask[] = {
		{ MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0)
		  | MACH_RCV_TRAILER_ELEMENTS(3), "element 3" },
		{ MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0)
		  | MACH_RCV_TRAILER_ELEMENTS(15), "element 15" },
		{ MACH_RCV_TRAILER_TYPE(1), "format 1" },
	};
	union {
		mach_msg_header_t	h;
		unsigned char		bytes[256];
	} buf;
	mach_msg_return_t	mr;
	mach_port_t		port;
	unsigned		i;

	port = arm_own_port(33);
	if (port == MACH_PORT_NULL)
		return 0;

	for (i = 0; i < sizeof ask / sizeof ask[0]; i++) {
		mr = mach_msg(&buf.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT
			      | ask[i].trailer, 0, sizeof buf, port, 0,
			      MACH_PORT_NULL);
		if (mr != MACH_RCV_INVALID_TRAILER) {
			printf("act_test: [33] a receive asking for trailer %s "
			       "answered 0x%x, not MACH_RCV_INVALID_TRAILER — "
			       "WRONG (#842)\n", ask[i].what, (int) mr);
			(void) mach_port_destroy(mach_task_self(), port);
			return 0;
		}
	}

	(void) mach_port_destroy(mach_task_self(), port);
	printf("act_test: [33] receives asking for trailer element 3, element "
	       "15 and format 1 were each refused with MACH_RCV_INVALID_TRAILER "
	       "(#842)\n");
	return 1;
}

/*
 * Arm thirty-four (#844): an RPC whose reply does not fit is told so with the
 * reply's own sequence number in its trailer.
 *
 * On the hot path of mach_msg_overwrite_trap(), a reply too large for
 * rcv_size goes to slow_copyout, where msg_receive_error() writes a header
 * and the trailer asked for -- and took the sequence number from a variable
 * only the slow receive sets, so on the hot path it was whatever the kernel's
 * stack held.  Here the reply port has had three messages received from it,
 * so the replies are its fourth and on, sequence numbers 3, 4, ...; a server
 * thread answers A34_ROUNDS RPCs, each with 256 bytes into a receive of 64
 * that asks for the sequence number, and each trailer must say its own reply's.
 *
 * Many rounds and not one, because only an RPC the hot path carries reaches
 * the defect, and one RPC did not reliably take it: a first ablation with a
 * single round caught 1 boot of 4.  After the first round the server is
 * already waiting in its receive when the next request comes, which is when
 * the hot path hands off.
 */
#define	A34_PRELOAD	3
#define	A34_ROUNDS	32
#define	A34_REPLY	256
#define	A34_RCV		64

static mach_port_t		arm_thirty_four_service;
static volatile mach_msg_return_t arm_thirty_four_server_rcv = -1;
static volatile mach_msg_return_t arm_thirty_four_server_snd = -1;

static void *
arm_thirty_four_server(void *arg)
{
	union {
		mach_msg_header_t	h;
		unsigned char		bytes[256];
	} req;
	static union {
		mach_msg_header_t	h;
		unsigned char		bytes[A34_REPLY];
	} rep;
	unsigned	i, round;

	(void) arg;
	for (i = 0; i < sizeof rep.bytes; i++)
		rep.bytes[i] = 0;

	for (round = 0; round < A34_ROUNDS; round++) {
		arm_thirty_four_server_rcv =
		    mach_msg(&req.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			     sizeof req, arm_thirty_four_service, 10000,
			     MACH_PORT_NULL);
		if (arm_thirty_four_server_rcv != MACH_MSG_SUCCESS)
			return NULL;

		rep.h.msgh_bits = MACH_MSGH_BITS(
		    MACH_MSGH_BITS_REMOTE(req.h.msgh_bits), 0);
		rep.h.msgh_size = A34_REPLY;
		rep.h.msgh_remote_port = req.h.msgh_remote_port;
		rep.h.msgh_local_port = MACH_PORT_NULL;
		rep.h.msgh_id = req.h.msgh_id + 100;
		arm_thirty_four_server_snd =
		    mach_msg(&rep.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
			     A34_REPLY, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
	}
	return NULL;
}

static int
arm_thirty_four_rpc_reply_seqno(void)
{
	mach_msg_header_t	small;
	union {
		mach_msg_header_t	h;
		unsigned char		bytes[A34_RCV];
	} buf;
	mach_msg_seqno_trailer_t	*tr;
	mach_port_t		reply;
	mach_msg_return_t	mr, bad_mr = MACH_MSG_SUCCESS;
	pthread_t		server;
	unsigned		i, round, bad_round = A34_ROUNDS;
	unsigned		bad_seqno = 0;

	arm_thirty_four_service = arm_own_port(34);
	if (arm_thirty_four_service == MACH_PORT_NULL)
		return 0;
	reply = arm_own_port(34);
	if (reply == MACH_PORT_NULL) {
		(void) mach_port_destroy(mach_task_self(),
					 arm_thirty_four_service);
		return 0;
	}

	/* Three messages through the reply port, so the next is number 3. */
	for (i = 0; i < A34_PRELOAD; i++) {
		small.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		small.msgh_size = sizeof small;
		small.msgh_remote_port = reply;
		small.msgh_local_port = MACH_PORT_NULL;
		small.msgh_reserved = 0;
		small.msgh_id = 34;
		mr = mach_msg(&small, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
			      sizeof small, 0, MACH_PORT_NULL, 0,
			      MACH_PORT_NULL);
		if (mr == MACH_MSG_SUCCESS)
			mr = mach_msg(&buf.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
				      sizeof buf, reply, 0, MACH_PORT_NULL);
		if (mr != MACH_MSG_SUCCESS) {
			printf("act_test: [34] message %u through the reply port "
			       "answered 0x%x — WRONG\n", i, (int) mr);
			(void) mach_port_destroy(mach_task_self(), reply);
			(void) mach_port_destroy(mach_task_self(),
						 arm_thirty_four_service);
			return 0;
		}
	}

	if (pthread_create(&server, NULL, arm_thirty_four_server, NULL) != 0) {
		printf("act_test: [34] pthread_create failed — WRONG\n");
		(void) mach_port_destroy(mach_task_self(), reply);
		(void) mach_port_destroy(mach_task_self(),
					 arm_thirty_four_service);
		return 0;
	}
	/* Let the server get to its receive, so the RPC can hand off to it. */
	for (i = 0; i < 20 && arm_thirty_four_server_rcv == -1; i++)
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 1);

	for (round = 0; round < A34_ROUNDS; round++) {
		for (i = 0; i < sizeof buf.bytes; i++)
			buf.bytes[i] = 0;
		buf.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
						 MACH_MSG_TYPE_MAKE_SEND_ONCE);
		buf.h.msgh_size = sizeof (mach_msg_header_t);
		buf.h.msgh_remote_port = arm_thirty_four_service;
		buf.h.msgh_local_port = reply;
		buf.h.msgh_reserved = 0;
		buf.h.msgh_id = 3400;
		mr = mach_msg(&buf.h, MACH_SEND_MSG | MACH_RCV_MSG
			      | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0)
			      | MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_SEQNO),
			      sizeof (mach_msg_header_t), A34_RCV, reply,
			      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		tr = (mach_msg_seqno_trailer_t *)
		     (buf.bytes + ((sizeof (mach_msg_header_t) + 3) & ~3UL));

		if (bad_round == A34_ROUNDS
		    && (mr != MACH_RCV_TOO_LARGE
			|| tr->msgh_seqno != A34_PRELOAD + round)) {
			bad_round = round;
			bad_seqno = (unsigned) tr->msgh_seqno;
			bad_mr = mr;
		}
		/* A request that was never sent leaves the server waiting. */
		if (mr != MACH_RCV_TOO_LARGE && mr != MACH_MSG_SUCCESS)
			break;
	}

	(void) pthread_join(server, NULL);
	(void) mach_port_destroy(mach_task_self(), reply);
	(void) mach_port_destroy(mach_task_self(), arm_thirty_four_service);

	if (bad_round < A34_ROUNDS) {
		printf("act_test: [34] RPC %u of %u, whose %u-byte reply does not "
		       "fit %u bytes, answered 0x%x, its trailer saying sequence "
		       "number %u (server: 0x%x, 0x%x) — WRONG, expected "
		       "MACH_RCV_TOO_LARGE and %u (#844)\n", bad_round + 1,
		       A34_ROUNDS, A34_REPLY, A34_RCV, (int) bad_mr, bad_seqno,
		       (int) arm_thirty_four_server_rcv,
		       (int) arm_thirty_four_server_snd,
		       A34_PRELOAD + bad_round);
		return 0;
	}

	printf("act_test: [34] %u RPCs whose %u-byte replies do not fit %u bytes "
	       "were each MACH_RCV_TOO_LARGE, their trailers saying their own "
	       "sequence numbers, %u to %u, read back (#844)\n", A34_ROUNDS,
	       A34_REPLY, A34_RCV, A34_PRELOAD, A34_PRELOAD + A34_ROUNDS - 1);
	return 1;
}

/*
 * Arm thirty-five (#843): an out-of-line port array the receiver has no room
 * for is released alone; the descriptors delivered before it stay delivered.
 *
 * ipc_kmsg_copyout_body() delivers a message's descriptors in order.  When
 * vm_allocate() failed for a port array in the receiver's map, it cleaned the
 * whole body from the first descriptor, so rights already moved to the
 * receiver were released a second time.
 *
 * Here a port P, with a no-senders notification asked for, has one send
 * right, and it travels in a port descriptor; a port array follows, and the
 * message is received with this task's address space full, so the array has
 * nowhere to go.  The receive must report the array's failure, P's right must
 * arrive, and P must not hear "no senders": the right is this task's again.
 * A kernel that releases it twice takes P's count of send rights to zero while
 * the right is held, and the notification comes.  Nothing is printed while
 * the space is full: printing may need memory.
 */
#define	A35_FILL_MAX	1024

static vm_address_t	a35_fill_addr[A35_FILL_MAX];
static vm_size_t	a35_fill_size[A35_FILL_MAX];

/* Every hole of the address space taken, largest first; the count taken. */
static unsigned
a35_fill_the_space(void)
{
	vm_size_t	size;
	unsigned	n = 0;

	for (size = (vm_size_t) 1 << 46; size >= vm_page_size; size >>= 1) {
		while (n < A35_FILL_MAX) {
			vm_address_t	addr = 0;

			if (vm_allocate(mach_task_self(), &addr, size, TRUE)
			    != KERN_SUCCESS)
				break;
			a35_fill_addr[n] = addr;
			a35_fill_size[n] = size;
			n++;
		}
	}
	return n;
}

static void
a35_empty_the_space(unsigned n)
{
	while (n > 0) {
		n--;
		(void) vm_deallocate(mach_task_self(), a35_fill_addr[n],
				     a35_fill_size[n]);
	}
}

static int
arm_thirty_five_port_array_with_no_room(void)
{
	struct {
		mach_msg_header_t		h;
		mach_msg_body_t			b;
		mach_msg_port_descriptor_t	port;
		mach_msg_ool_ports_descriptor_t	ports;
	} m;
	union {
		mach_msg_header_t	h;
		unsigned char		bytes[512];
	} r, note;
	mach_port_t		dest, p, q, notify, prev = MACH_PORT_NULL;
	mach_port_t		one[1];
	mach_port_urefs_t	refs = 0;
	mach_msg_return_t	mr, sent, heard;
	vm_address_t		probe = 0;
	kern_return_t		kr, full;
	unsigned		filled, i;
	int			ok = 0;

	dest = arm_own_port(35);
	p = arm_own_port(35);		/* one send right: the one that travels */
	q = arm_own_port(35);
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&notify);
	if (dest == MACH_PORT_NULL || p == MACH_PORT_NULL
	    || q == MACH_PORT_NULL || kr != KERN_SUCCESS) {
		printf("act_test: [35] the ports could not be made — WRONG\n");
		return 0;
	}
	kr = mach_port_request_notification(mach_task_self(), p,
					    MACH_NOTIFY_NO_SENDERS, 0, notify,
					    MACH_MSG_TYPE_MAKE_SEND_ONCE, &prev);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [35] the no-senders request answered %d — "
		       "WRONG\n", kr);
		return 0;
	}

	for (i = 0; i < sizeof m; i++)
		((unsigned char *) &m)[i] = 0;
	m.h.msgh_bits = MACH_MSGH_BITS_COMPLEX
			| MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	m.h.msgh_size = sizeof m;
	m.h.msgh_remote_port = dest;
	m.h.msgh_local_port = MACH_PORT_NULL;
	m.h.msgh_id = 35;
	m.b.msgh_descriptor_count = 2;
	m.port.name = p;
	m.port.disposition = MACH_MSG_TYPE_MOVE_SEND;
	m.port.type = MACH_MSG_PORT_DESCRIPTOR;
	one[0] = q;
	m.ports.address = one;
	m.ports.count = 1;
	m.ports.deallocate = FALSE;
	m.ports.copy = MACH_MSG_PHYSICAL_COPY;
	m.ports.disposition = MACH_MSG_TYPE_COPY_SEND;
	m.ports.type = MACH_MSG_OOL_PORTS_DESCRIPTOR;

	sent = mach_msg(&m.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m, 0,
			MACH_PORT_NULL, 0, MACH_PORT_NULL);

	for (i = 0; i < sizeof r.bytes; i++)	/* its pages, touched now */
		r.bytes[i] = 0;

	/* ── the space full: no printing until it is emptied ── */
	filled = a35_fill_the_space();
	full = vm_allocate(mach_task_self(), &probe, vm_page_size, TRUE);
	mr = MACH_MSG_SUCCESS;
	if (sent == MACH_MSG_SUCCESS && full != KERN_SUCCESS)
		mr = mach_msg(&r.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			      sizeof r, dest, 0, MACH_PORT_NULL);
	if (full == KERN_SUCCESS)
		(void) vm_deallocate(mach_task_self(), probe, vm_page_size);
	a35_empty_the_space(filled);
	/* ── emptied ── */

	(void) mach_port_get_refs(mach_task_self(), p, MACH_PORT_RIGHT_SEND,
				  &refs);
	heard = mach_msg(&note.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			 sizeof note, notify, 200, MACH_PORT_NULL);

	if (sent != MACH_MSG_SUCCESS)
		printf("act_test: [35] the send answered 0x%x — WRONG\n",
		       (int) sent);
	else if (full == KERN_SUCCESS)
		printf("act_test: [35] %u allocations did not fill the address "
		       "space, a page was still free — WRONG\n", filled);
	else if ((mr & ~MACH_MSG_MASK) != MACH_RCV_BODY_ERROR
		 || (mr & MACH_MSG_VM_SPACE) == 0)
		printf("act_test: [35] a receive whose port array had no room "
		       "answered 0x%x, not MACH_RCV_BODY_ERROR with "
		       "MACH_MSG_VM_SPACE — WRONG (#843)\n", (int) mr);
	else if (heard == MACH_MSG_SUCCESS)
		printf("act_test: [35] P heard \"no senders\" (id %d) while this "
		       "task holds %u send right%s to it: the right delivered "
		       "was released again with the array — WRONG (#843)\n",
		       note.h.msgh_id, (unsigned) refs, refs == 1 ? "" : "s");
	else if (refs != 1)
		printf("act_test: [35] P's delivered send right is held %u "
		       "times, not once — WRONG (#843)\n", (unsigned) refs);
	else {
		printf("act_test: [35] with the address space full (%u "
		       "allocations), the port array failed alone, 0x%x, and "
		       "the right delivered before it stayed, held once and "
		       "with no \"no senders\" (#843)\n", filled, (int) mr);
		ok = 1;
	}

	(void) mach_port_destroy(mach_task_self(), dest);
	(void) mach_port_destroy(mach_task_self(), p);
	(void) mach_port_destroy(mach_task_self(), q);
	(void) mach_port_destroy(mach_task_self(), notify);
	return ok;
}

/*
 * Arm thirty-six (#810): a vm_write() of a small physical copy to an address
 * this task does not have is refused, and its copy is freed once.
 *
 * A small out-of-line region sent with MACH_MSG_PHYSICAL_COPY becomes a
 * kernel-buffer copy, and vm_write() hands it to
 * vm_map_copyout_kernel_buffer(), whose copyout fails on an address nothing
 * maps -- and which freed the buffer all the same, before the destruction of
 * the failed request freed it again.  The library's vm_write() takes a trap
 * and sends a virtual copy, so the request is built here by hand, to this
 * task's own port.
 *
 * 🔴 Sized as MIG sizes it, to the end of its last field: the server compares
 * msgh_size with 68, and sizeof the struct is 72 with its padding.  Sized 72 it
 * was refused with MIG_BAD_ARGUMENTS before vm_write() ran -- an error in the
 * reply, all this arm asked for, with the double free put back as without it:
 * the ablation was caught 0 times in 4 (02/11).  The reply must be the
 * copyout's own KERN_INVALID_ADDRESS, which only that routine gives here.
 *
 * 🔴 Then one message of A36_COPIES physical copies of that size, each with
 * bytes of its own, received and read back.  The copies' buffers come from one
 * kalloc zone through a per-processor magazine, last in first out and fifteen
 * deep, and a free into a full magazine goes to the zone's list instead
 * (kern/zalloc.c).  The element freed twice is handed out twice: by the next
 * two allocations when the magazine had room for both frees, or else once now
 * and once when the magazine is refilled from the zone, within thirty more.
 * Two copies asked for one at a time, as this arm first did, saw it in no boot
 * of 8 -- the kernel fell later, on an assertion about a port, or not at all.
 * Thirty-two in one copyin, back to back, take both.  A36_ROUNDS times.
 */
#define	A36_BYTES	64
#define	A36_COPIES	32
#define	A36_NOWHERE	((vm_address_t) 0x0000700000000000ULL)	/* no arm maps it */
#define	A36_VM_WRITE	2027	/* vm_write, mach.defs */
#define	A36_ROUNDS	16

union arm36_msg {
	struct {
		mach_msg_header_t		head;
		mach_msg_body_t			body;
		mach_msg_ool_descriptor_t	data;
		NDR_record_t			ndr;
		vm_address_t			address;
		mach_msg_type_number_t		count;
	}	in;
	struct {
		mach_msg_header_t	head;
		NDR_record_t		ndr;
		kern_return_t		ret;
	}	out;
	char	room[256];
};

struct arm36_many {
	mach_msg_header_t		head;
	mach_msg_body_t			body;
	mach_msg_ool_descriptor_t	data[A36_COPIES];
};

static unsigned char	arm36_bytes[A36_COPIES][A36_BYTES];

/* One message of A36_COPIES physical copies; copy j holds the byte 0x40 + j. */
static mach_msg_return_t
arm36_send_many(mach_port_t port)
{
	struct arm36_many	c;
	unsigned		i, j;

	for (j = 0; j < A36_COPIES; j++)
		for (i = 0; i < A36_BYTES; i++)
			arm36_bytes[j][i] = (unsigned char) (0x40 + j);
	for (i = 0; i < sizeof c; i++)
		((unsigned char *) &c)[i] = 0;
	c.head.msgh_bits = MACH_MSGH_BITS_COMPLEX
			   | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	c.head.msgh_size = sizeof c;
	c.head.msgh_remote_port = port;
	c.head.msgh_local_port = MACH_PORT_NULL;
	c.head.msgh_id = 36;
	c.body.msgh_descriptor_count = A36_COPIES;
	for (j = 0; j < A36_COPIES; j++) {
		c.data[j].address = arm36_bytes[j];
		c.data[j].size = A36_BYTES;
		c.data[j].deallocate = FALSE;
		c.data[j].copy = MACH_MSG_PHYSICAL_COPY;
		c.data[j].type = MACH_MSG_OOL_DESCRIPTOR;
	}
	return mach_msg(&c.head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof c, 0,
			MACH_PORT_NULL, 0, MACH_PORT_NULL);
}

/*
 * The copies back: how many of their bytes are not their own, and in
 * *firstp the first copy that has any.
 */
static int
arm36_receive_many(mach_port_t port, mach_msg_return_t *mrp, int *firstp)
{
	union {
		struct arm36_many	c;
		unsigned char		room[1024];
	} r;
	int		off = 0;
	unsigned	i, j;

	*firstp = -1;
	*mrp = mach_msg(&r.c.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			sizeof r, port, 1000, MACH_PORT_NULL);
	if (*mrp != MACH_MSG_SUCCESS)
		return -1;
	if (r.c.body.msgh_descriptor_count != A36_COPIES)
		return -1;
	for (j = 0; j < A36_COPIES; j++) {
		unsigned char	*got = (unsigned char *) r.c.data[j].address;
		int		bad = 0;

		for (i = 0; i < A36_BYTES; i++)
			if (got[i] != (unsigned char) (0x40 + j))
				bad++;
		if (bad != 0 && *firstp < 0)
			*firstp = (int) j;
		off += bad;
		(void) vm_deallocate(mach_task_self(), (vm_address_t) got,
				     r.c.data[j].size);
	}
	return off;
}

static int
arm_thirty_six_vm_write_small_physical_copy(void)
{
	union arm36_msg		m;
	static unsigned char	src[A36_BYTES];
	mach_port_t		reply, port;
	mach_msg_return_t	mr = MACH_MSG_SUCCESS, sent = MACH_MSG_SUCCESS;
	mach_msg_return_t	got = MACH_MSG_SUCCESS;
	kern_return_t		ret = KERN_INVALID_ADDRESS;
	int			off = 0, first = -1;
	unsigned		i, round;

	reply = arm_own_port(36);
	port = arm_own_port(36);
	if (reply == MACH_PORT_NULL || port == MACH_PORT_NULL)
		return 0;

	for (i = 0; i < A36_BYTES; i++)
		src[i] = 0x36;
	for (round = 0; round < A36_ROUNDS; round++) {
		for (i = 0; i < sizeof m; i++)
			((unsigned char *) &m)[i] = 0;
		m.in.head.msgh_bits = MACH_MSGH_BITS_COMPLEX
				      | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
						       MACH_MSG_TYPE_MAKE_SEND_ONCE);
		m.in.head.msgh_size = __builtin_offsetof(union arm36_msg, in.count)
				      + sizeof m.in.count;
		m.in.head.msgh_remote_port = mach_task_self();
		m.in.head.msgh_local_port = reply;
		m.in.head.msgh_id = A36_VM_WRITE;
		m.in.body.msgh_descriptor_count = 1;
		m.in.data.address = src;
		m.in.data.size = A36_BYTES;
		m.in.data.deallocate = FALSE;
		m.in.data.copy = MACH_MSG_PHYSICAL_COPY;
		m.in.data.type = MACH_MSG_OOL_DESCRIPTOR;
		m.in.ndr = NDR_record;
		m.in.address = A36_NOWHERE;
		m.in.count = A36_BYTES;

		mr = mach_msg(&m.in.head,
			      MACH_SEND_MSG | MACH_RCV_MSG | MACH_RCV_TIMEOUT,
			      m.in.head.msgh_size, sizeof m, reply, 5000,
			      MACH_PORT_NULL);
		ret = mr == MACH_MSG_SUCCESS ? m.out.ret : KERN_FAILURE;

		sent = arm36_send_many(port);
		off = sent == MACH_MSG_SUCCESS
		      ? arm36_receive_many(port, &got, &first) : -1;

		if (mr != MACH_MSG_SUCCESS || ret != KERN_INVALID_ADDRESS
		    || sent != MACH_MSG_SUCCESS || got != MACH_MSG_SUCCESS
		    || off != 0)
			break;
	}

	(void) mach_port_destroy(mach_task_self(), port);
	(void) mach_port_destroy(mach_task_self(), reply);

	if (mr != MACH_MSG_SUCCESS || ret != KERN_INVALID_ADDRESS) {
		printf("act_test: [36] round %u: a vm_write of a %d-byte physical "
		       "copy to 0x%lx answered 0x%x, its reply 0x%x — WRONG, "
		       "expected the copyout's KERN_INVALID_ADDRESS in the "
		       "reply (#810)\n", round + 1, A36_BYTES,
		       (unsigned long) A36_NOWHERE, (int) mr, (int) ret);
		return 0;
	}
	if (round < A36_ROUNDS) {
		printf("act_test: [36] round %u: after a refused vm_write, %d "
		       "physical copies of %d bytes in one message came back with "
		       "%d bytes not their own, the first at copy %d (send 0x%x, "
		       "receive 0x%x) — WRONG: one kalloc buffer handed out twice "
		       "(#810)\n", round + 1, A36_COPIES, A36_BYTES, off, first,
		       (int) sent, (int) got);
		return 0;
	}

	printf("act_test: [36] %u vm_writes of a %d-byte physical copy to 0x%lx "
	       "were each refused with KERN_INVALID_ADDRESS, and the %d copies of "
	       "that size sent after each in one message came back with their "
	       "own bytes (#810)\n", A36_ROUNDS, A36_BYTES,
	       (unsigned long) A36_NOWHERE, A36_COPIES);
	return 1;
}

/*
 * Arm thirty-seven (#812): a snapshot taken of memory never touched stays as
 * it was taken, however the memory is then read and written.
 *
 * vm_object_copy_delayed() write-protects only the pages resident when the
 * copy is taken.  A page first touched afterwards by a read was zero-filled
 * by vm_fault()'s fast path and mapped writable, so the write after it went
 * straight into the source -- where the copy reads it, through its shadow.
 * Here three snapshots are taken of a range never touched: vm_read(), vm_copy()
 * to a second range, and an out-of-line send to a port of this task not yet
 * received.  Then the range is read, so the fast path fills its pages, and
 * written.  All three snapshots must still read zero.
 *
 * 🔴 The range is mapped a second time first, with vm_remap() and no copy.  A
 * range just allocated has no object, and an anonymous object copies
 * symmetrically -- shadows and needs_copy, no object->copy -- so its snapshots
 * never reached the branch the fix is in: the ablation was caught 0 times in
 * 4 (02/11).  A second mapping of the same object makes it copy-delay
 * (vm_remap_extract(), as a shared fork does), and creates the object without
 * touching a page of it; each snapshot is then a delayed copy, and the read
 * after them is a zero fill in an object that has a copy.
 */
#define	A37_PAGES	2

/* How many bytes of [p, p + len) are not zero. */
static unsigned
a37_not_zero(const volatile unsigned char *p, vm_size_t len)
{
	unsigned	n = 0;
	vm_size_t	i;

	for (i = 0; i < len; i++)
		if (p[i] != 0)
			n++;
	return n;
}

static int
arm_thirty_seven_snapshot_of_untouched_memory(void)
{
	struct {
		mach_msg_header_t		head;
		mach_msg_body_t			body;
		mach_msg_ool_descriptor_t	data;
	} s;
	union {
		struct {
			mach_msg_header_t		head;
			mach_msg_body_t			body;
			mach_msg_ool_descriptor_t	data;
		} m;
		unsigned char	room[256];
	} r;
	vm_size_t		len = A37_PAGES * vm_page_size;
	vm_address_t		src = 0, dst = 0, alias = 0;
	vm_offset_t		rd = 0;
	mach_msg_type_number_t	rdcnt = 0;
	vm_prot_t		cur, max;
	kern_return_t		kr1, kr2, kr3;
	mach_msg_return_t	sent, got = MACH_RCV_TIMED_OUT;
	volatile unsigned char	*p;
	unsigned		bad_read = 0, bad_copy = 0, bad_msg = 0;
	unsigned		i, sum = 0;
	mach_port_t		port;

	port = arm_own_port(37);
	if (port == MACH_PORT_NULL)
		return 0;
	kr1 = vm_allocate(mach_task_self(), &src, len, TRUE);
	kr2 = vm_allocate(mach_task_self(), &dst, len, TRUE);
	kr3 = kr1 == KERN_SUCCESS
	      ? vm_remap(mach_task_self(), &alias, len, 0, TRUE, mach_task_self(),
			 src, FALSE, &cur, &max, VM_INHERIT_NONE)
	      : KERN_FAILURE;
	if (kr1 != KERN_SUCCESS || kr2 != KERN_SUCCESS || kr3 != KERN_SUCCESS) {
		printf("act_test: [37] vm_allocate answered %d and %d, the second "
		       "mapping %d — WRONG\n", kr1, kr2, kr3);
		(void) mach_port_destroy(mach_task_self(), port);
		return 0;
	}

	/* Three snapshots of a range nothing has touched. */
	kr1 = vm_read(mach_task_self(), src, len, &rd, &rdcnt);
	kr2 = vm_copy(mach_task_self(), src, len, dst);
	for (i = 0; i < sizeof s; i++)
		((unsigned char *) &s)[i] = 0;
	s.head.msgh_bits = MACH_MSGH_BITS_COMPLEX
			   | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	s.head.msgh_size = sizeof s;
	s.head.msgh_remote_port = port;
	s.head.msgh_local_port = MACH_PORT_NULL;
	s.head.msgh_id = 37;
	s.body.msgh_descriptor_count = 1;
	s.data.address = (void *) src;
	s.data.size = len;
	s.data.deallocate = FALSE;
	s.data.copy = MACH_MSG_VIRTUAL_COPY;
	s.data.type = MACH_MSG_OOL_DESCRIPTOR;
	sent = mach_msg(&s.head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof s, 0,
			MACH_PORT_NULL, 0, MACH_PORT_NULL);

	/* Read first, so the fast path fills the pages; then write them. */
	p = (volatile unsigned char *) src;
	for (i = 0; i < A37_PAGES; i++)
		sum += p[i * vm_page_size];
	for (i = 0; i < len; i++)
		p[i] = 0x37;

	if (kr1 == KERN_SUCCESS)
		bad_read = a37_not_zero((const volatile unsigned char *) rd,
					rdcnt);
	if (kr2 == KERN_SUCCESS)
		bad_copy = a37_not_zero((const volatile unsigned char *) dst,
					len);
	if (sent == MACH_MSG_SUCCESS) {
		got = mach_msg(&r.m.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			       sizeof r, port, 1000, MACH_PORT_NULL);
		if (got == MACH_MSG_SUCCESS) {
			bad_msg = a37_not_zero(
			    (const volatile unsigned char *) r.m.data.address,
			    r.m.data.size);
			(void) vm_deallocate(mach_task_self(),
					     (vm_address_t) r.m.data.address,
					     r.m.data.size);
		}
	}

	if (kr1 == KERN_SUCCESS)
		(void) vm_deallocate(mach_task_self(), rd, rdcnt);
	(void) vm_deallocate(mach_task_self(), src, len);
	(void) vm_deallocate(mach_task_self(), alias, len);
	(void) vm_deallocate(mach_task_self(), dst, len);
	(void) mach_port_destroy(mach_task_self(), port);

	if (kr1 != KERN_SUCCESS || kr2 != KERN_SUCCESS
	    || sent != MACH_MSG_SUCCESS || got != MACH_MSG_SUCCESS) {
		printf("act_test: [37] the snapshots could not all be taken: "
		       "vm_read %d, vm_copy %d, send 0x%x, receive 0x%x "
		       "(sum %u) — WRONG\n", kr1, kr2, (int) sent, (int) got,
		       sum);
		return 0;
	}
	if (bad_read != 0 || bad_copy != 0 || bad_msg != 0) {
		printf("act_test: [37] after a read and a write of %lu bytes "
		       "never touched before, the snapshots taken earlier changed: "
		       "%u bytes of vm_read's, %u of vm_copy's, %u of the "
		       "out-of-line message's — WRONG (#812)\n",
		       (unsigned long) len, bad_read, bad_copy, bad_msg);
		return 0;
	}

	printf("act_test: [37] snapshots of %lu bytes never touched -- vm_read, "
	       "vm_copy and an out-of-line send -- read zero after the range "
	       "was read and then written (sum %u) (#812)\n",
	       (unsigned long) len, sum);
	return 1;
}

/*
 * Arm thirty-eight (#813): task_wire() is refused without the privileged host
 * port.
 *
 * task_wire() took the task's own port alone and set its map to wire every
 * entry it would ever create, with no limit anywhere: any task could wire all
 * it allocated, and drain the free pool of every task in the system.  It now
 * takes the privileged host port, as vm_wire() does.  Here it is asked on the
 * host's ordinary port and must answer KERN_INVALID_HOST.  A kernel that does
 * not ask sets the flag, and the arm clears it again at once, so the rest of
 * this program does not run with every allocation wired.
 */
static int
arm_thirty_eight_task_wire_unprivileged(void)
{
	mach_port_t	host = mach_host_self();
	kern_return_t	kr;

	kr = task_wire(host, mach_task_self(), TRUE);
	if (kr == KERN_SUCCESS)
		(void) task_wire(host, mach_task_self(), FALSE);
	(void) mach_port_deallocate(mach_task_self(), host);

	if (kr != KERN_INVALID_HOST) {
		printf("act_test: [38] task_wire on the host's ordinary port "
		       "answered %d, not KERN_INVALID_HOST (%d) — WRONG (#813)\n",
		       kr, KERN_INVALID_HOST);
		return 0;
	}

	printf("act_test: [38] task_wire on the host's ordinary port was "
	       "refused with KERN_INVALID_HOST, %d (#813)\n", kr);
	return 1;
}

/* A thread thread_create_running() starts needs somewhere to run; it spins
 * here until the trap that created it terminates it, so no fault reaches
 * act_test's exception port (#756).  It runs in act_test's own task, where
 * this code is mapped. */
static void
arm_sixteen_spin(void)
{
	for (;;)
		__asm__ __volatile__("pause");
}

/*
 * ── [16] A creation whose name cannot be copied out leaves no name behind ──
 *
 * syscall_thread_create(), syscall_thread_create_running() and
 * syscall_task_create() each put a send right in the caller's space and then
 * copy out its name.  Told to write the name at a page the caller cannot write,
 * the copyout fails: the object is torn down, and #662 adds the step that takes
 * the right back out too, so no name is left under a name the caller was never
 * told.  The arm counts the space's names with mach_port_names() before and
 * after; equal means nothing leaked.  The #662 ablation takes the step out, and
 * the count rises.  (act_test is x86-64 only; the same four traps carry the
 * same fix on i386.)
 *
 * #756: thread_create_running() is given a real x86-64 state -- rip at the spin
 * above, on a stack of its own -- so it creates the thread and reaches its own
 * copyout.  An earlier version passed a zero-length state, which was refused
 * with KERN_INVALID_ARGUMENT before it created, leaving that trap's fix
 * exercised only by the ablation.
 */
static int
arm_sixteen_name_not_copied_out(void)
{
	mach_port_t			me = mach_task_self();
	vm_address_t			ro = 0, stack = 0;
	mach_port_array_t		names = 0;
	mach_port_type_array_t		types = 0;
	mach_msg_type_number_t		nn = 0, nt = 0;
	struct x86_64_thread_state	rstate;
	mach_msg_type_number_t		rcount = x86_64_THREAD_STATE_COUNT;
	mach_port_t			tmp = MACH_PORT_NULL;
	int				before, after;
	kern_return_t			kr, kc, kcr, ktk;

	/* A page the traps cannot write, so each copyout of the name fails. */
	kr = vm_allocate(me, &ro, vm_page_size, TRUE);
	if (kr == KERN_SUCCESS)
		kr = vm_protect(me, ro, vm_page_size, FALSE, VM_PROT_NONE);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [16] no unwritable page for the OUT name "
		       "(%d) — WRONG\n", kr);
		return 0;
	}

	kr = mach_port_names(me, &names, &nn, &types, &nt);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [16] mach_port_names before (%d) — WRONG\n",
		       kr);
		(void) vm_deallocate(me, ro, vm_page_size);
		return 0;
	}
	before = (int) nn;
	(void) vm_deallocate(me, (vm_address_t) names, nn * sizeof(*names));
	(void) vm_deallocate(me, (vm_address_t) types, nt * sizeof(*types));

	/*
	 * #756: a valid x86-64 state for thread_create_running, so it creates
	 * the thread and reaches its copyout.  A fresh stopped thread gives a
	 * valid baseline (its cs, ss and rflags); rip goes to the spin and rsp
	 * to a stack of its own, eight below a 16-byte boundary as a call leaves
	 * it.  If the state cannot be built the arm still tests the other two.
	 */
	kr = thread_create(me, &tmp);
	if (kr == KERN_SUCCESS)
		kr = thread_get_state(tmp, x86_64_THREAD_STATE,
				      (thread_state_t) &rstate, &rcount);
	if (tmp != MACH_PORT_NULL) {
		(void) thread_terminate(tmp);
		(void) mach_port_deallocate(me, tmp);
	}
	if (kr == KERN_SUCCESS)
		kr = vm_allocate(me, &stack, vm_page_size * 4, TRUE);
	if (kr == KERN_SUCCESS) {
		rstate.rip = (uint64_t) (uintptr_t) arm_sixteen_spin;
		rstate.rsp = (uint64_t) (stack + vm_page_size * 4 - 8);
	}

	kc  = syscall_thread_create(me, (mach_port_t *) ro);
	kcr = (kr == KERN_SUCCESS)
	    ? syscall_thread_create_running(me, x86_64_THREAD_STATE,
					    (natural_t *) &rstate, rcount,
					    (mach_port_t *) ro)
	    : KERN_FAILURE;
	ktk = syscall_task_create(me, 0, 0, FALSE, (mach_port_t *) ro);

	kr = mach_port_names(me, &names, &nn, &types, &nt);
	(void) vm_protect(me, ro, vm_page_size, FALSE,
			  VM_PROT_READ | VM_PROT_WRITE);
	(void) vm_deallocate(me, ro, vm_page_size);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [16] mach_port_names after (%d) — WRONG\n",
		       kr);
		return 0;
	}
	after = (int) nn;
	(void) vm_deallocate(me, (vm_address_t) names, nn * sizeof(*names));
	(void) vm_deallocate(me, (vm_address_t) types, nt * sizeof(*types));

	if (after != before) {
		printf("act_test: [16] %d name(s) left behind after creations "
		       "whose name could not be copied out (%d -> %d): a right "
		       "the caller was never told and cannot release — WRONG "
		       "(#662)\n", after - before, before, after);
		return 0;
	}

	printf("act_test: [16] a creation whose name could not be copied out "
	       "left no name behind: %d names before and after, trap codes "
	       "0x%x/0x%x/0x%x (#662)\n", before, kc, kcr, ktk);
	return 1;
}

/*
 * ── [15] A kernel-loaded task is refused without the privileged host port ──
 *
 * syscall_kernel_task_create() (the trap) and kernel_task_create() (its RPC
 * twin, the MIG stub libmach links) each make a task in the kernel's own
 * address space.  #752 gave them an authority: the privileged host port.
 * Called without it -- MACH_PORT_NULL here -- both must refuse, and refuse the
 * same way (#548), or a task holding only its own task port could put a thread
 * into the kernel's address space.  This test is x86-64 only, where the answer
 * is KERN_NOT_SUPPORTED, the machine having no kernel-loaded task at all (#453);
 * the same shared check answers KERN_NO_ACCESS on i386, which the i386 suite
 * exercises by booting (bootstrap still loads a collocated server with the
 * privileged port it holds).
 *
 * Were the refusal taken out (the ablation), the task would be created and the
 * thread this arm then makes in it would reach pcb_user_to_kernel(), which on
 * x86-64 stops the machine (#453).  On the fixed tree that line is never run.
 */
static int
arm_fifteen_kernel_task_needs_privilege(void)
{
	mach_port_t		me = mach_task_self();
	mach_port_t		kt_trap = MACH_PORT_NULL, kt_rpc = MACH_PORT_NULL;
	const kern_return_t	want = KERN_NOT_SUPPORTED;	/* x86-64 (#453) */
	kern_return_t		kr_trap, kr_rpc;

	kr_trap = syscall_kernel_task_create(me, MACH_PORT_NULL, 0, 0, &kt_trap);
	kr_rpc = kernel_task_create(me, MACH_PORT_NULL, 0, 0, &kt_rpc);

	if (kr_trap == KERN_SUCCESS || kr_rpc == KERN_SUCCESS) {
		mach_port_t	kt = (kr_trap == KERN_SUCCESS) ? kt_trap : kt_rpc;
		mach_port_t	th = MACH_PORT_NULL;

		printf("act_test: [15] a kernel-loaded task was created without "
		       "the privileged host port — making a thread in it to "
		       "show the cost (#752/#453)\n");
		(void) thread_create(kt, &th);
		printf("act_test: [15] a kernel-loaded task was created without "
		       "privilege, and a thread in it did not stop the machine "
		       "— WRONG\n");
		if (th != MACH_PORT_NULL) {
			(void) thread_terminate(th);
			(void) mach_port_deallocate(me, th);
		}
		(void) task_terminate(kt);
		return 0;
	}

	if (kr_trap != kr_rpc) {
		printf("act_test: [15] the trap answered 0x%x and the RPC 0x%x "
		       "— the two paths must agree (#548/#752) — WRONG\n",
		       kr_trap, kr_rpc);
		return 0;
	}

	if (kr_trap != want) {
		printf("act_test: [15] refused with 0x%x, wanted 0x%x "
		       "(KERN_NOT_SUPPORTED, x86-64 has no kernel-loaded task) "
		       "— WRONG\n", kr_trap, want);
		return 0;
	}

	printf("act_test: [15] a kernel-loaded task is refused without the "
	       "privileged host port: the trap and the RPC both answered 0x%x "
	       "(#752)\n", kr_trap);
	return 1;
}

/* ----------------------------------------------------------------
 * Arms seventeen and eighteen: a return to ring 3 that IRETQ would refuse
 * (#780)
 * ----------------------------------------------------------------
 *
 * Arm six asks the syscall return, which since #518 refuses a non-canonical
 * rip before SYSRET can fault on it.  The other two ways back to ring 3 are
 * IRETQs -- the tail every thread return shares, and trap_common's -- and
 * nothing asked them the same question.  An IRETQ to a non-canonical rip is a
 * general protection fault at CPL 0, and since #770 it is taken with %rsp in
 * the thread's pcb: the fault's frame and the C that reports it went over the
 * pcb and the zone around it.  Before #770 it halted the machine.
 *
 * Arm seventeen asks the thread-return tail.  A thread created and given a
 * non-canonical rip before it ever ran starts through thread_bootstrap_return,
 * and its handler then answers "resume" without moving rip, a hundred times:
 * the thread comes back through thread_exception_return a hundred times.  The
 * tails start again at the top of the kernel stack, so a round costs nothing;
 * without that, each round would sit below the last until the stack ran out.
 *
 * Arm eighteen asks trap_common's: a thread spinning in ring 3 is suspended --
 * inside an interrupt, the only way into the kernel it has -- given a
 * non-canonical rip, and resumed.
 *
 * 🔴 UNDER KVM, NOT TCG.  TCG does not raise the #GP an IRETQ owes a
 * non-canonical target (#518, #516): there the iretq goes through and the
 * fetch after it faults in ring 3, so both arms pass under TCG with or without
 * the fix.  What makes them mean something is KVM, and an ablation per arm
 * that turns it red there.
 *
 * 🔥 Each announces what it is about to do before doing it, for arm six's
 * reason: on a kernel without the fix the machine stops at the iretq, and the
 * last line printed has to say what was being done.
 */
/* Arms seventeen and eighteen's state: declared with arm six's, above. */

/*
 * One exception message, waited for four seconds at most and answered -- with
 * a bound for arm six's reason: a kernel that delivers nothing must not take
 * the summary of the arms before it down with it.
 */
static kern_return_t
arm_780_serve_one(void)
{
	union {
		mach_msg_header_t	h;
		char			room[4096];
	} req, rep;
	kern_return_t	kr;

	kr = mach_msg(&req.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof req,
		      exc_port, 4000, MACH_PORT_NULL);
	if (kr != KERN_SUCCESS)
		return kr;
	(void) exc_server(&req.h, &rep.h);
	if (rep.h.msgh_remote_port != MACH_PORT_NULL)
		(void) mach_msg(&rep.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
				rep.h.msgh_size, 0, MACH_PORT_NULL, 1000,
				MACH_PORT_NULL);
	return KERN_SUCCESS;
}

static int
arm_780_watch_bad_instruction(const char *tag)
{
	kern_return_t	kr;

	if (exc_port == MACH_PORT_NULL) {
		printf("act_test: %s no exception port — arm one did not get "
		       "far enough for this arm to mean anything — WRONG\n", tag);
		return 0;
	}
	kr = task_set_exception_ports(mach_task_self(),
				      EXC_MASK_BAD_INSTRUCTION, exc_port,
				      EXCEPTION_DEFAULT, THREAD_STATE_NONE);
	if (kr != KERN_SUCCESS) {
		printf("act_test: %s task_set_exception_ports failed (%d)"
		       " — WRONG\n", tag, kr);
		return 0;
	}
	return 1;
}

static int
arm_seventeen_start_at_a_noncanonical_rip(void)
{
	struct x86_64_thread_state	state;
	thread_act_t			t;
	vm_address_t			stack = 0;
	kern_return_t			kr;
	unsigned int			i;
	int				round;
	char				vendor[13];

	if (!arm_780_watch_bad_instruction("[17]"))
		return 0;

	kr = vm_allocate(mach_task_self(), &stack, vm_page_size, TRUE);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [17] no page for a stack (%d) — WRONG\n", kr);
		return 0;
	}
	kr = thread_create(mach_task_self(), &t);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [17] thread_create answered %d — WRONG\n", kr);
		return 0;
	}

	/*
	 * A whole state, written before the thread has ever run: zero, a stack
	 * of its own, and the address.  cs and ss are not ours to choose -- the
	 * kernel imposes them -- and the flags keep only what ring 3 may set.
	 */
	for (i = 0; i < sizeof state; i++)
		((volatile char *) &state)[i] = 0;
	state.rip = NONCANONICAL;
	state.rsp = stack + vm_page_size - 16;
	state.rflags = 0x202;
	kr = thread_set_state(t, x86_64_THREAD_STATE, (thread_state_t) &state,
			      x86_64_THREAD_STATE_COUNT);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [17] thread_set_state refused a non-canonical "
		       "rip with %d — not this kernel's answer (#780), and the "
		       "return path is left unasked — WRONG\n", kr);
		(void) thread_terminate(t);
		return 0;
	}

	arm_six_armed = 0;		/* arm six's victim is gone */
	arm_seventeen_thread = t;
	arm_seventeen_armed = 1;
	printf("act_test: [17] starting a thread at rip 0x%llx and answering "
	       "\"resume\" %d times; if the machine stops here, that is the "
	       "answer (#780)\n", NONCANONICAL, ARM17_ROUNDS);

	kr = thread_resume(t);
	if (kr != KERN_SUCCESS) {
		arm_seventeen_armed = 0;
		printf("act_test: [17] thread_resume answered %d — WRONG\n", kr);
		(void) thread_terminate(t);
		return 0;
	}

	for (round = 0; round < ARM17_ROUNDS; round++)
		if ((kr = arm_780_serve_one()) != KERN_SUCCESS)
			break;
	arm_seventeen_armed = 0;
	(void) mach_port_deallocate(mach_task_self(), t);
	(void) vm_deallocate(mach_task_self(), stack, vm_page_size);

	if (arm_seventeen_seen != ARM17_ROUNDS) {
		printf("act_test: [17] %d of %d exceptions arrived (the last wait "
		       "answered %d) — WRONG\n", arm_seventeen_seen, ARM17_ROUNDS,
		       kr);
		return 0;
	}
	if (arm_seventeen_others != 0) {
		printf("act_test: [17] %d exceptions named another thread "
		       "— WRONG\n", arm_seventeen_others);
		return 0;
	}
	if (arm_seventeen_exception != EXC_BAD_INSTRUCTION) {
		printf("act_test: [17] the exception was %d, not "
		       "EXC_BAD_INSTRUCTION (%d) — WRONG\n",
		       arm_seventeen_exception, EXC_BAD_INSTRUCTION);
		return 0;
	}

	cpu_vendor(vendor);
	printf("act_test: [17] a thread started at 0x%llx raised "
	       "EXC_BAD_INSTRUCTION code %d against itself %d times, was told to "
	       "resume each time, and the machine is still running to say so "
	       "— cpu %s (#780)\n", NONCANONICAL, arm_seventeen_code,
	       arm_seventeen_seen, vendor);
	return 1;
}

static void *
the_thread_stopped_in_a_trap(void *arg)
{
	(void) arg;

	arm_eighteen_thread = mach_thread_self();
	while (!arm_eighteen_stop)
		;
	/*
	 * Not reached when the arm works: the thread is terminated at the
	 * exception its poisoned frame raises.  Reached, it ran on past it.
	 */
	arm_eighteen_named_the_victim = -1;
	return NULL;
}

static int
arm_eighteen_stopped_in_a_trap(void)
{
	pthread_t			spinner;
	struct x86_64_thread_state	state;
	mach_msg_type_number_t		count = x86_64_THREAD_STATE_COUNT;
	kern_return_t			kr;
	int				i;
	unsigned long long		stopped_at;
	char				vendor[13];

	if (!arm_780_watch_bad_instruction("[18]"))
		return 0;

	if (pthread_create(&spinner, NULL, the_thread_stopped_in_a_trap,
			   NULL) != 0) {
		printf("act_test: [18] pthread_create failed — WRONG\n");
		return 0;
	}
	for (i = 0; i < PATIENCE && arm_eighteen_thread == MACH_PORT_NULL; i++)
		(void) nap(100);
	if (arm_eighteen_thread == MACH_PORT_NULL) {
		printf("act_test: [18] the spinner never published its port "
		       "— WRONG\n");
		return 0;
	}
	(void) nap(200);

	/*
	 * ⚠️ Stopped INSIDE ITS LOOP, and asked rather than assumed.  The
	 * spinner made one system call before it began to spin, and a thread
	 * stopped inside one leaves through SYSRET, whose check is #518's: the
	 * arm would pass, and the ablation of trap_common's check would stay
	 * green without saying why.  A rip in this function's own code means
	 * the thread was stopped where only an interrupt could have stopped it;
	 * anywhere else, it is let go and stopped again.
	 */
	for (i = 0; i < 10; i++) {
		kr = thread_suspend(arm_eighteen_thread);
		if (kr != KERN_SUCCESS) {
			printf("act_test: [18] thread_suspend answered %d "
			       "— WRONG\n", kr);
			return 0;
		}
		count = x86_64_THREAD_STATE_COUNT;
		kr = thread_get_state(arm_eighteen_thread, x86_64_THREAD_STATE,
				      (thread_state_t) &state, &count);
		if (kr == KERN_SUCCESS
		    && state.rip >= (uintptr_t) the_thread_stopped_in_a_trap
		    && state.rip < (uintptr_t) the_thread_stopped_in_a_trap
				   + 0x100)
			break;
		(void) thread_resume(arm_eighteen_thread);
		(void) nap(50);
	}
	if (kr != KERN_SUCCESS || state.cs != 0x2bULL || state.ss != 0x23ULL
	    || i == 10) {
		printf("act_test: [18] thread_get_state answered %d, cs 0x%llx "
		       "ss 0x%llx rip 0x%llx after %d tries — not a ring-3 frame "
		       "inside the spinner's loop to poison — WRONG\n", kr,
		       (unsigned long long) state.cs,
		       (unsigned long long) state.ss,
		       (unsigned long long) state.rip, i);
		arm_eighteen_stop = 1;
		(void) thread_resume(arm_eighteen_thread);
		return 0;
	}
	stopped_at = state.rip;

	state.rip = NONCANONICAL;
	kr = thread_set_state(arm_eighteen_thread, x86_64_THREAD_STATE,
			      (thread_state_t) &state, count);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [18] thread_set_state refused a non-canonical "
		       "rip with %d — WRONG\n", kr);
		arm_eighteen_stop = 1;
		(void) thread_resume(arm_eighteen_thread);
		return 0;
	}

	arm_six_armed = 0;
	arm_eighteen_armed = 1;
	printf("act_test: [18] resuming a thread stopped inside a trap, its "
	       "frame now saying rip 0x%llx; if the machine stops here, that is "
	       "the answer (#780)\n", NONCANONICAL);
	(void) thread_resume(arm_eighteen_thread);

	kr = arm_780_serve_one();
	arm_eighteen_armed = 0;
	arm_eighteen_stop = 1;		/* for a thread that ran on */
	(void) mach_port_deallocate(mach_task_self(), arm_eighteen_thread);

	if (kr != KERN_SUCCESS) {
		printf("act_test: [18] no exception arrived within four seconds "
		       "(%d) — WRONG\n", kr);
		return 0;
	}
	if (arm_eighteen_named_the_victim != 1) {
		printf("act_test: [18] the exception %s — WRONG\n",
		       arm_eighteen_named_the_victim == -1
		       ? "came after the thread ran on past its trap"
		       : "named a thread that is not the one that was poisoned");
		return 0;
	}
	if (arm_eighteen_exception != EXC_BAD_INSTRUCTION) {
		printf("act_test: [18] the exception was %d, not "
		       "EXC_BAD_INSTRUCTION (%d) — WRONG\n",
		       arm_eighteen_exception, EXC_BAD_INSTRUCTION);
		return 0;
	}

	cpu_vendor(vendor);
	printf("act_test: [18] a thread stopped inside a trap at 0x%llx, in its "
	       "loop, and told to resume at 0x%llx raised EXC_BAD_INSTRUCTION "
	       "code %d against itself, and the machine is still running to say "
	       "so — cpu %s (#780)\n", stopped_at, NONCANONICAL,
	       arm_eighteen_code, vendor);
	return 1;
}

/* ----------------------------------------------------------------
 * Arm nineteen: the last page of the user half is nobody's (#780)
 * ----------------------------------------------------------------
 *
 * Code in the last page of the canonical half leaves it without a branch: an
 * instruction ending on its last byte, or a SYSCALL in its last two, gives the
 * kernel a user rip of 2^47 to return to.  So no task may map that page, and
 * the stacks that start at the top start a page lower.  Asked of this task's
 * own map: nothing there or above it, and an allocation there refused.  On a
 * kernel that still gave the page out, the region found is this program's own
 * stack.
 */
#define	LAST_USER_PAGE	((vm_address_t) 0x00007ffffffff000ULL)

static int
arm_nineteen_last_page_unmapped(void)
{
	vm_address_t			a = LAST_USER_PAGE, fixed = LAST_USER_PAGE;
	vm_size_t			sz = 0;
	vm_region_basic_info_data_t	bi;
	mach_msg_type_number_t		c = VM_REGION_BASIC_INFO_COUNT;
	mach_port_t			obj = MACH_PORT_NULL;
	kern_return_t			kr_region, kr_alloc;

	kr_region = vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO,
			      (vm_region_info_t) &bi, &c, &obj);
	kr_alloc = vm_allocate(mach_task_self(), &fixed, vm_page_size, FALSE);
	if (kr_alloc == KERN_SUCCESS)
		(void) vm_deallocate(mach_task_self(), fixed, vm_page_size);

	if (kr_region == KERN_SUCCESS) {
		printf("act_test: [19] a region at 0x%llx-0x%llx covers the last "
		       "page of the user half — WRONG (#780)\n",
		       (unsigned long long) a, (unsigned long long) (a + sz));
		return 0;
	}
	if (kr_alloc == KERN_SUCCESS) {
		printf("act_test: [19] vm_allocate gave this task the page at "
		       "0x%llx — WRONG (#780)\n",
		       (unsigned long long) LAST_USER_PAGE);
		return 0;
	}
	printf("act_test: [19] the last page of the user half, 0x%llx: "
	       "vm_region found nothing there or above (%d), and vm_allocate "
	       "there answered %d — no task can map it (#780)\n",
	       (unsigned long long) LAST_USER_PAGE, kr_region, kr_alloc);
	return 1;
}

/*
 * Arm thirty-nine (#777): a hand-off whose bet is lost is undone where it shows.
 *
 * A send with MACH_SEND_HANDOFF says "I block next", and queues the thread it
 * wakes on the sender's own processor, whose queue no other processor takes
 * from.  Two ways of not blocking next, each made certain here rather than
 * raced for:
 *
 *  - another call that is not a receive: a plain send to a port of our own;
 *  - a receive that finds a message: one put on that port before the send.
 *
 * Either way the woken thread must leave our queue and run.  The verdict is
 * the kernel's count of bets undone, and that the thread ran at all.  How soon
 * it ran is printed and not judged: without the undo it runs at the next tick
 * anyway, and under TCG on a busy host a good kernel can take as long, so a
 * limit here would fail good kernels in campaigns.
 *
 * The third thing is the window itself, which a boot does not hit on its own:
 * a server's reply and receive look at an empty queue, and a request arrives
 * before the receive.  That part counts and asks nothing, because it depends on
 * a race: ablations/777-widen-queue-fills-in-the-window.patch holds it open.
 *
 * Not asked where the hand-off is off: i386 without -X or -P.  Nor where -N
 * has the wakeup: a direct switch runs the receiver from inside the send, with
 * no run queue and no bet.  A thread the hand-off queued sits on this thread's
 * processor until this thread leaves it, so one that has run by the time the
 * send comes back was not bet on.
 */
#define	A39_WAKE	3900
#define	A39_RPC		3901
#define	A39_REPLY	3902	/* the widen holds this reply's window */
#define	A39_NOISE	3903
#define	A39_STOP	3904
#define	A39_TRIES	20
#define	A39_RPCS	300

static mach_port_t	arm_thirty_nine_port;
static volatile unsigned arm_thirty_nine_got;
static volatile mach_port_t arm_thirty_nine_thread;
static volatile int	arm_thirty_nine_stop;

/*
 * A thread the kernel has in a wait.  A flag set before mach_msg() says only
 * that it is on its way, and thread_switch() with nothing else to run comes
 * back at once, so a message sent on that word reaches a queue with no receiver
 * on it, and no hand-off is bet at all.  arm 39 and arm 40.
 */
static int
arm_thread_waiting(volatile mach_port_t *thread)
{
	struct thread_basic_info	info;
	mach_msg_type_number_t		n;
	unsigned			i;

	for (i = 0; i < 4000; i++) {
		n = THREAD_BASIC_INFO_COUNT;
		if (*thread != MACH_PORT_NULL &&
		    thread_info(*thread, THREAD_BASIC_INFO, (thread_info_t) &info,
				&n) == KERN_SUCCESS &&
		    info.run_state == TH_STATE_WAITING)
			return 1;
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 1);
	}
	return 0;
}

static unsigned long long
arm_thirty_nine_tsc(void)
{
	return __builtin_ia32_rdtsc();
}

static int
arm_thirty_nine_counts(mach_port_t host, unsigned *reach, unsigned *rcv,
		       unsigned *ret)
{
	host_rpc_handoff_info_data_t	hi;
	mach_msg_type_number_t		c = HOST_RPC_HANDOFF_INFO_COUNT;

	if (host_info(host, HOST_RPC_HANDOFF_INFO, (host_info_t) &hi, &c)
	    != KERN_SUCCESS || c != HOST_RPC_HANDOFF_INFO_COUNT)
		return 0;
	*reach = hi.reach;
	*rcv = hi.undone_receive;
	*ret = hi.undone_return;
	return 1;
}

static void *
arm_thirty_nine_receiver(void *arg)
{
	union {
		mach_msg_header_t	h;
		char			bytes[128];
	} m;

	(void) arg;
	arm_thirty_nine_thread = mach_thread_self();
	for (;;) {
		if (mach_msg(&m.h, MACH_RCV_MSG, 0, sizeof m,
			     arm_thirty_nine_port, MACH_MSG_TIMEOUT_NONE,
			     MACH_PORT_NULL) != MACH_MSG_SUCCESS ||
		    m.h.msgh_id != A39_WAKE)
			return NULL;
		arm_thirty_nine_got++;
	}
}

/* A header-only message to `port', sent without waiting for room. */
static mach_msg_return_t
arm_thirty_nine_send(mach_port_t port, mach_msg_option_t option, int id)
{
	mach_msg_header_t	h;

	h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	h.msgh_size = sizeof h;
	h.msgh_remote_port = port;
	h.msgh_local_port = MACH_PORT_NULL;
	h.msgh_reserved = 0;
	h.msgh_id = id;
	return mach_msg(&h, MACH_SEND_MSG | MACH_SEND_TIMEOUT | option,
			sizeof h, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
}

/* The last message a server sees: the queue may be full, so until it is taken. */
static void
arm_thirty_nine_stop_it(void)
{
	unsigned	i;

	for (i = 0; i < 2000; i++) {
		if (arm_thirty_nine_send(arm_thirty_nine_port, 0, A39_STOP)
		    == MACH_MSG_SUCCESS)
			return;
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 1);
	}
}

static mach_msg_return_t
arm_thirty_nine_drain(mach_port_t port)
{
	union {
		mach_msg_header_t	h;
		char			bytes[128];
	} m;

	return mach_msg(&m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof m,
			port, 0, MACH_PORT_NULL);
}

/*
 * One way of breaking the promise: `at_receive' or the plain send.  Answers
 * whether the kernel counted a bet undone and the thread ran.
 */
static int
arm_thirty_nine_break(mach_port_t host, mach_port_t own, int at_receive,
		      unsigned cpus)
{
	const char	*where = at_receive ? "a receive that found a message" :
					      "a call that was not a receive";
	unsigned	try, i, before, r0, t0, r1, t1, reach, ran_first = 0;
	unsigned long long start, took = 0;

	for (try = 0; try < A39_TRIES; try++) {
		/*
		 * One call more each try, in turn, so that no try lines up with
		 * something periodic on the way: 622-widen-entry.patch yields
		 * at every fourth mach_msg(), and a try of a multiple of four
		 * gave this thread's processor to the wakee at the same call
		 * twenty times of twenty, a bet won and nothing to undo.
		 */
		for (i = 0; i < try % 4; i++)
			(void) arm_thirty_nine_counts(host, &reach, &r0, &t0);
		if (!arm_thread_waiting(&arm_thirty_nine_thread)) {
			printf("act_test: [39] the receiver never came to wait "
			       "— WRONG\n");
			return 0;
		}

		if (at_receive &&
		    arm_thirty_nine_send(own, 0, A39_NOISE) != MACH_MSG_SUCCESS) {
			printf("act_test: [39] the message for the receive "
			       "was refused — WRONG\n");
			return 0;
		}
		if (!arm_thirty_nine_counts(host, &reach, &r0, &t0))
			return 0;
		before = arm_thirty_nine_got;
		start = arm_thirty_nine_tsc();

		/* The bet, then the promise broken with no call between. */
		(void) arm_thirty_nine_send(arm_thirty_nine_port,
					    MACH_SEND_HANDOFF, A39_WAKE);
		if (arm_thirty_nine_got != before)
			ran_first++;
		if (at_receive)
			(void) arm_thirty_nine_drain(own);
		else
			(void) arm_thirty_nine_send(own, 0, A39_NOISE);

		if (cpus > 1)
			while (arm_thirty_nine_got == before &&
			       arm_thirty_nine_tsc() - start < 200000000ULL)
				;
		/* On one processor it can run only now. */
		for (i = 0; i < 2000 && arm_thirty_nine_got == before; i++)
			(void) thread_switch(MACH_PORT_NULL,
					     SWITCH_OPTION_DEPRESS, 1);
		took = arm_thirty_nine_tsc() - start;
		if (arm_thirty_nine_got == before) {
			printf("act_test: [39] the thread woken before %s "
			       "never ran — WRONG\n", where);
			return 0;
		}
		if (!at_receive)
			(void) arm_thirty_nine_drain(own);
		if (!arm_thirty_nine_counts(host, &reach, &r1, &t1))
			return 0;
		if ((at_receive ? r1 - r0 : t1 - t0) != 0) {
			printf("act_test: [39] the bet lost at %s was undone, "
			       "try %u; the thread ran %llu TSC ticks after the "
			       "send\n", where, try + 1, took);
			return 1;
		}
	}
	if (ran_first != 0) {
		printf("act_test: [39] %u of %u sends before %s were taken by a "
		       "direct switch, with no bet to lose — NOT ASKED\n",
		       ran_first, A39_TRIES, where);
		return -1;
	}
	printf("act_test: [39] %u bets lost at %s, none undone — WRONG\n",
	       A39_TRIES, where);
	return 0;
}

/* The server of the window: a reply and a receive in one call, or a receive. */
static void *
arm_thirty_nine_server(void *arg)
{
	union {
		mach_msg_header_t	h;
		char			bytes[128];
	} m;
	mach_msg_return_t	mr;

	(void) arg;
	mr = mach_msg(&m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof m,
		      arm_thirty_nine_port, 10000, MACH_PORT_NULL);
	while (mr == MACH_MSG_SUCCESS && m.h.msgh_id != A39_STOP) {
		if (m.h.msgh_id == A39_RPC) {
			m.h.msgh_bits = MACH_MSGH_BITS(
			    MACH_MSGH_BITS_REMOTE(m.h.msgh_bits), 0);
			m.h.msgh_size = sizeof m.h;
			m.h.msgh_local_port = MACH_PORT_NULL;
			m.h.msgh_id = A39_REPLY;
			mr = mach_msg(&m.h, MACH_SEND_MSG | MACH_RCV_MSG |
				      MACH_RCV_TIMEOUT, sizeof m.h, sizeof m,
				      arm_thirty_nine_port, 10000,
				      MACH_PORT_NULL);
		} else
			mr = mach_msg(&m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
				      sizeof m, arm_thirty_nine_port, 10000,
				      MACH_PORT_NULL);
	}
	return NULL;
}

/* A request now and then, for one to land in the window. */
static void *
arm_thirty_nine_noise(void *arg)
{
	unsigned long long	t;

	(void) arg;
	while (!arm_thirty_nine_stop) {
		(void) arm_thirty_nine_send(arm_thirty_nine_port, 0, A39_NOISE);
		t = arm_thirty_nine_tsc();
		while (arm_thirty_nine_tsc() - t < 100000ULL &&
		       !arm_thirty_nine_stop)
			;
	}
	return NULL;
}

/* Says how often the window closed on a request.  Asks nothing of the count. */
static int
arm_thirty_nine_window(mach_port_t host)
{
	union {
		mach_msg_header_t	h;
		char			bytes[128];
	} m;
	pthread_t		server, noise;
	mach_port_t		reply;
	mach_msg_return_t	mr = MACH_MSG_SUCCESS;
	unsigned		i, reach, r0, t0, r1, t1;

	reply = arm_own_port(39);
	if (reply == MACH_PORT_NULL)
		return 0;
	if (!arm_thirty_nine_counts(host, &reach, &r0, &t0) ||
	    pthread_create(&server, NULL, arm_thirty_nine_server, NULL) != 0) {
		(void) mach_port_destroy(mach_task_self(), reply);
		return 0;
	}
	arm_thirty_nine_stop = 0;
	if (pthread_create(&noise, NULL, arm_thirty_nine_noise, NULL) != 0) {
		arm_thirty_nine_stop = 1;
		arm_thirty_nine_stop_it();
		(void) pthread_join(server, NULL);
		(void) mach_port_destroy(mach_task_self(), reply);
		return 0;
	}
	for (i = 0; i < A39_RPCS && mr == MACH_MSG_SUCCESS; i++) {
		m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
					       MACH_MSG_TYPE_MAKE_SEND_ONCE);
		m.h.msgh_size = sizeof m.h;
		m.h.msgh_remote_port = arm_thirty_nine_port;
		m.h.msgh_local_port = reply;
		m.h.msgh_reserved = 0;
		m.h.msgh_id = A39_RPC;
		mr = mach_msg(&m.h, MACH_SEND_MSG | MACH_RCV_MSG |
			      MACH_RCV_TIMEOUT, sizeof m.h, sizeof m, reply,
			      10000, MACH_PORT_NULL);
	}
	arm_thirty_nine_stop = 1;
	(void) pthread_join(noise, NULL);
	arm_thirty_nine_stop_it();
	(void) pthread_join(server, NULL);
	(void) mach_port_destroy(mach_task_self(), reply);
	if (mr != MACH_MSG_SUCCESS) {
		printf("act_test: [39] RPC %u of the window answered 0x%x — "
		       "WRONG\n", i, (int) mr);
		return 0;
	}
	if (!arm_thirty_nine_counts(host, &reach, &r1, &t1))
		return 0;
	printf("act_test: [39] the queue filled in the window %u time(s) in %u "
	       "RPCs\n", r1 - r0, A39_RPCS);
	return 1;
}

static int
arm_thirty_nine_asked(mach_port_t host)
{
	mach_port_t	own;
	pthread_t	receiver;
	unsigned	reach, r0, t0, cpus = processors();
	int		ok = 0, not_asked = 0, a, b;

	if (!arm_thirty_nine_counts(host, &reach, &r0, &t0)) {
		printf("act_test: [39] host_info refused the hand-off's counts "
		       "— WRONG\n");
		return 0;
	}
	if (reach == 0) {
		printf("act_test: [39] no hand-off on this kernel — NOT ASKED\n");
		return 1;
	}
	arm_thirty_nine_port = arm_own_port(39);
	if (arm_thirty_nine_port == MACH_PORT_NULL)
		return 0;
	own = arm_own_port(39);
	if (own == MACH_PORT_NULL) {
		(void) mach_port_destroy(mach_task_self(),
					 arm_thirty_nine_port);
		return 0;
	}
	arm_thirty_nine_thread = MACH_PORT_NULL;
	if (pthread_create(&receiver, NULL, arm_thirty_nine_receiver, NULL)
	    != 0) {
		printf("act_test: [39] pthread_create failed — WRONG\n");
	} else {
		a = arm_thirty_nine_break(host, own, 0, cpus);
		b = arm_thirty_nine_break(host, own, 1, cpus);
		ok = a != 0 && b != 0;
		not_asked = a < 0 || b < 0;
		arm_thirty_nine_stop_it();
		(void) pthread_join(receiver, NULL);
		(void) mach_port_deallocate(mach_task_self(),
					    arm_thirty_nine_thread);
		if (cpus > 1 && arm_thirty_nine_window(host) == 0)
			ok = 0;
	}
	(void) mach_port_destroy(mach_task_self(), own);
	(void) mach_port_destroy(mach_task_self(), arm_thirty_nine_port);
	if (ok && !not_asked)
		printf("act_test: [39] a lost bet is undone, at a receive and "
		       "at a return — OK\n");
	return ok;
}

static int
arm_thirty_nine_lost_bet_undone(void)
{
	mach_port_t	host = mach_host_self();
	int		ok = arm_thirty_nine_asked(host);

	(void) mach_port_deallocate(mach_task_self(), host);
	return ok;
}

/*
 * Arm forty (#777): a hand-off that no call comes back to.  Arm thirty-nine's
 * undo sits in mach_msg(), so two ways past it are asked here:
 *
 *  - no call at all: a send alone with MACH_SEND_HANDOFF, then user code.  Only
 *    a tick looks.  This thread computes until the woken thread has run, and
 *    the verdict is the kernel's count of bets undone at a tick;
 *  - a combined call, whose hot path switches to its own receiver past
 *    thread_invoke(), and must settle the hand-off before it does.  The server
 *    reads the count while it serves, with the caller not back yet, so that
 *    the undo at the caller's return cannot stand in for it.
 *
 * Neither is timed, so neither depends on the host.  Not asked where the
 * hand-off is off, or where -N's direct switch takes the wakeup (arm 39).
 */
#define	A40_WAKE	4000
#define	A40_RPC		4001
#define	A40_REPLY	4002
#define	A40_TRIES	20
#define	A40_WARM	20
#define	A40_SPIN	1000000000ULL	/* TSC ticks: tens of kernel ticks */

static mach_port_t	arm_forty_port;		/* the woken thread's */
static mach_port_t	arm_forty_service;	/* the server's */
static mach_port_t	arm_forty_host;
static volatile unsigned arm_forty_got;
static volatile mach_port_t arm_forty_thread;
static volatile unsigned arm_forty_seen;	/* the server read this */

static int
arm_forty_counts(mach_port_t host, unsigned *at_return, unsigned *at_tick)
{
	host_rpc_handoff_info_data_t	hi;
	mach_msg_type_number_t		c = HOST_RPC_HANDOFF_INFO_COUNT;

	if (host_info(host, HOST_RPC_HANDOFF_INFO, (host_info_t) &hi, &c)
	    != KERN_SUCCESS || c != HOST_RPC_HANDOFF_INFO_COUNT)
		return 0;
	*at_return = hi.undone_return;
	*at_tick = hi.undone_tick;
	return 1;
}

static void *
arm_forty_receiver(void *arg)
{
	union {
		mach_msg_header_t	h;
		char			bytes[128];
	} m;

	(void) arg;
	arm_forty_thread = mach_thread_self();
	for (;;) {
		if (mach_msg(&m.h, MACH_RCV_MSG, 0, sizeof m, arm_forty_port,
			     MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL)
		    != MACH_MSG_SUCCESS || m.h.msgh_id != A40_WAKE)
			return NULL;
		arm_forty_got++;
	}
}

static void *
arm_forty_server(void *arg)
{
	union {
		mach_msg_header_t	h;
		char			bytes[128];
	} m;
	mach_msg_option_t	opt = MACH_RCV_MSG;
	mach_msg_size_t		size = 0;
	unsigned		ret, tick;

	(void) arg;
	for (;;) {
		if (mach_msg(&m.h, opt, size, sizeof m, arm_forty_service,
			     MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL)
		    != MACH_MSG_SUCCESS || m.h.msgh_id != A40_RPC)
			return NULL;
		if (arm_forty_counts(arm_forty_host, &ret, &tick))
			arm_forty_seen = ret + tick;
		m.h.msgh_bits = MACH_MSGH_BITS(
		    MACH_MSGH_BITS_REMOTE(m.h.msgh_bits), 0);
		m.h.msgh_size = sizeof m.h;
		m.h.msgh_local_port = MACH_PORT_NULL;
		m.h.msgh_id = A40_REPLY;
		size = sizeof m.h;
		opt = MACH_SEND_MSG | MACH_RCV_MSG;
	}
}

/* Exactly the options the hot path takes: no timeout, nothing else. */
static mach_msg_return_t
arm_forty_rpc(mach_port_t reply)
{
	union {
		mach_msg_header_t	h;
		char			bytes[128];
	} m;

	m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
				       MACH_MSG_TYPE_MAKE_SEND_ONCE);
	m.h.msgh_size = sizeof m.h;
	m.h.msgh_remote_port = arm_forty_service;
	m.h.msgh_local_port = reply;
	m.h.msgh_reserved = 0;
	m.h.msgh_id = A40_RPC;
	return mach_msg(&m.h, MACH_SEND_MSG | MACH_RCV_MSG, sizeof m.h, sizeof m,
			reply, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

static int
arm_forty_ran(unsigned before)
{
	unsigned	i;

	for (i = 0; i < 2000 && arm_forty_got == before; i++)
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 1);
	return arm_forty_got != before;
}

/*
 * Each of these stops at the first bet settled as it should be: this thread can
 * lose its processor to the wakee first, on a tick or to a higher priority,
 * and a bet won has nothing to undo.  So a kernel is wrong only where no try
 * of twenty says otherwise.  1 settled, 0 wrong, -1 a direct switch took it.
 */
static int
arm_forty_no_call(mach_port_t host)
{
	unsigned		try, i, before, r0, t0, r1, t1, direct = 0;
	unsigned long long	start, took;

	for (try = 0; try < A40_TRIES; try++) {
		for (i = 0; i < try % 4; i++)	/* as arm 39 */
			(void) arm_forty_counts(host, &r0, &t0);
		if (!arm_thread_waiting(&arm_forty_thread)) {
			printf("act_test: [40] the woken thread never came to "
			       "wait — WRONG\n");
			return 0;
		}
		if (!arm_forty_counts(host, &r0, &t0))
			return 0;
		before = arm_forty_got;
		start = arm_thirty_nine_tsc();
		(void) arm_thirty_nine_send(arm_forty_port, MACH_SEND_HANDOFF,
					    A40_WAKE);
		if (arm_forty_got != before)
			direct++;
		while (arm_forty_got == before &&
		       arm_thirty_nine_tsc() - start < A40_SPIN)
			;
		took = arm_thirty_nine_tsc() - start;
		if (!arm_forty_ran(before)) {
			printf("act_test: [40] the thread woken by a send alone "
			       "never ran — WRONG\n");
			return 0;
		}
		if (!arm_forty_counts(host, &r1, &t1))
			return 0;
		if (t1 != t0) {
			printf("act_test: [40] a tick undid the bet of a send "
			       "alone, try %u; the thread ran %llu TSC ticks "
			       "after it\n", try + 1, took);
			return 1;
		}
	}
	if (direct != 0) {
		printf("act_test: [40] %u of %u sends alone were taken by a direct "
		       "switch, with no bet to lose — NOT ASKED\n", direct,
		       A40_TRIES);
		return -1;
	}
	printf("act_test: [40] %u sends alone, then only user code, and no tick "
	       "undid one — WRONG\n", A40_TRIES);
	return 0;
}

static int
arm_forty_combined(mach_port_t host, mach_port_t reply)
{
	unsigned	i, try, before, r0, t0, seen, direct = 0;

	for (i = 0; i < A40_WARM; i++)
		if (arm_forty_rpc(reply) != MACH_MSG_SUCCESS) {
			printf("act_test: [40] a call to the server failed — "
			       "WRONG\n");
			return 0;
		}
	for (try = 0; try < A40_TRIES; try++) {
		for (i = 0; i < try % 4; i++)	/* as arm 39 */
			(void) arm_forty_counts(host, &r0, &t0);
		if (!arm_thread_waiting(&arm_forty_thread)) {
			printf("act_test: [40] the woken thread never came to "
			       "wait — WRONG\n");
			return 0;
		}
		if (!arm_forty_counts(host, &r0, &t0))
			return 0;
		before = arm_forty_got;
		arm_forty_seen = r0 + t0;
		(void) arm_thirty_nine_send(arm_forty_port, MACH_SEND_HANDOFF,
					    A40_WAKE);
		if (arm_forty_got != before)
			direct++;
		if (arm_forty_rpc(reply) != MACH_MSG_SUCCESS) {
			printf("act_test: [40] a call to the server failed — "
			       "WRONG\n");
			return 0;
		}
		seen = arm_forty_seen;
		if (!arm_forty_ran(before)) {
			printf("act_test: [40] the thread woken before a combined "
			       "call never ran — WRONG\n");
			return 0;
		}
		if (seen != r0 + t0) {
			printf("act_test: [40] a combined call settled the "
			       "hand-off before its server ran, try %u\n",
			       try + 1);
			return 1;
		}
	}
	if (direct != 0) {
		printf("act_test: [40] %u of %u sends before a combined call were "
		       "taken by a direct switch — NOT ASKED\n", direct,
		       A40_TRIES);
		return -1;
	}
	printf("act_test: [40] %u combined calls went to their server with a "
	       "hand-off still pending — WRONG\n", A40_TRIES);
	return 0;
}

static int
arm_forty_asked(mach_port_t host)
{
	pthread_t	receiver, server;
	mach_port_t	reply;
	unsigned	reach, r0, t0;
	int		a = 0, b = 0;

	if (!arm_thirty_nine_counts(host, &reach, &r0, &t0)) {
		printf("act_test: [40] host_info refused the hand-off's counts "
		       "— WRONG\n");
		return 0;
	}
	if (reach == 0) {
		printf("act_test: [40] no hand-off on this kernel — NOT ASKED\n");
		return 1;
	}
	arm_forty_host = host;
	arm_forty_thread = MACH_PORT_NULL;
	arm_forty_port = arm_own_port(40);
	arm_forty_service = arm_own_port(40);
	reply = arm_own_port(40);
	if (arm_forty_port == MACH_PORT_NULL ||
	    arm_forty_service == MACH_PORT_NULL || reply == MACH_PORT_NULL)
		return 0;
	if (pthread_create(&receiver, NULL, arm_forty_receiver, NULL) != 0 ||
	    pthread_create(&server, NULL, arm_forty_server, NULL) != 0) {
		printf("act_test: [40] pthread_create failed — WRONG\n");
		return 0;
	}
	a = arm_forty_no_call(host);
	if (a != 0)
		b = arm_forty_combined(host, reply);
	(void) arm_thirty_nine_send(arm_forty_port, 0, A40_REPLY);
	(void) arm_thirty_nine_send(arm_forty_service, 0, A40_REPLY);
	(void) pthread_join(receiver, NULL);
	(void) pthread_join(server, NULL);
	(void) mach_port_deallocate(mach_task_self(), arm_forty_thread);
	(void) mach_port_destroy(mach_task_self(), reply);
	(void) mach_port_destroy(mach_task_self(), arm_forty_service);
	(void) mach_port_destroy(mach_task_self(), arm_forty_port);
	if (a > 0 && b > 0)
		printf("act_test: [40] a hand-off no call comes back to is undone "
		       "at a tick, and before a combined call's switch — OK\n");
	return a != 0 && b != 0;
}

static int
arm_forty_nothing_after_the_hand_off(void)
{
	mach_port_t	host = mach_host_self();
	int		ok = arm_forty_asked(host);

	(void) mach_port_deallocate(mach_task_self(), host);
	return ok;
}

/*
 * Arm forty-one (#867): a disposition in a message's body is the sender's own
 * byte.  ipc_object_copyin_type() panics on one it does not know, as it should
 * for the kernel's own messages, so the first pass over the body has to refuse
 * it before anything is taken.  Every value around the six known ones, in a
 * port descriptor, with a name and without, and in an out-of-line port array,
 * with names and empty.  With the check out the first of these stops the
 * kernel.  And disposition 0, which is legal with no right behind it and
 * refused with one.
 */
static const unsigned char a41_bad[] = { 1, 15, 22, 23, 127, 128, 255 };

struct a41_msg {
	mach_msg_header_t		h;
	mach_msg_body_t			b;
	mach_msg_port_descriptor_t	first;
	mach_msg_port_descriptor_t	port;
	mach_msg_ool_ports_descriptor_t	ports;
};

static void
a41_start(struct a41_msg *m, mach_port_t dest, unsigned descriptors)
{
	unsigned	i;

	for (i = 0; i < sizeof *m; i++)
		((unsigned char *) m)[i] = 0;
	m->h.msgh_bits = MACH_MSGH_BITS_COMPLEX
			 | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	m->h.msgh_size = sizeof m->h + sizeof m->b;
	m->h.msgh_remote_port = dest;
	m->h.msgh_local_port = MACH_PORT_NULL;
	m->h.msgh_id = 41;
	m->b.msgh_descriptor_count = descriptors;
}

static mach_msg_return_t
a41_send(struct a41_msg *m)
{
	return mach_msg(&m->h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, m->h.msgh_size,
			0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
}

/* A port descriptor as the message's only one. */
static mach_msg_return_t
a41_port(mach_port_t dest, mach_port_t name, unsigned disposition)
{
	struct a41_msg	m;

	a41_start(&m, dest, 1);
	m.first.name = name;
	m.first.disposition = disposition;
	m.first.type = MACH_MSG_PORT_DESCRIPTOR;
	m.h.msgh_size += sizeof m.first;
	return a41_send(&m);
}

/* An out-of-line port array as the message's only one, of count names. */
static mach_msg_return_t
a41_ports(mach_port_t dest, mach_port_t *names, unsigned count,
	  unsigned disposition)
{
	struct {
		mach_msg_header_t		h;
		mach_msg_body_t			b;
		mach_msg_ool_ports_descriptor_t	ports;
	} m;
	unsigned	i;

	for (i = 0; i < sizeof m; i++)
		((unsigned char *) &m)[i] = 0;
	m.h.msgh_bits = MACH_MSGH_BITS_COMPLEX
			| MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	m.h.msgh_size = sizeof m;
	m.h.msgh_remote_port = dest;
	m.h.msgh_id = 41;
	m.b.msgh_descriptor_count = 1;
	m.ports.address = names;
	m.ports.count = count;
	m.ports.copy = MACH_MSG_PHYSICAL_COPY;
	m.ports.disposition = disposition;
	m.ports.type = MACH_MSG_OOL_PORTS_DESCRIPTOR;
	return mach_msg(&m.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m, 0,
			MACH_PORT_NULL, 0, MACH_PORT_NULL);
}

static int
arm_forty_one_body_disposition(void)
{
	union {
		mach_msg_header_t	h;
		unsigned char		bytes[256];
	} r;
	struct a41_msg		m;
	mach_port_t		dest, p, one[1];
	mach_port_urefs_t	refs = 0;
	mach_msg_return_t	mr;
	unsigned		i, refused = 0, cases = 0;
	int			ok = 1;

	dest = arm_own_port(41);
	p = arm_own_port(41);
	if (dest == MACH_PORT_NULL || p == MACH_PORT_NULL) {
		printf("act_test: [41] the ports could not be made — WRONG\n");
		return 0;
	}
	one[0] = p;
	for (i = 0; i < sizeof a41_bad; i++) {
		unsigned	d = a41_bad[i];
		mach_msg_return_t a[4];
		unsigned	j;

		a[0] = a41_port(dest, p, d);
		a[1] = a41_port(dest, MACH_PORT_NULL, d);
		a[2] = a41_ports(dest, one, 1, d);
		a[3] = a41_ports(dest, one, 0, d);
		for (j = 0; j < 4; j++) {
			cases++;
			if (a[j] == MACH_SEND_INVALID_TYPE)
				refused++;
			else {
				printf("act_test: [41] disposition %u, case %u, "
				       "answered 0x%x — WRONG\n", d, j,
				       (int) a[j]);
				ok = 0;
			}
		}
	}

	/* A right named before the bad one is still there afterwards. */
	a41_start(&m, dest, 2);
	m.first.name = p;
	m.first.disposition = MACH_MSG_TYPE_MOVE_SEND;
	m.first.type = MACH_MSG_PORT_DESCRIPTOR;
	m.port.name = p;
	m.port.disposition = 23;
	m.port.type = MACH_MSG_PORT_DESCRIPTOR;
	m.h.msgh_size += sizeof m.first + sizeof m.port;
	mr = a41_send(&m);
	(void) mach_port_get_refs(mach_task_self(), p, MACH_PORT_RIGHT_SEND,
				  &refs);
	if (mr != MACH_SEND_INVALID_TYPE || refs != 1) {
		printf("act_test: [41] a message with a good right before a bad "
		       "one answered 0x%x, and left %u send right(s), not 1 — "
		       "WRONG\n", (int) mr, (unsigned) refs);
		ok = 0;
	}

	/* Disposition 0: refused with a right behind it, legal without. */
	mr = a41_port(dest, p, 0);
	if (mr != MACH_SEND_INVALID_RIGHT) {
		printf("act_test: [41] disposition 0 with a name answered 0x%x — "
		       "WRONG\n", (int) mr);
		ok = 0;
	}
	mr = a41_ports(dest, one, 1, 0);
	if (mr != MACH_SEND_INVALID_RIGHT) {
		printf("act_test: [41] disposition 0 in an array with a name "
		       "answered 0x%x — WRONG\n", (int) mr);
		ok = 0;
	}
	mr = a41_port(dest, MACH_PORT_NULL, 0);
	if (mr != MACH_MSG_SUCCESS) {
		printf("act_test: [41] disposition 0 with no name answered 0x%x "
		       "— WRONG\n", (int) mr);
		ok = 0;
	}
	(void) mach_msg(&r.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof r, dest,
			0, MACH_PORT_NULL);

	(void) mach_port_destroy(mach_task_self(), p);
	(void) mach_port_destroy(mach_task_self(), dest);
	if (ok)
		printf("act_test: [41] %u of %u bad dispositions refused, a right "
		       "named before one left where it was, and disposition 0 "
		       "as it should be — OK\n", refused, cases);
	return ok;
}

/*
 * Arm forty-two (#870): a simple message has no descriptors, whatever its
 * payload looks like.  This one's first word says one, and what follows is an
 * out-of-line descriptor, and the receive names a scatter list of a port array:
 * read as a gather list it does not match.  The count is one, and the message
 * holds that one descriptor, so that a kernel without the check stays inside
 * the message while it says the wrong thing.
 */
static int
arm_forty_two_simple_message_scattered(void)
{
	union {
		mach_msg_header_t	h;
		unsigned char		bytes[128];
	} buf, m;
	mach_msg_descriptor_t	d;
	mach_msg_size_t		scatter_size;
	mach_msg_return_t	mr;
	mach_port_t		port;
	unsigned		i, count = 1, same = 0;
	const unsigned		pay = sizeof (mach_msg_header_t);
	const unsigned		dsc = pay + sizeof (mach_msg_body_t);
	unsigned char		*from;

	port = arm_own_port(42);
	if (port == MACH_PORT_NULL)
		return 0;
	scatter_size = dsc + sizeof d;

	for (i = 0; i < sizeof m.bytes; i++)
		m.bytes[i] = buf.bytes[i] = 0;
	for (i = 0; i < sizeof d; i++)
		((unsigned char *) &d)[i] = 0;

	/* The sender's payload: a count, then what would be a descriptor. */
	m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	m.h.msgh_size = scatter_size;
	m.h.msgh_remote_port = port;
	m.h.msgh_id = 42;
	for (i = 0; i < sizeof count; i++)
		m.bytes[pay + i] = ((unsigned char *) &count)[i];
	d.type.type = MACH_MSG_OOL_DESCRIPTOR;
	from = (unsigned char *) &d;
	for (i = 0; i < sizeof d; i++)
		m.bytes[dsc + i] = from[i];

	/* The receiver's scatter list: one descriptor of another type. */
	for (i = 0; i < sizeof count; i++)
		buf.bytes[pay + i] = ((unsigned char *) &count)[i];
	d.type.type = MACH_MSG_OOL_PORTS_DESCRIPTOR;
	for (i = 0; i < sizeof d; i++)
		buf.bytes[dsc + i] = from[i];

	mr = mach_msg(&m.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, scatter_size, 0,
		      MACH_PORT_NULL, 0, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS) {
		printf("act_test: [42] the send answered 0x%x — WRONG\n", (int) mr);
		(void) mach_port_destroy(mach_task_self(), port);
		return 0;
	}
	mr = mach_msg_overwrite(&buf.h, MACH_RCV_MSG | MACH_RCV_OVERWRITE |
				MACH_RCV_TIMEOUT, 0, sizeof buf, port, 0,
				MACH_PORT_NULL, &buf.h, scatter_size);
	(void) mach_port_destroy(mach_task_self(), port);

	if (mr != MACH_MSG_SUCCESS) {
		printf("act_test: [42] a simple message whose payload does not "
		       "match the scatter list answered 0x%x — WRONG (#870)\n",
		       (int) mr);
		return 0;
	}
	for (i = 0; i < sizeof d; i++)
		same += buf.bytes[dsc + i] == m.bytes[dsc + i];
	if (buf.h.msgh_id != 42 || buf.h.msgh_size < scatter_size ||
	    same != sizeof d) {
		printf("act_test: [42] the message came back as id %d, %u bytes, "
		       "%u of %u payload bytes as sent — WRONG (#870)\n",
		       (int) buf.h.msgh_id, (unsigned) buf.h.msgh_size, same,
		       (unsigned) sizeof d);
		return 0;
	}
	printf("act_test: [42] a simple message passed a scatter list its "
	       "payload does not match, whole — OK (#870)\n");
	return 1;
}

/*
 * Arm forty-three (#871): a receive that blocks and ends with no message gives
 * back what it held.  Three ways out of it did not, and nothing failed: a port
 * stayed allocated for good, one for every nanosleep().  So the zone is what is
 * read.  Rounds of a fresh port, a receive that blocks until its timeout, and
 * the port destroyed; the same with a scatter list, whose size picks a kalloc
 * zone little else uses; and the same with the port destroyed under a thread
 * blocked on it.  Other tasks allocate as this runs, so a count that grew is
 * asked again before it is believed.  Not asked where host_zone_info is not
 * compiled in.
 */
/*
 * More than the magazines can hide.  zi_count moves when a magazine is refilled
 * or drained, and an element taken from one moves nothing: a magazine is 15 deep
 * and there is one for each processor (kern/zalloc.c), so that 16 lost ports read
 * as 0 in most boots.
 */
#define	A43_ROUNDS	128
#define	A43_TRIES	3
#define	A43_LIST	3000	/* kalloc.4096 */

static volatile mach_port_t	a43_thread, a43_port;
static volatile unsigned	a43_ended;
static volatile int		a43_stop;

static int
a43_same(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static int
a43_zone(mach_port_t host, const char *name, int *count)
{
	zone_name_t		*names = 0;
	zone_info_t		*info = 0;
	mach_msg_type_number_t	n = 0, m = 0, i;
	int			found = 0;

	if (host_zone_info(host, &names, &n, &info, &m) != KERN_SUCCESS)
		return 0;
	for (i = 0; i < n && i < m; i++)
		if (a43_same(names[i].zn_name, name)) {
			*count = info[i].zi_count;
			found = 1;
		}
	(void) vm_deallocate(mach_task_self(), (vm_address_t) names,
			     n * sizeof *names);
	(void) vm_deallocate(mach_task_self(), (vm_address_t) info,
			     m * sizeof *info);
	return found;
}

static void *
a43_blocked(void *arg)
{
	union {
		mach_msg_header_t	h;
		char			bytes[128];
	} m;

	(void) arg;
	a43_thread = mach_thread_self();
	while (!a43_stop) {
		mach_port_t	p = a43_port;

		if (p == MACH_PORT_NULL) {
			(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS,
					     1);
			continue;
		}
		(void) mach_msg(&m.h, MACH_RCV_MSG, 0, sizeof m, p,
				MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		a43_port = MACH_PORT_NULL;
		a43_ended++;
	}
	return NULL;
}

/* How far `zone' moved over one round of `what'; 0 where it cannot be read. */
static int
a43_round(mach_port_t host, const char *zone, int what, int *grew)
{
	static unsigned char	list[sizeof (mach_msg_header_t) + A43_LIST];
	union {
		mach_msg_header_t	h;
		char			bytes[128];
	} m;
	int		before, after;
	unsigned	i, j;

	if (!a43_zone(host, zone, &before))
		return 0;
	for (i = 0; i < A43_ROUNDS; i++) {
		mach_port_t port = arm_own_port(43);

		if (port == MACH_PORT_NULL)
			return 0;
		if (what == 0)
			(void) mach_msg(&m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
					sizeof m, port, 1, MACH_PORT_NULL);
		else if (what == 1)
			(void) mach_msg_overwrite((mach_msg_header_t *) list,
						  MACH_RCV_MSG | MACH_RCV_OVERWRITE |
						  MACH_RCV_TIMEOUT, 0, sizeof list,
						  port, 1, MACH_PORT_NULL,
						  (mach_msg_header_t *) list,
						  sizeof list);
		else {
			unsigned	ended = a43_ended;

			a43_port = port;
			(void) arm_thread_waiting(&a43_thread);
			(void) mach_port_destroy(mach_task_self(), port);
			for (j = 0; j < 2000 && a43_ended == ended; j++)
				(void) thread_switch(MACH_PORT_NULL,
						     SWITCH_OPTION_DEPRESS, 1);
			continue;
		}
		(void) mach_port_destroy(mach_task_self(), port);
	}
	if (!a43_zone(host, zone, &after))
		return 0;
	*grew = after - before;
	return 1;
}

static int
a43_asked(mach_port_t host, const char *zone, int what, const char *how)
{
	int		grew = 0;
	unsigned	try;

	for (try = 0; try < A43_TRIES; try++) {
		if (!a43_round(host, zone, what, &grew)) {
			printf("act_test: [43] %s could not be read — WRONG\n",
			       zone);
			return 0;
		}
		if (grew < A43_ROUNDS / 2) {
			printf("act_test: [43] %d receive(s) %s moved %s by %d\n",
			       A43_ROUNDS, how, zone, grew);
			return 1;
		}
	}
	printf("act_test: [43] %d receive(s) %s grew %s by %d, %u time(s) out of "
	       "%u — WRONG (#871)\n", A43_ROUNDS, how, zone, grew, A43_TRIES,
	       A43_TRIES);
	return 0;
}

static int
arm_forty_three_receive_gives_back(void)
{
	mach_port_t	host = mach_host_self();
	pthread_t	blocked;
	int		count, ok = 1;

	if (!a43_zone(host, "ipc ports", &count)) {
		printf("act_test: [43] host_zone_info does not answer here — NOT "
		       "ASKED\n");
		(void) mach_port_deallocate(mach_task_self(), host);
		return 1;
	}
	a43_thread = MACH_PORT_NULL;
	a43_port = MACH_PORT_NULL;
	a43_stop = 0;
	if (pthread_create(&blocked, NULL, a43_blocked, NULL) != 0) {
		printf("act_test: [43] pthread_create failed — WRONG\n");
		(void) mach_port_deallocate(mach_task_self(), host);
		return 0;
	}
	if (!a43_asked(host, "ipc ports", 0, "timed out"))
		ok = 0;
	if (!a43_asked(host, "kalloc.4096", 1, "timed out with a scatter list"))
		ok = 0;
	if (!a43_asked(host, "ipc ports", 2, "ended by their port's death"))
		ok = 0;
	a43_stop = 1;
	(void) pthread_join(blocked, NULL);
	(void) mach_port_deallocate(mach_task_self(), a43_thread);
	(void) mach_port_deallocate(mach_task_self(), host);
	if (ok)
		printf("act_test: [43] a blocked receive that ends with no message "
		       "gives back its port and its scatter list — OK\n");
	return ok;
}

/*
 * Arm forty-four (#880): a name with index 0 is refused by every route that
 * lets a task choose one.  No lookup can reach such a name: index 0 of a table
 * is the free list's head.  The arm stops at the first answer that is not the
 * refusal, because a right moved to such a name can stop the kernel at the next
 * receive, and the answer is what is being judged.
 */
static int
a44_refused(const char *route, mach_port_t name, kern_return_t kr)
{
	if (kr == KERN_INVALID_VALUE)
		return 1;
	printf("act_test: [44] %s for name 0x%x, of index 0, answered 0x%x, not "
	       "KERN_INVALID_VALUE — WRONG (#880)\n", route, (unsigned) name,
	       (int) kr);
	return 0;
}

static int
arm_forty_four_index_zero_name(void)
{
	mach_port_t	names[2], port, valid;
	mach_port_t	task = mach_task_self();
	kern_return_t	kr;
	unsigned	i;

	names[0] = 1;					/* the lowest */
	names[1] = (1U << MACH_PORT_GEN_BITS) - 1;	/* the highest */
	port = arm_own_port(44);
	if (port == MACH_PORT_NULL)
		return 0;
	for (i = 0; i < 2; i++) {
		if (!a44_refused("mach_port_allocate_name()", names[i],
				 mach_port_allocate_name(task,
							 MACH_PORT_RIGHT_RECEIVE,
							 names[i])))
			return 0;
		if (!a44_refused("mach_port_insert_right()", names[i],
				 mach_port_insert_right(task, names[i], port,
							MACH_MSG_TYPE_MAKE_SEND)))
			return 0;
		if (!a44_refused("mach_port_rename()", names[i],
				 mach_port_rename(task, port, names[i])))
			return 0;
	}

	/* The lowest index that can be reached is still a name to ask for. */
	valid = 1U << MACH_PORT_GEN_BITS;
	kr = mach_port_allocate_name(task, MACH_PORT_RIGHT_RECEIVE, valid);
	if (kr == KERN_SUCCESS)
		(void) mach_port_destroy(task, valid);
	else if (kr != KERN_NAME_EXISTS) {
		printf("act_test: [44] the lowest index that can be reached, name "
		       "0x%x, answered 0x%x — WRONG (#880)\n", (unsigned) valid,
		       (int) kr);
		return 0;
	}
	(void) mach_port_destroy(task, port);
	printf("act_test: [44] names 0x%x and 0x%x refused by all three routes, "
	       "and index 1 still a name to ask for — OK\n", (unsigned) names[0],
	       (unsigned) names[1]);
	return 1;
}

/*
 * Arm forty-five (#881): a message taken off a port wakes a sender parked on
 * it, by the hot path too.  A port with a limit of one, a server parked by the
 * hot path, and a thread that sends two messages: the first is handed to the
 * server and fills the count, the second parks until the server runs.  With no
 * wakeup it stays parked on a port with room.  At one processor that happens by
 * itself; at more, the server is there first unless
 * ablations/881-widen-receiver-held.patch holds it, and then nothing was asked.
 *
 * The second send has no timeout, and that is the point of it.  A timeout would
 * end the wait, and ipc_mqueue_send() goes round once more when it does, finds
 * the room, delivers and answers success: a lost wakeup would cost half a second
 * and show in no answer.  So this thread is what times it, and the port's death
 * is what lets it go.
 *
 * Two rounds, because the two questions get in each other's way.  In the second,
 * while the sender is parked, a kernel RPC with this port as its reply port: the
 * queue is empty and a sender is parked, which the hot path once asserted could
 * not be.  That reply makes no room, and a sender still parked after it is let go
 * by destroying the port.  But a server that has come back to the port by then
 * takes that reply as it would any message, the slow way, and that wakes the
 * sender, which then says nothing of the hot path's wakeup.  So the first round
 * makes no such call.
 * A limit of 0, which mach_port.defs rules out, is refused.
 */
#define	A45_RPC		4500
#define	A45_ONE		4501
#define	A45_TWO		4502
#define	A45_FIN		4503
#define	A45_NO_SUCH_ID	999999	/* the kernel answers it with MIG_BAD_ID */
#define	A45_TIMEOUT	500
#define	A45_TICKS	2000	/* how long it may stay parked: 20 s */

static mach_port_t	a45_port;
static volatile mach_port_t a45_server_thread, a45_sender_thread;
static volatile mach_msg_return_t a45_second;
static volatile int	a45_second_next, a45_done;

static void *
a45_server(void *arg)
{
	union {
		mach_msg_header_t	h;
		char			bytes[128];
	} m;
	mach_msg_option_t	opt = MACH_RCV_MSG;
	mach_msg_size_t		size = 0;

	(void) arg;
	a45_server_thread = mach_thread_self();
	for (;;) {
		if (mach_msg(&m.h, opt, size, sizeof m, a45_port,
			     MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL)
		    != MACH_MSG_SUCCESS || m.h.msgh_id == A45_TWO)
			return NULL;
		if (m.h.msgh_id == A45_RPC) {
			m.h.msgh_bits = MACH_MSGH_BITS(
			    MACH_MSGH_BITS_REMOTE(m.h.msgh_bits), 0);
			m.h.msgh_size = sizeof m.h;
			m.h.msgh_local_port = MACH_PORT_NULL;
			m.h.msgh_id = A45_RPC + 100;
			size = sizeof m.h;
			opt = MACH_SEND_MSG | MACH_RCV_MSG;
		} else {
			size = 0;
			opt = MACH_RCV_MSG;
		}
	}
}

static mach_msg_return_t
a45_send(int id, mach_msg_option_t option, mach_msg_timeout_t timeout)
{
	mach_msg_header_t	h;

	h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	h.msgh_size = sizeof h;
	h.msgh_remote_port = a45_port;
	h.msgh_local_port = MACH_PORT_NULL;
	h.msgh_reserved = 0;
	h.msgh_id = id;
	return mach_msg(&h, MACH_SEND_MSG | option, sizeof h, 0, MACH_PORT_NULL,
			timeout, MACH_PORT_NULL);
}

static void *
a45_sender(void *arg)
{
	(void) arg;
	a45_sender_thread = mach_thread_self();
	(void) a45_send(A45_ONE, MACH_SEND_TIMEOUT, 0);
	a45_second_next = 1;
	a45_second = a45_send(A45_TWO, 0, 0);
	(void) a45_send(A45_FIN, MACH_SEND_TIMEOUT, A45_TIMEOUT);
	a45_done = 1;
	return NULL;
}

/* The answer to a combined call to `dest', received on a45_port's name. */
static mach_msg_return_t
a45_rpc(mach_port_t dest, mach_port_t rcv, int id)
{
	union {
		mach_msg_header_t	h;
		char			bytes[128];
	} m;

	m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
				       MACH_MSG_TYPE_MAKE_SEND_ONCE);
	m.h.msgh_size = sizeof m.h;
	m.h.msgh_remote_port = dest;
	m.h.msgh_local_port = rcv;
	m.h.msgh_reserved = 0;
	m.h.msgh_id = id;
	return mach_msg(&m.h, MACH_SEND_MSG | MACH_RCV_MSG, sizeof m.h, sizeof m,
			rcv, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

/*
 * Answers 1 for a sender seen parked that went on, 0 for one that did not, and
 * -1 if it was never seen parked.
 */
static int
a45_round(int with_rpc)
{
	const char		*round = with_rpc ? ", around a kernel RPC" : "";
	mach_port_t		task = mach_task_self(), reply;
	mach_port_limits_t	l;
	struct thread_basic_info info;
	mach_msg_type_number_t	n;
	mach_msg_return_t	mr = MACH_MSG_SUCCESS;
	pthread_t		server, sender;
	kern_return_t		kr;
	unsigned		i;
	int			parked = 0, gone = 0, ok = 1;

	a45_port = arm_own_port(45);
	reply = arm_own_port(45);
	if (a45_port == MACH_PORT_NULL || reply == MACH_PORT_NULL)
		return 0;

	l.mpl_qlimit = 0;
	kr = mach_port_set_attributes(task, a45_port, MACH_PORT_LIMITS_INFO,
				      (mach_port_info_t) &l,
				      MACH_PORT_LIMITS_INFO_COUNT);
	if (kr != KERN_INVALID_VALUE) {
		printf("act_test: [45] a limit of 0 answered 0x%x — WRONG\n",
		       (int) kr);
		return 0;
	}
	l.mpl_qlimit = 1;
	kr = mach_port_set_attributes(task, a45_port, MACH_PORT_LIMITS_INFO,
				      (mach_port_info_t) &l,
				      MACH_PORT_LIMITS_INFO_COUNT);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [45] a limit of 1 answered 0x%x — WRONG\n",
		       (int) kr);
		return 0;
	}

	a45_server_thread = a45_sender_thread = MACH_PORT_NULL;
	a45_second = -1;
	a45_second_next = a45_done = 0;
	if (pthread_create(&server, NULL, a45_server, NULL) != 0) {
		printf("act_test: [45] pthread_create failed — WRONG\n");
		return 0;
	}
	/* Until the server sits in a combined call, parked by the hot path. */
	for (i = 0; i < 3 && mr == MACH_MSG_SUCCESS; i++)
		mr = a45_rpc(a45_port, reply, A45_RPC);
	if (mr != MACH_MSG_SUCCESS || !arm_thread_waiting(&a45_server_thread)) {
		printf("act_test: [45] the server did not come to wait: 0x%x — "
		       "WRONG\n", (int) mr);
		return 0;
	}

	if (pthread_create(&sender, NULL, a45_sender, NULL) != 0) {
		printf("act_test: [45] pthread_create failed — WRONG\n");
		return 0;
	}
	for (i = 0; i < 4000 && !a45_done && !parked; i++) {
		n = THREAD_BASIC_INFO_COUNT;
		if (a45_second_next &&
		    thread_info(a45_sender_thread, THREAD_BASIC_INFO,
				(thread_info_t) &info, &n) == KERN_SUCCESS &&
		    info.run_state == TH_STATE_WAITING)
			parked = 1;
		else
			(void) thread_switch(MACH_PORT_NULL,
					     SWITCH_OPTION_WAIT, 1);
	}
	if (with_rpc && parked && !a45_done) {
		/* A reply to this port, with the sender parked on it. */
		mr = a45_rpc(task, a45_port, A45_NO_SUCH_ID);
		if (mr != MACH_MSG_SUCCESS && mr != MACH_RCV_TOO_LARGE) {
			printf("act_test: [45] a kernel RPC with this reply port "
			       "answered 0x%x — WRONG (#881)\n", (int) mr);
			ok = 0;
		}
		n = THREAD_BASIC_INFO_COUNT;
		if (thread_info(a45_sender_thread, THREAD_BASIC_INFO,
				(thread_info_t) &info, &n) == KERN_SUCCESS &&
		    info.run_state == TH_STATE_WAITING) {
			(void) mach_port_destroy(task, a45_port);
			gone = 1;
		}
	}
	/*
	 * In time, and not in polls: a depressed thread_switch() comes back at
	 * once with nothing else to run, and 4000 of those are over before the
	 * sender has got anywhere under the widen.
	 */
	for (i = 0; i < A45_TICKS && !a45_done; i++)
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 1);

	if (!a45_done) {
		printf("act_test: [45] after %u ticks the sender stayed parked on "
		       "a port with room — WRONG (#881)\n", i);
		ok = 0;
		(void) mach_port_destroy(task, a45_port);
		gone = 1;
	} else if (a45_second != MACH_MSG_SUCCESS) {
		printf("act_test: [45] the second send answered 0x%x — WRONG "
		       "(#881)\n", (int) a45_second);
		ok = 0;
	}
	(void) pthread_join(sender, NULL);
	if (!gone)
		(void) mach_port_destroy(task, a45_port);
	(void) mach_port_destroy(task, reply);
	(void) pthread_join(server, NULL);
	(void) mach_port_deallocate(task, a45_sender_thread);
	(void) mach_port_deallocate(task, a45_server_thread);
	if (!ok)
		return 0;
	if (!parked) {
		printf("act_test: [45] the server took the first message before "
		       "the second was sent%s — NOT ASKED\n", round);
		return -1;
	}
	if (gone)
		printf("act_test: [45] a sender stayed parked through a kernel "
		       "reply on its port, and the port's death let it go — "
		       "OK\n");
	else
		printf("act_test: [45] a sender parked by a message the hot path "
		       "held went on when it was taken%s — OK\n", round);
	return 1;
}

static int
arm_forty_five_parked_sender(void)
{
	int	alone, around;

	alone = a45_round(0);
	if (alone == 0)
		return 0;
	around = a45_round(1);
	if (around == 0)
		return 0;
	return (alone > 0 || around > 0) ? 1 : -1;
}

/*
 * Arm forty-six (#887): the flavor of an exception action.  It used to be
 * stored as given, and a state exception indexed a table with it.
 *
 * The four routines are asked for flavors below, inside and above the table,
 * with each behavior that carries state, and every answer is printed as read.
 * EXCEPTION_DEFAULT and a null port are asked too, since neither ever uses the
 * flavor and callers pass 0, THREAD_STATE_NONE and worse.  No exception is
 * raised here: with the refusal gone these answers change and nothing faults.
 */
#define	A46_MASK	EXC_MASK_SOFTWARE
#define	A46_ROUTINES	4

static const char *const a46_routine[A46_ROUTINES] = {
	"thread_set", "task_set", "thread_swap", "task_swap"
};
static const int	a46_outside[] = {
	-1, 0, THREAD_STATE_NONE, THREAD_STATE_NONE + 1, 99,
	0x7fffffff, -0x7fffffff - 1
};
static const int	a46_inside[] = {
	x86_64_THREAD_STATE, x86_64_FLOAT_STATE, x86_64_EXCEPTION_STATE
};

#define	A46_N(a)	(sizeof (a) / sizeof ((a)[0]))

static kern_return_t
a46_ask(unsigned routine, mach_port_t thread, mach_port_t port,
	exception_behavior_t behavior, thread_state_flavor_t flavor)
{
	exception_mask_t	masks[EXC_TYPES_COUNT];
	mach_port_t		ports[EXC_TYPES_COUNT];
	exception_behavior_t	behaviors[EXC_TYPES_COUNT];
	thread_state_flavor_t	flavors[EXC_TYPES_COUNT];
	mach_msg_type_number_t	n = EXC_TYPES_COUNT, i;
	kern_return_t		kr;

	switch (routine) {
	case 0:
		return thread_set_exception_ports(thread, A46_MASK, port,
						  behavior, flavor);
	case 1:
		return task_set_exception_ports(mach_task_self(), A46_MASK,
						port, behavior, flavor);
	case 2:
		kr = thread_swap_exception_ports(thread, A46_MASK, port,
						 behavior, flavor, masks, &n,
						 ports, behaviors, flavors);
		break;
	default:
		kr = task_swap_exception_ports(mach_task_self(), A46_MASK,
					       port, behavior, flavor, masks,
					       &n, ports, behaviors, flavors);
		break;
	}
	if (kr == KERN_SUCCESS)
		for (i = 0; i < n; i++)
			if (MACH_PORT_VALID(ports[i]))
				(void) mach_port_deallocate(mach_task_self(),
							    ports[i]);
	return kr;
}

/* One routine and one behavior: the numbers on one line, as they were read. */
static unsigned
a46_one_behavior(unsigned routine, mach_port_t thread, mach_port_t port,
		 exception_behavior_t behavior, const char *name)
{
	kern_return_t	kr;
	unsigned	i, bad = 0;

	printf("act_test: [46] %s, %s, outside:", a46_routine[routine], name);
	for (i = 0; i < A46_N(a46_outside); i++) {
		kr = a46_ask(routine, thread, port, behavior, a46_outside[i]);
		printf(" %d=0x%x", a46_outside[i], kr);
		bad += kr != KERN_INVALID_ARGUMENT;
	}
	printf(", inside:");
	for (i = 0; i < A46_N(a46_inside); i++) {
		kr = a46_ask(routine, thread, port, behavior, a46_inside[i]);
		printf(" %d=0x%x", a46_inside[i], kr);
		bad += kr != KERN_SUCCESS;
	}
	printf("\n");
	return bad;
}

/* What never uses the flavor takes any flavor. */
static unsigned
a46_flavor_unused(unsigned routine, mach_port_t thread, mach_port_t port)
{
	kern_return_t	kr;
	unsigned	i, bad = 0;

	for (i = 0; i < A46_N(a46_outside); i++) {
		kr = a46_ask(routine, thread, port, EXCEPTION_DEFAULT,
			     a46_outside[i]);
		if (kr != KERN_SUCCESS) {
			printf("act_test: [46] %s, EXCEPTION_DEFAULT with "
			       "flavor %d answered 0x%x\n",
			       a46_routine[routine], a46_outside[i], kr);
			bad++;
		}
		kr = a46_ask(routine, thread, MACH_PORT_NULL, EXCEPTION_STATE,
			     a46_outside[i]);
		if (kr != KERN_SUCCESS) {
			printf("act_test: [46] %s, a null port with flavor %d "
			       "answered 0x%x\n",
			       a46_routine[routine], a46_outside[i], kr);
			bad++;
		}
	}
	return bad;
}

static int
arm_forty_six_exception_flavor(void)
{
	mach_port_t	port, thread = mach_thread_self();
	unsigned	r, bad = 0;

	port = arm_own_port(46);
	if (port == MACH_PORT_NULL)
		return 0;

	for (r = 0; r < A46_ROUTINES; r++) {
		bad += a46_one_behavior(r, thread, port, EXCEPTION_STATE,
					"EXCEPTION_STATE");
		bad += a46_one_behavior(r, thread, port,
					EXCEPTION_STATE_IDENTITY,
					"EXCEPTION_STATE_IDENTITY");
		bad += a46_flavor_unused(r, thread, port);
	}

	(void) thread_set_exception_ports(thread, A46_MASK, MACH_PORT_NULL,
					  EXCEPTION_DEFAULT, THREAD_STATE_NONE);
	(void) task_set_exception_ports(mach_task_self(), A46_MASK,
					MACH_PORT_NULL, EXCEPTION_DEFAULT,
					THREAD_STATE_NONE);
	(void) mach_port_deallocate(mach_task_self(), thread);
	(void) mach_port_destroy(mach_task_self(), port);

	if (bad != 0) {
		printf("act_test: [46] %u answer(s) of the four routines are "
		       "not what the flavor asks for, a flavor outside the "
		       "table can reach an exception — WRONG (#887)\n", bad);
		return 0;
	}
	printf("act_test: [46] the four routines refuse a flavor with no state "
	       "for a behavior that sends one, and take any flavor where none "
	       "is used — OK\n");
	return 1;
}

/*
 * Arm forty-seven (#887): an exception whose state cannot be read gives back
 * the send right it took for the message.
 *
 * Only the message consumes that right.  When there is no state there is no
 * message, and each such exception used to leave one right behind, so that the
 * port's no-senders notification could never fire.  A thread registers flavor
 * 0 and faults, the exception goes on to the task's port, where the thread is
 * ended, and then every right this arm knows of goes.
 *
 * A healthy kernel refuses the flavor, so here the arm has nothing to ask:
 * ablations/887-flavor-not-refused.patch is what lets it run, and
 * 887-state-failure-keeps-the-right.patch, which goes further, what makes it
 * say WRONG.
 */
static mach_port_t		a47_port;
static mach_port_t		a47_thread;
static volatile kern_return_t	a47_set_kr = -1;
static volatile int		a47_set_done;

static void *
a47_victim(void *arg)
{
	(void) arg;
	a47_thread = mach_thread_self();
	a47_set_kr = thread_set_exception_ports(a47_thread,
						EXC_MASK_BAD_ACCESS, a47_port,
						EXCEPTION_STATE, 0);
	a47_set_done = 1;
	if (a47_set_kr == KERN_SUCCESS)
		(void) *(volatile unsigned long *) A32_PROBE;
	return NULL;
}

static int
arm_forty_seven_failed_state_gives_back(void)
{
	union {
		mach_msg_header_t	h;
		unsigned char		bytes[1024];
	} req, ans;
	mach_port_t		kport, old;
	mach_msg_return_t	mr;
	kern_return_t		kr;
	pthread_t		victim;
	int			ok = 1;

	a47_port = arm_own_port(47);
	kport = arm_own_port(47);
	if (a47_port == MACH_PORT_NULL || kport == MACH_PORT_NULL)
		return 0;
	kr = task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS,
				      kport, EXCEPTION_DEFAULT,
				      THREAD_STATE_NONE);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [47] task_set_exception_ports answered 0x%x "
		       "— WRONG\n", kr);
		return 0;
	}

	arm_forty_seven_armed = 1;
	if (pthread_create(&victim, NULL, a47_victim, NULL) != 0) {
		printf("act_test: [47] pthread_create failed — WRONG\n");
		ok = 0;
	}
	while (ok && !a47_set_done)
		(void) thread_switch(MACH_PORT_NULL,
				     SWITCH_OPTION_DEPRESS, 1);

	if (ok && a47_set_kr == KERN_SUCCESS) {
		mr = mach_msg(&req.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			      sizeof req, kport, 5000, MACH_PORT_NULL);
		if (mr == MACH_MSG_SUCCESS) {
			(void) exc_server(&req.h, &ans.h);
			(void) mach_msg(&ans.h,
					MACH_SEND_MSG | MACH_SEND_TIMEOUT,
					ans.h.msgh_size, 0, MACH_PORT_NULL, 0,
					MACH_PORT_NULL);
		}
		if (mr != MACH_MSG_SUCCESS || !arm_forty_seven_task_seen) {
			printf("act_test: [47] the fault did not reach the "
			       "task's port: 0x%x, seen %d — WRONG\n",
			       (int) mr, arm_forty_seven_task_seen);
			ok = 0;
		}
	}
	arm_forty_seven_armed = 0;
	(void) task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS,
					MACH_PORT_NULL, EXCEPTION_DEFAULT,
					THREAD_STATE_NONE);
	(void) mach_port_destroy(mach_task_self(), kport);
	if (a47_thread != MACH_PORT_NULL)
		(void) mach_port_deallocate(mach_task_self(), a47_thread);

	if (ok && a47_set_kr != KERN_SUCCESS) {
		printf("act_test: [47] flavor 0 was refused, 0x%x, so no "
		       "exception can fail to read its state — NOT ASKED\n",
		       a47_set_kr);
		(void) mach_port_destroy(mach_task_self(), a47_port);
		return -1;
	}
	if (!ok) {
		(void) mach_port_destroy(mach_task_self(), a47_port);
		return 0;
	}

	/*
	 * The thread is gone, and its exception port with it, once it has
	 * been reaped: the one right left is this arm's, and when that goes
	 * the notification comes.  Until the thread is reaped the kernel holds
	 * a right of its own, so the wait is a long one and only its end says
	 * anything.
	 */
	kr = mach_port_request_notification(mach_task_self(), a47_port,
					    MACH_NOTIFY_NO_SENDERS, 0, a47_port,
					    MACH_MSG_TYPE_MAKE_SEND_ONCE, &old);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [47] the request answered 0x%x — WRONG\n",
		       kr);
		(void) mach_port_destroy(mach_task_self(), a47_port);
		return 0;
	}
	(void) mach_port_deallocate(mach_task_self(), a47_port);
	mr = mach_msg(&req.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof req,
		      a47_port, 5000, MACH_PORT_NULL);
	(void) mach_port_destroy(mach_task_self(), a47_port);
	if (mr != MACH_MSG_SUCCESS || req.h.msgh_id != MACH_NOTIFY_NO_SENDERS) {
		printf("act_test: [47] no no-senders notification in 5 s, 0x%x: "
		       "the failed exception kept a send right nobody can "
		       "give back — WRONG (#887)\n", (int) mr);
		return 0;
	}
	printf("act_test: [47] an exception that could not read its state gave "
	       "its send right back: the notification came — OK\n");
	return 1;
}

/*
 * Arm fifty (#851): the holes in two structures any task can ask for about
 * itself.  They are filled field by field, and the holes used to bring back what
 * was on the kernel's stack.  The buffer starts as anything but zero, so a zero
 * there was written.
 */
static unsigned
a50_hole(const unsigned char *p, size_t from, size_t to, const char *what)
{
	size_t		i;
	unsigned	bad = 0;

	for (i = from; i < to; i++)
		if (p[i] != 0) {
			printf("act_test: [50] byte %u of %s, a hole, came back "
			       "as 0x%x\n", (unsigned) i, what, p[i]);
			bad++;
		}
	return bad;
}

#define	A50_END(type, field)						\
	(__builtin_offsetof(type, field) + sizeof (((type *) 0)->field))

static int
arm_fifty_holes_come_back_zero(void)
{
	static union {
		struct task_basic_info		t;
		vm_region_basic_info_data_t	r;
		unsigned char			b[256];
	} u;
	vm_address_t		addr = (vm_address_t) &u;
	vm_size_t		size = 0;
	mach_port_t		object = MACH_PORT_NULL;
	mach_msg_type_number_t	n;
	kern_return_t		kr;
	unsigned		i, bad = 0;

	for (i = 0; i < sizeof u; i++)
		u.b[i] = 0xa5;
	n = TASK_BASIC_INFO_COUNT;
	kr = task_info(mach_task_self(), TASK_BASIC_INFO,
		       (task_info_t) &u.t, &n);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [50] task_info answered 0x%x — WRONG\n", kr);
		return 0;
	}
	bad += a50_hole(u.b, A50_END(struct task_basic_info, suspend_count),
			__builtin_offsetof(struct task_basic_info, virtual_size),
			"task_basic_info");
	bad += a50_hole(u.b, A50_END(struct task_basic_info, policy),
			sizeof (struct task_basic_info), "task_basic_info");

	for (i = 0; i < sizeof u; i++)
		u.b[i] = 0xa5;
	n = VM_REGION_BASIC_INFO_COUNT;
	kr = vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO,
		       (vm_region_info_t) &u.r, &n, &object);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [50] vm_region answered 0x%x — WRONG\n", kr);
		return 0;
	}
	bad += a50_hole(u.b, A50_END(vm_region_basic_info_data_t, reserved),
			__builtin_offsetof(vm_region_basic_info_data_t, offset),
			"vm_region_basic_info");
	bad += a50_hole(u.b, A50_END(vm_region_basic_info_data_t,
				     user_wired_count),
			sizeof (vm_region_basic_info_data_t),
			"vm_region_basic_info");

	if (bad != 0) {
		printf("act_test: [50] %u byte(s) of holes came back from the "
		       "kernel not zero — WRONG (#851)\n", bad);
		return 0;
	}
	printf("act_test: [50] the holes in task_basic_info and "
	       "vm_region_basic_info came back zero — OK (#851)\n");
	return 1;
}

/*
 * Arm forty-eight (#889): thread_switch() with a hint, against the two paths
 * that take a thread's activation and then its port.  One thread asks for its
 * own port in a loop, and is the hint.  Then another thread makes thread after
 * thread and ends each, and each is the hint while it lives.  Both orders of
 * those two locks together sleep for ever, so what is asked is only that all of
 * it ends, and it is this thread, in neither of them, that says so.  At one
 * processor a tick between the two locks is enough, which is why the others
 * yield: a hint hands over the rest of the quantum.
 *
 * Every thread is made before anything starts, because this one could not make
 * another once it has stopped.  The stop does not stay with the two: libpthreads
 * calls task_threads() every two seconds for a thread waiting to be joined, that
 * takes the task's lock and then each activation's, and it sleeps behind them
 * holding the first.  thread_create() then asks for that lock for ever, in
 * ref_task_port_locked().  What is left here, thread_switch() with no hint and
 * printf(), needs neither.
 */
#define	A48_MS_SELF	2000
#define	A48_KILLS	32
#define	A48_MS_LIMIT	60000

static volatile mach_port_t	a48_hint;
static volatile int		a48_go, a48_kill_go, a48_stop;
static volatile int		a48_self_done, a48_switch_done, a48_kill_done;
static volatile unsigned	a48_selfs, a48_switches, a48_kills;

static void
a48_wait_for(volatile int *flag)
{
	while (!*flag)
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 1);
}

static void *
a48_self_loop(void *arg)
{
	mach_port_t	t;

	(void) arg;
	a48_hint = mach_thread_self();
	a48_wait_for(&a48_go);
	while (!a48_stop) {
		t = mach_thread_self();
		(void) mach_port_deallocate(mach_task_self(), t);
		a48_selfs++;
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0);
	}
	a48_self_done = 1;
	return NULL;
}

static void *
a48_switch_loop(void *arg)
{
	(void) arg;
	a48_wait_for(&a48_go);
	while (!a48_stop) {
		(void) thread_switch(a48_hint, SWITCH_OPTION_NONE, 0);
		a48_switches++;
	}
	a48_switch_done = 1;
	return NULL;
}

static void
a48_spin(void)
{
	for (;;)
		__asm__ volatile ("pause");
}

static void *
a48_kill_loop(void *arg)
{
	struct x86_64_thread_state	state;
	thread_act_t			t;
	vm_address_t			stack = 0;
	mach_port_t			self = a48_hint;
	unsigned			i, round;

	(void) arg;
	a48_wait_for(&a48_kill_go);
	if (vm_allocate(mach_task_self(), &stack, vm_page_size, TRUE)
	    != KERN_SUCCESS)
		return NULL;
	for (i = 0; i < sizeof state; i++)
		((volatile char *) &state)[i] = 0;
	state.rip = (unsigned long) a48_spin;
	state.rsp = stack + vm_page_size - 24;
	state.rflags = 0x202;
	for (round = 0; round < A48_KILLS; round++) {
		if (thread_create(mach_task_self(), &t) != KERN_SUCCESS)
			break;
		if (thread_set_state(t, x86_64_THREAD_STATE,
				     (thread_state_t) &state,
				     x86_64_THREAD_STATE_COUNT) != KERN_SUCCESS ||
		    thread_resume(t) != KERN_SUCCESS) {
			(void) thread_terminate(t);
			(void) mach_port_deallocate(mach_task_self(), t);
			break;
		}
		a48_hint = t;
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 1);
		(void) thread_terminate(t);
		a48_hint = self;
		(void) mach_port_deallocate(mach_task_self(), t);
		a48_kills++;
	}
	(void) vm_deallocate(mach_task_self(), stack, vm_page_size);
	a48_kill_done = 1;
	return NULL;
}

static void
a48_wait(volatile int *done, unsigned ms)
{
	unsigned	i;

	for (i = 0; i < ms / 10 && (done == NULL || !*done); i++)
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 10);
}

static int
arm_forty_eight_hint_and_lock_order(void)
{
	pthread_t	self, sw, kill;

	if (pthread_create(&self, NULL, a48_self_loop, NULL) != 0) {
		printf("act_test: [48] pthread_create failed — WRONG\n");
		return 0;
	}
	while (a48_hint == MACH_PORT_NULL)
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 1);
	if (pthread_create(&sw, NULL, a48_switch_loop, NULL) != 0 ||
	    pthread_create(&kill, NULL, a48_kill_loop, NULL) != 0) {
		printf("act_test: [48] pthread_create failed — WRONG\n");
		a48_stop = a48_go = a48_kill_go = 1;
		return 0;
	}

	a48_go = 1;
	a48_wait(NULL, A48_MS_SELF);
	a48_kill_go = 1;
	a48_wait(&a48_kill_done, A48_MS_LIMIT);
	a48_stop = 1;
	a48_wait(&a48_switch_done, A48_MS_LIMIT / 4);
	a48_wait(&a48_self_done, A48_MS_LIMIT / 4);

	if (!a48_kill_done || !a48_switch_done || !a48_self_done) {
		printf("act_test: [48] hint against the lock order stopped: "
		       "after %u hints, %u mach_thread_self() and %u of %d "
		       "thread_terminate(), ended: killer %d, hint %d, self %d "
		       "— WRONG (#889)\n", a48_switches, a48_selfs, a48_kills,
		       A48_KILLS, a48_kill_done, a48_switch_done,
		       a48_self_done);
		return 0;
	}
	if (a48_kills != A48_KILLS || a48_switches == 0 || a48_selfs == 0) {
		printf("act_test: [48] %u hints, %u mach_thread_self() and %u of "
		       "%d thread_terminate(): the race was not run — WRONG\n",
		       a48_switches, a48_selfs, a48_kills, A48_KILLS);
		return 0;
	}
	printf("act_test: [48] %u hints against %u mach_thread_self() and %u "
	       "thread_terminate(), and all of it ended — OK (#889)\n",
	       a48_switches, a48_selfs, a48_kills);
	return 1;
}

/*
 * Arm forty-nine (#869): a port holds one reference on its subsystem, and gives
 * it back when it dies, once, however it dies.
 *
 * A port with a port-destroyed request does not die when it is destroyed: it
 * goes to its backup alive, and on the subsystem.  The release used to come
 * before that was known, so each such destroy released again, down to zero with
 * the creator's reference outstanding.  And the other ways to destroy a receive
 * right did not release at all.  The count is read at each step, and the
 * creator allocates on the subsystem after, through the trap, for cap_test's
 * reason in [10]: libmach's fallback would turn a refusal into a success on a
 * port with no subsystem.
 */
#define	A49_ROUNDS	3

static int
a49_count_is(mach_port_t subsys, unsigned want, const char *when)
{
	unsigned	have = 0;
	kern_return_t	kr;

	kr = urmach_subsystem_ref_count(subsys, &have);
	if (kr != KERN_SUCCESS || have != want) {
		printf("act_test: [49] %s: the count answered 0x%x, %u, where %u "
		       "was due — WRONG (#869)\n", when, kr, have, want);
		return 0;
	}
	return 1;
}

/* A port on the subsystem goes to its backup, comes back, and then dies. */
static int
a49_survives_a_destroy(mach_port_t subsys, unsigned base)
{
	struct {
		mach_msg_header_t		h;
		mach_msg_body_t			b;
		mach_msg_port_descriptor_t	d;
		unsigned char			rest[256];
	} n;
	mach_port_t		a = MACH_PORT_NULL, backup, old = MACH_PORT_NULL;
	mach_msg_return_t	mr;
	kern_return_t		kr;
	unsigned		i;
	int			ok = 1;

	backup = arm_own_port(49);
	if (backup == MACH_PORT_NULL)
		return 0;
	kr = mach_port_allocate_subsystem(mach_task_self(), subsys, &a);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [49] a port on the subsystem answered 0x%x — "
		       "WRONG\n", kr);
		(void) mach_port_destroy(mach_task_self(), backup);
		return 0;
	}

	for (i = 0; ok && i < A49_ROUNDS; i++) {
		kr = mach_port_request_notification(mach_task_self(), a,
						    MACH_NOTIFY_PORT_DESTROYED,
						    0, backup,
						    MACH_MSG_TYPE_MAKE_SEND_ONCE,
						    &old);
		if (kr != KERN_SUCCESS) {
			printf("act_test: [49] the request answered 0x%x — "
			       "WRONG\n", kr);
			ok = 0;
			break;
		}
		(void) mach_port_destroy(mach_task_self(), a);
		a = MACH_PORT_NULL;
		ok = a49_count_is(subsys, base + 1, "a port gone to its backup");
		mr = mach_msg(&n.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			      sizeof n, backup, 5000, MACH_PORT_NULL);
		if (mr != MACH_MSG_SUCCESS ||
		    n.h.msgh_id != MACH_NOTIFY_PORT_DESTROYED) {
			printf("act_test: [49] the backup got 0x%x, id %d — "
			       "WRONG\n", (int) mr, (int) n.h.msgh_id);
			ok = 0;
			break;
		}
		a = n.d.name;
	}
	if (a != MACH_PORT_NULL)
		(void) mach_port_destroy(mach_task_self(), a);
	(void) mach_port_destroy(mach_task_self(), backup);
	return ok;
}

/* The two ways that used not to release. */
static int
a49_other_deaths(mach_port_t subsys, unsigned base)
{
	struct {
		mach_msg_header_t		h;
		mach_msg_body_t			b;
		mach_msg_port_descriptor_t	d;
	} m;
	mach_port_t		a = MACH_PORT_NULL, q;
	mach_msg_return_t	mr;
	kern_return_t		kr;
	int			ok;

	kr = mach_port_allocate_subsystem(mach_task_self(), subsys, &a);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [49] a port on the subsystem answered 0x%x — "
		       "WRONG\n", kr);
		return 0;
	}
	(void) mach_port_mod_refs(mach_task_self(), a, MACH_PORT_RIGHT_RECEIVE,
				  -1);
	ok = a49_count_is(subsys, base, "after mach_port_mod_refs()");

	q = arm_own_port(49);
	if (q == MACH_PORT_NULL)
		return 0;
	kr = mach_port_allocate_subsystem(mach_task_self(), subsys, &a);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [49] a port on the subsystem answered 0x%x — "
		       "WRONG\n", kr);
		(void) mach_port_destroy(mach_task_self(), q);
		return 0;
	}
	m.h.msgh_bits = MACH_MSGH_BITS_COMPLEX |
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	m.h.msgh_size = sizeof m;
	m.h.msgh_remote_port = q;
	m.h.msgh_local_port = MACH_PORT_NULL;
	m.h.msgh_id = 49;
	m.b.msgh_descriptor_count = 1;
	m.d.name = a;
	m.d.disposition = MACH_MSG_TYPE_MOVE_RECEIVE;
	m.d.type = MACH_MSG_PORT_DESCRIPTOR;
	mr = mach_msg(&m.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m, 0,
		      MACH_PORT_NULL, 0, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS) {
		printf("act_test: [49] the send answered 0x%x — WRONG\n",
		       (int) mr);
		(void) mach_port_destroy(mach_task_self(), a);
		ok = 0;
	}
	(void) mach_port_destroy(mach_task_self(), q);
	return a49_count_is(subsys, base,
			    "after a destroyed message held the right") && ok;
}

static int
arm_forty_nine_subsystem_released_once(void)
{
	struct rpc_subsystem	desc;
	mach_port_t		subsys = MACH_PORT_NULL, probe = MACH_PORT_NULL;
	unsigned		i, base = 0;
	kern_return_t		kr;
	int			ok = 1;

	for (i = 0; i < sizeof desc; i++)
		((volatile char *) &desc)[i] = 0;
	desc.base_addr = (vm_address_t) &desc;
	kr = mach_subsystem_create(mach_task_self(), (user_subsystem_t) &desc,
				   (mach_msg_type_number_t) sizeof desc,
				   &subsys);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [49] mach_subsystem_create answered 0x%x — "
		       "WRONG\n", kr);
		return 0;
	}
	kr = urmach_subsystem_ref_count(subsys, &base);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [49] the count answered 0x%x — WRONG\n", kr);
		(void) mach_port_deallocate(mach_task_self(), subsys);
		return 0;
	}

	ok = a49_survives_a_destroy(subsys, base);

	/* The creator's reference has to be there, whatever the count says. */
	kr = syscall_mach_port_allocate_subsystem(mach_task_self(), subsys,
						  &probe);
	if (kr != KERN_SUCCESS) {
		printf("act_test: [49] the creator can no longer allocate on "
		       "its subsystem: 0x%x — WRONG (#869)\n", kr);
		(void) mach_port_deallocate(mach_task_self(), subsys);
		return 0;
	}
	(void) mach_port_destroy(mach_task_self(), probe);
	if (!ok) {
		(void) mach_port_deallocate(mach_task_self(), subsys);
		return 0;
	}

	ok = a49_count_is(subsys, base, "after the port died") &&
	    a49_other_deaths(subsys, base);
	(void) mach_port_deallocate(mach_task_self(), subsys);
	if (ok)
		printf("act_test: [49] a port's subsystem reference stayed "
		       "through %d destroys with a backup, and came back at its "
		       "death by each route: the count is %u again — OK (#869)\n",
		       A49_ROUNDS, base);
	return ok;
}

/*
 * Arm fifty-one (#868): a right given up is gone.
 *
 * One thread, and a fast RPC: a combined send and receive with COPY_SEND and
 * MAKE_SEND_ONCE, the shape of mach_msg's hot path.  Nobody serves the
 * destination, so the answer is put on the reply port first, by this thread.
 * After a few of those the right is given up, by each route that does not move
 * is_generation, and the RPC is asked again.  It must be refused.
 *
 * The one in the middle needs a second thread: a right deallocated between the
 * lock-free resolve's reading the name and its locking the port, in the window
 * ablations/868-widen-resolve.patch holds open, is what the resolve asks about
 * again with the ports locked.
 */
#define	A51_ID		5151	/* the message the widen waits for */
#define	A51_ROUNDS	6
#define	A51_NEW_PORTS	16

union a51_msg {
	mach_msg_header_t	h;
	unsigned char		b[256];
};

static volatile unsigned	a51_go, a51_done;
static volatile mach_port_t	a51_target;

static void
a51_zero(union a51_msg *m)
{
	unsigned	i;

	for (i = 0; i < sizeof *m; i++)
		m->b[i] = 0;
}

/* The answer an RPC will find, put there in advance. */
static void
a51_answer(mach_port_t reply, mach_msg_type_name_t how)
{
	union a51_msg	m;

	a51_zero(&m);
	m.h.msgh_bits = MACH_MSGH_BITS(how, 0);
	m.h.msgh_size = sizeof m.h;
	m.h.msgh_remote_port = reply;
	m.h.msgh_id = 50;
	(void) mach_msg(&m.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m.h, 0,
			MACH_PORT_NULL, 0, MACH_PORT_NULL);
}

static mach_msg_return_t
a51_rpc(mach_port_t dest, mach_port_t reply, int id)
{
	union a51_msg	m;

	a51_zero(&m);
	m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
				       MACH_MSG_TYPE_MAKE_SEND_ONCE);
	m.h.msgh_size = sizeof m.h;
	m.h.msgh_remote_port = dest;
	m.h.msgh_local_port = reply;
	m.h.msgh_id = id;
	return mach_msg(&m.h, MACH_SEND_MSG | MACH_RCV_MSG, sizeof m.h,
			sizeof m, reply, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

static void
a51_drain(mach_port_t port)
{
	union a51_msg	m;

	while (mach_msg(&m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof m,
			port, 0, MACH_PORT_NULL) == MACH_MSG_SUCCESS)
		mach_msg_destroy(&m.h);
}

/* A right into a message, which stays queued on `to'. */
static mach_msg_return_t
a51_move(mach_port_t to, mach_port_t what, mach_msg_type_name_t how)
{
	struct {
		mach_msg_header_t		h;
		mach_msg_body_t			b;
		mach_msg_port_descriptor_t	d;
	} m;

	m.h.msgh_bits = MACH_MSGH_BITS_COMPLEX |
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	m.h.msgh_size = sizeof m;
	m.h.msgh_remote_port = to;
	m.h.msgh_local_port = MACH_PORT_NULL;
	m.h.msgh_id = 51;
	m.b.msgh_descriptor_count = 1;
	m.d.name = what;
	m.d.disposition = how;
	m.d.type = MACH_MSG_PORT_DESCRIPTOR;
	return mach_msg(&m.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m, 0,
			MACH_PORT_NULL, 0, MACH_PORT_NULL);
}

/* Both rights work: two RPCs before one of them is given up. */
static int
a51_warm(mach_port_t dest, mach_port_t reply)
{
	mach_msg_return_t	mr;
	unsigned		i;

	for (i = 0; i < 2; i++) {
		a51_answer(reply, MACH_MSG_TYPE_MAKE_SEND);
		mr = a51_rpc(dest, reply, 51);
		if (mr != MACH_MSG_SUCCESS) {
			printf("act_test: [51] an RPC with both rights answered "
			       "0x%x — WRONG\n", (int) mr);
			return 0;
		}
	}
	return 1;
}

/* The same RPC after the right is gone, with its answer waiting. */
static int
a51_refused(mach_port_t dest, mach_port_t reply, const char *route)
{
	mach_msg_return_t	mr;

	a51_answer(reply, MACH_MSG_TYPE_MAKE_SEND);
	mr = a51_rpc(dest, reply, 51);
	printf("act_test: [51] %s: the RPC answered 0x%x\n", route, (int) mr);
	a51_drain(reply);
	if (mr != MACH_SEND_INVALID_DEST) {
		printf("act_test: [51] %s: a right given up went on working — "
		       "WRONG (#868)\n", route);
		return 0;
	}
	return 1;
}

static void *
a51_dealloc_thread(void *arg)
{
	unsigned	round;

	(void) arg;
	for (round = 1; round <= A51_ROUNDS; round++) {
		while (a51_go != round)
			(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE,
					     0);
		(void) mach_port_deallocate(mach_task_self(), a51_target);
		a51_done = round;
	}
	return NULL;
}

/* The route that is not a matter of order: deallocated during the resolve. */
static int
a51_during_the_resolve(mach_port_t reply)
{
	pthread_t		other;
	mach_msg_return_t	mr;
	mach_port_t		dest;
	unsigned		round;
	int			ok = 1;

	if (pthread_create(&other, NULL, a51_dealloc_thread, NULL) != 0) {
		printf("act_test: [51] pthread_create failed — WRONG\n");
		return 0;
	}
	for (round = 1; round <= A51_ROUNDS; round++) {
		dest = arm_own_port(51);
		if (dest == MACH_PORT_NULL)
			return 0;
		a51_target = dest;
		a51_answer(reply, MACH_MSG_TYPE_MAKE_SEND);
		a51_go = round;
		(void) a51_rpc(dest, reply, A51_ID);
		while (a51_done != round)
			(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE,
					     0);
		a51_answer(reply, MACH_MSG_TYPE_MAKE_SEND);
		mr = a51_rpc(dest, reply, 51);
		a51_drain(reply);
		if (mr != MACH_SEND_INVALID_DEST) {
			printf("act_test: [51] round %u: a right deallocated "
			       "during the resolve went on working, 0x%x — "
			       "WRONG (#868)\n", round, (int) mr);
			ok = 0;
		}
		(void) mach_port_destroy(mach_task_self(), dest);
	}
	(void) pthread_join(other, NULL);
	return ok;
}

/* The name turns dead, the port's memory is a new port, and the name is used. */
static int
a51_dead_name(mach_port_t reply)
{
	mach_port_t		fresh[A51_NEW_PORTS];
	mach_port_t		p, q;
	mach_port_status_t	status;
	mach_msg_type_number_t	n;
	mach_port_type_t	type = 0;
	unsigned		i, stray = 0;
	int			ok;

	p = arm_own_port(51);
	q = arm_own_port(51);
	if (p == MACH_PORT_NULL || q == MACH_PORT_NULL)
		return 0;
	if (a51_move(q, p, MACH_MSG_TYPE_MOVE_RECEIVE) != MACH_MSG_SUCCESS ||
	    !a51_warm(p, reply)) {
		printf("act_test: [51] the dead name could not be set up — "
		       "WRONG\n");
		return 0;
	}
	(void) mach_port_destroy(mach_task_self(), q);
	(void) mach_port_type(mach_task_self(), p, &type);
	for (i = 0; i < A51_NEW_PORTS; i++)
		fresh[i] = arm_own_port(51);

	ok = a51_refused(p, reply, "a name turned dead, its port's memory in use");
	for (i = 0; i < A51_NEW_PORTS; i++) {
		n = MACH_PORT_RECEIVE_STATUS_COUNT;
		if (fresh[i] != MACH_PORT_NULL &&
		    mach_port_get_attributes(mach_task_self(), fresh[i],
					     MACH_PORT_RECEIVE_STATUS,
					     (mach_port_info_t) &status,
					     &n) == KERN_SUCCESS &&
		    status.mps_msgcount != 0)
			stray++;
		if (fresh[i] != MACH_PORT_NULL)
			(void) mach_port_destroy(mach_task_self(), fresh[i]);
	}
	(void) mach_port_deallocate(mach_task_self(), p);
	if (stray != 0) {
		printf("act_test: [51] a dead name sent to %u port(s) nobody "
		       "named — WRONG (#868)\n", stray);
		return 0;
	}
	return ok;
}

static int
arm_fifty_one_rights_that_left(void)
{
	mach_port_t		task = mach_task_self();
	mach_port_t		reply, q, d, r;
	union a51_msg		m;
	mach_msg_return_t	mr;
	int			ok = 1;

	reply = arm_own_port(51);
	q = arm_own_port(51);
	d = arm_own_port(51);
	if (reply == MACH_PORT_NULL || q == MACH_PORT_NULL ||
	    d == MACH_PORT_NULL)
		return 0;

	/* MOVE_SEND of the last uref leaves a receive right. */
	if (!a51_warm(d, reply) ||
	    a51_move(q, d, MACH_MSG_TYPE_MOVE_SEND) != MACH_MSG_SUCCESS ||
	    !a51_refused(d, reply, "MOVE_SEND of the last uref"))
		ok = 0;
	a51_drain(d);

	/* ipc_kmsg_copyin_header() frees the entry of a moved destination. */
	r = arm_own_port(51);
	if (r == MACH_PORT_NULL ||
	    a51_move(q, r, MACH_MSG_TYPE_MOVE_RECEIVE) != MACH_MSG_SUCCESS ||
	    !a51_warm(r, reply))
		ok = 0;
	else {
		a51_zero(&m);
		m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND, 0);
		m.h.msgh_size = sizeof m.h;
		m.h.msgh_remote_port = r;
		m.h.msgh_id = 51;
		mr = mach_msg(&m.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
			      sizeof m.h, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
		if (mr != MACH_MSG_SUCCESS ||
		    !a51_refused(r, reply, "a destination moved in a header"))
			ok = 0;
	}

	ok = a51_dead_name(reply) && ok;
	ok = a51_during_the_resolve(reply) && ok;

	/*
	 * MOVE_RECEIVE on a send+receive entry leaves a send right.  What is
	 * asked of here is the reply, so the name to answer is the one that is
	 * left: a kernel that believed the old mapping would find that message.
	 */
	r = arm_own_port(51);
	(void) mach_port_insert_right(task, d, d, MACH_MSG_TYPE_MAKE_SEND);
	if (r == MACH_PORT_NULL || !a51_warm(d, r) ||
	    a51_move(q, r, MACH_MSG_TYPE_MOVE_RECEIVE) != MACH_MSG_SUCCESS) {
		ok = 0;
	} else {
		a51_answer(r, MACH_MSG_TYPE_COPY_SEND);
		mr = a51_rpc(d, r, 51);
		printf("act_test: [51] MOVE_RECEIVE of the reply: the RPC "
		       "answered 0x%x\n", (int) mr);
		if (mr != MACH_SEND_INVALID_REPLY) {
			printf("act_test: [51] a reply port given up went on "
			       "working — WRONG (#868)\n");
			ok = 0;
		}
	}

	(void) mach_port_destroy(task, q);
	(void) mach_port_destroy(task, d);
	(void) mach_port_destroy(task, reply);
	if (r != MACH_PORT_NULL)
		(void) mach_port_deallocate(task, r);
	if (ok)
		printf("act_test: [51] a right given up by each route was "
		       "refused on the next RPC — OK (#868)\n");
	return ok;
}

int
main(int argc, char **argv)
{
	kern_return_t	kr;
	int		passed = 0, arm_one_passed, arms = 19, r;

	(void) argc;
	(void) argv;

	printf("act_test: started (#475, #474)\n");

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&nap_port);
	if (kr != KERN_SUCCESS) {
		printf("act_test: no port to wait on (%d) — WRONG\n", kr);
		return 1;
	}

	arm_one_passed = arm_one_terminate_in_exception();
	passed += arm_one_passed;
	passed += arm_two_abort_in_mach_msg();
	passed += arm_three_suspend_in_mach_msg();
	passed += arm_four_state_of_a_thread_in_a_trap();
	passed += arm_five_registers_a_trap_leaves();
	passed += arm_seven_suspend_in_page_faults();

	/*
	 * An arm that was not asked is not an arm that passed: at more than
	 * one processor this one says NOT ASKED, and the count below is of the
	 * arms that were.
	 */
	r = arm_eight_suspend_a_spinner();
	if (r == ARM8_NOT_ASKED)
		arms--;
	else
		passed += r;

	passed += arm_nine_refused_create_running();
	passed += arm_ten_abort_an_rpc_with_its_reply_on_the_way();
	passed += arm_eleven_abort_a_thread_that_keeps_asking();
	passed += arm_twelve_look_at_the_reply_port();
	r = arm_thirteen_message_to_a_receiver_being_read();
	if (r == ARM13_NOT_ASKED)	/* not asked is not passed (arm eight) */
		arms--;
	else
		passed += r;
	passed += arm_fourteen_terminate_inside_traps();
	arms++;		/* arm twenty, #783: counted here, beside its call */
	r = arm_twenty_no_empty_activation();
	if (r < 0)
		arms--;
	else
		passed += r;
	arms++;		/* arm twenty-one, #786 */
	r = arm_twenty_one_set_limit_never_raises();
	if (r < 0)
		arms--;
	else
		passed += r;
	arms++;		/* arm twenty-three, #786: harmless even ablated */
	r = arm_twenty_three_float_state_is_what_the_thread_finds();
	if (r < 0)
		arms--;
	else
		passed += r;
	arms++;		/* arm twenty-four, #786: harmless even ablated */
	r = arm_twenty_four_initial_reads_initial();
	if (r < 0)
		arms--;
	else
		passed += r;
	arms++;		/* arm twenty-five, #786 */
	r = arm_twenty_five_fifo_rr_need_the_set();
	if (r < 0)
		arms--;
	else
		passed += r;

	/*
	 * Down here, after everything above has printed, for arm sixteen's
	 * reason: on a healthy tree it is refused and returns, but on the
	 * processor_exit ablation the action thread panics (#453).
	 */
	arms++;		/* arm twenty-two, #786 */
	r = arm_twenty_two_processor_ports_are_privileged();
	if (r < 0)
		arms--;
	else
		passed += r;
	arms++;		/* arm twenty-six, #771 */
	r = arm_twenty_six_a_receive_that_slept_keeps_six();
	if (r < 0)
		arms--;
	else
		passed += r;

	arms++;		/* arm twenty-seven, #808 */
	passed += arm_twenty_seven_send_size_cannot_take_the_trailer();
	arms++;		/* arm twenty-eight, #808 */
	passed += arm_twenty_eight_send_past_the_inline_limit();
	arms++;		/* arm twenty-nine, #808 */
	passed += arm_twenty_nine_receive_measured_with_its_trailer();
	arms++;		/* arm thirty, #808 */
	passed += arm_thirty_scatter_list_one_word_short();
	arms++;		/* arm thirty-one, #840 */
	passed += arm_thirty_one_descriptor_count_past_the_message();
	arms++;		/* arm thirty-two, #841 */
	passed += arm_thirty_two_exception_answered_too_large();
	arms++;		/* arm thirty-three, #842 */
	passed += arm_thirty_three_trailer_not_there();
	arms++;		/* arm thirty-four, #844 */
	passed += arm_thirty_four_rpc_reply_seqno();
	arms++;		/* arm thirty-five, #843 */
	passed += arm_thirty_five_port_array_with_no_room();
	arms++;		/* arm thirty-six, #810 */
	passed += arm_thirty_six_vm_write_small_physical_copy();
	arms++;		/* arm thirty-seven, #812 */
	passed += arm_thirty_seven_snapshot_of_untouched_memory();
	arms++;		/* arm thirty-eight, #813 */
	passed += arm_thirty_eight_task_wire_unprivileged();
	arms++;		/* arm thirty-nine, #777 */
	passed += arm_thirty_nine_lost_bet_undone();
	arms++;		/* arm forty, #777 */
	passed += arm_forty_nothing_after_the_hand_off();
	arms++;		/* arm forty-one, #867 */
	passed += arm_forty_one_body_disposition();
	arms++;		/* arm forty-two, #870 */
	passed += arm_forty_two_simple_message_scattered();
	arms++;		/* arm forty-three, #871 */
	passed += arm_forty_three_receive_gives_back();
	arms++;		/* arm forty-four, #880 */
	passed += arm_forty_four_index_zero_name();
	arms++;		/* arm forty-five, #881 */
	r = arm_forty_five_parked_sender();
	if (r < 0)
		arms--;
	else
		passed += r;
	arms++;		/* arm forty-six, #887 */
	passed += arm_forty_six_exception_flavor();
	arms++;		/* arm forty-seven, #887 */
	r = arm_forty_seven_failed_state_gives_back();
	if (r < 0)
		arms--;
	else
		passed += r;
	arms++;		/* arm fifty, #851 */
	passed += arm_fifty_holes_come_back_zero();
	arms++;		/* arm forty-eight, #889 */
	passed += arm_forty_eight_hint_and_lock_order();
	arms++;		/* arm forty-nine, #869 */
	passed += arm_forty_nine_subsystem_released_once();
	arms++;		/* arm fifty-one, #868 */
	passed += arm_fifty_one_rights_that_left();

	/*
	 * Before arm six, and after everything above has printed: on a healthy
	 * tree this arm only asks a trap and an RPC to refuse and returns, but on
	 * the #752 ablation it provokes the same kind of machine stop arm six
	 * does, so it belongs down here beside it.
	 */
	passed += arm_sixteen_name_not_copied_out();

	passed += arm_fifteen_kernel_task_needs_privilege();
	passed += arm_nineteen_last_page_unmapped();

	/*
	 * ⚠️ Last, and not by accident.  On a healthy tree this is the only arm
	 * that can stop the machine: it makes the kernel take a fault on its own
	 * return instruction, and if that fault is not attributed to the thread
	 * that caused it, nothing after this line runs.  Everything the other
	 * arms have to say is already printed by the time it is provoked.
	 */
	passed += arm_six_non_canonical_return();

	/*
	 * And the two IRETQ returns after it (#780), for the same reason: each
	 * can stop the machine on a kernel without the fix, and does nothing to
	 * one with it.
	 */
	passed += arm_seventeen_start_at_a_noncanonical_rip();
	passed += arm_eighteen_stopped_in_a_trap();

	/*
	 * The last thing arm one is owed, asked now that the two arms after it
	 * have kept the machine busy for a while: a terminated thread must not
	 * have run any of its own code again.
	 */
	if (arm_one_thread_ran_on) {
		printf("act_test: [1] the terminated thread resumed past its "
		       "fault after all — WRONG\n");
		/* Once: an arm that already failed is not failed again. */
		if (arm_one_passed)
			passed--;
	}

	printf("act_test: %d of %d arms passed\n", passed, arms);

	/*
	 * 🔴 IT ENDS -- see the note at the end of fault_test's main for why
	 * the reason that used to be here was never true (#513).
	 *
	 * ⚠️ This said exit() for one commit, because arm one KILLS a thread
	 * with thread_terminate and does not join it, so libpthreads' count of
	 * live threads never comes back down and a `return' waited to be joined
	 * by a thread the kernel had destroyed.  That was treating the symptom:
	 * a return from main() is exit() by definition, and it is crt0's path
	 * that had to say so.  It does now, so this is a plain return again --
	 * which is what every other program in the bundle writes.
	 */
	return passed == arms ? 0 : 1;
}
