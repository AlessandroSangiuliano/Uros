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
	 * ⚠️ And then ask the kernel about the thread again, HERE, while the
	 * port is still in hand -- MIG deallocates it as soon as this returns.
	 *
	 * Because "thread_terminate answered KERN_SUCCESS" is the kernel
	 * agreeing with itself, and this issue is precisely about a path where
	 * the answer and the state came apart.  thread_suspend() on a live
	 * activation returns KERN_SUCCESS and on a dead one returns
	 * KERN_TERMINATED, so the two outcomes are different VALUES rather
	 * than the presence or absence of one -- which is the only shape of
	 * observation that can tell the kill from a kill that did nothing.
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
	 * ⚠️ Two answers are right here and one is wrong, and which two took a
	 * run to learn.
	 *
	 * KERN_TERMINATED is what an activation marked inactive answers.
	 * KERN_INVALID_ARGUMENT is what the NAME answers once the activation
	 * behind it is gone entirely -- which is what this kernel does, because
	 * the thread was the last one on its shuttle and the whole thing was
	 * destroyed rather than merely disabled.  The second is the stronger
	 * of the two.
	 *
	 * KERN_SUCCESS is the one that would mean nothing had happened, and it
	 * is the reason this is asked at all: what makes the pair a proof is
	 * that the SAME name answered KERN_SUCCESS to thread_terminate a
	 * moment ago.  One name, two different values, in that order.
	 */
	if (arm_one_after_kr != KERN_TERMINATED
	    && arm_one_after_kr != KERN_INVALID_ARGUMENT) {
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
	       "terminated, the kernel now calls it KERN_TERMINATED, and the "
	       "task is still here to say so\n");
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
 * there or not.  Measured, not assumed: with -DABLATE_411_CANONICAL=1 the arm
 * still reported six of six on an AMD model, and the same ablated kernel under
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
 * processor's misbehaviour.  Because the guard diverts before SYSRET ever
 * runs, the outcome is the same on AMD, on Intel, under TCG and under KVM.
 * An arm that needed the vendor difference would be an arm that only ran on
 * the machine nobody has.
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

int
main(int argc, char **argv)
{
	kern_return_t	kr;
	int		passed = 0;

	(void) argc;
	(void) argv;

	printf("act_test: started (#475, #474)\n");

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&nap_port);
	if (kr != KERN_SUCCESS) {
		printf("act_test: no port to wait on (%d) — WRONG\n", kr);
		return 1;
	}

	passed += arm_one_terminate_in_exception();
	passed += arm_two_abort_in_mach_msg();
	passed += arm_three_suspend_in_mach_msg();
	passed += arm_four_state_of_a_thread_in_a_trap();
	passed += arm_five_registers_a_trap_leaves();

	/*
	 * ⚠️ Last, and not by accident.  This is the only arm that can stop the
	 * machine: it makes the kernel take a fault on its own return
	 * instruction, and if that fault is not attributed to the thread that
	 * caused it, nothing after this line runs.  Everything the other arms
	 * have to say is already printed by the time it is provoked.
	 */
	passed += arm_six_non_canonical_return();

	/*
	 * The last thing arm one is owed, asked now that the two arms after it
	 * have kept the machine busy for a while: a terminated thread must not
	 * have run any of its own code again.
	 */
	if (arm_one_thread_ran_on) {
		printf("act_test: [1] the terminated thread resumed past its "
		       "fault after all — WRONG\n");
		passed--;
	}

	printf("act_test: %d of 6 arms passed\n", passed);

	/*
	 * ⚠️ Does not exit.  There is no proc server on this target to reap a
	 * task and nothing waits for this one, so returning would take the
	 * last thread out from under a program whose output the boot log is
	 * still the record of.  It stops here on purpose.
	 */
	for (;;)
		nap(1000);

	return 0;
}
