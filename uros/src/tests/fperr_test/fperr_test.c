/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * fperr_test — does an arithmetic fault reach the thread that caused it (#515)?
 *
 * #515 is about the two ways a processor reports that a floating-point
 * operation went wrong, and about the fact that each of this project's targets
 * was losing the one the other delivered.  The question only has an honest
 * answer at the far end of the whole route -- processor, IDT, trap, exception,
 * message, port -- so this asks it from a task, with an exception port of its
 * own, and reports what the kernel named rather than what this program
 * expected.
 *
 * ── Why one arm per run ─────────────────────────────────────────────────
 *
 * 🔴 The two arms cannot share a process, and the reason is the SIMD arm.
 * #XF is a precise fault, so a thread the handler answers goes straight back
 * into the division and raises it again; that thread then parks for ever
 * inside exception_raise() waiting for a reply nobody is left to send.  Which
 * is harmless -- until a second arm in the same task calls
 * mach_msg_server_once() and receives THAT message instead of its own, and
 * reports the first arm's fault as the second arm's answer.
 *
 * So the arm is an argument, the bundle names the program twice, and each run
 * has exactly one thing to say.  A parked thread is then the task's problem
 * for the few milliseconds before it exits, and nobody else's.
 *
 * ── What each arm expects, and why they differ ──────────────────────────
 *
 * x87:  the exception arrives AND the faulting routine comes back.  It comes
 *       back only if the kernel cleared the reported condition in the thread's
 *       saved state -- otherwise the `fwait' it was resumed at faults again.
 *       Both halves are required, and the second is the one nothing has ever
 *       checked.
 *
 * SIMD: the exception arrives.  The routine is NOT expected to return, because
 *       a precise fault re-executes; saying so here is the difference between
 *       a known consequence and an unexplained hang.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/message.h>
#include <mach/exception.h>
#include <mach/task_special_ports.h>
#include <mach/thread_switch.h>

/*
 * ⚠️ mach_host.h and not mach.h: task_set_exception_ports is generated from
 * mach_host.defs, and the umbrella header does not pull that half in.  Without
 * this the call compiles as an implicit declaration returning int.
 */
#include <mach/mach_host.h>

#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "exc_server.h"

/* The two provocations, in fperr_provoke_<arch>.S beside this file. */
extern void	fperr_provoke_x87(void);
extern void	fperr_provoke_sse(void);

enum arm { ARM_X87, ARM_SSE };

static enum arm		the_arm;
static mach_port_t	exc_port;

static volatile int		exception_count;
static volatile int		exception_type;
static volatile natural_t	exception_code0;
static volatile int		victim_returned;

static void *
the_thread_that_divides(void *arg)
{
	(void) arg;

	if (the_arm == ARM_X87)
		fperr_provoke_x87();
	else
		fperr_provoke_sse();

	victim_returned = 1;
	return NULL;
}

/*
 * What exc_server calls when the message arrives.  MIG demands this name and
 * this shape.
 *
 * 🔑 It answers KERN_SUCCESS and repairs NOTHING, deliberately.  A handler
 * that patched the thread's floating-point state would make both arms pass on
 * a kernel that left the condition armed, because the repair would hide
 * exactly the defect the x87 arm exists to find.  The kernel is supposed to
 * hand back a thread that can run; whether it does is the question.
 */
kern_return_t
catch_exception_raise(mach_port_t exception_port, mach_port_t thread,
		      mach_port_t task, int exception,
		      exception_data_t code, mach_msg_type_number_t codeCnt)
{
	(void) exception_port;
	(void) thread;
	(void) task;

	exception_type = exception;
	exception_code0 = (codeCnt > 0) ? code[0] : 0;
	exception_count++;

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

/*
 * The subcode the kernel is expected to name.  It is machine-dependent by
 * definition -- it says WHICH fault, and the two architectures number their
 * faults differently -- so it is the one thing in this file that forks.
 *
 * ⚠️ These are read from <mach/machine/exception.h> rather than written out,
 * so that a change to the table is a compile error here instead of a test that
 * quietly starts comparing against a number nobody maintains.
 */
#if defined(__x86_64__)
#define EXPECT_X87	EXC_X86_64_EXTERRFLT
#define EXPECT_SSE	EXC_X86_64_SSEFLT
#else
#define EXPECT_X87	EXC_I386_EXTERRFLT
#define EXPECT_SSE	EXC_I386_SSEFLT
#endif

static const char *
arm_name(void)
{
	return the_arm == ARM_X87 ? "x87" : "SIMD";
}

static int
run_the_arm(void)
{
	pthread_t	victim;
	kern_return_t	kr;
	int		want_code;
	int		i;

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &exc_port);
	if (kr != KERN_SUCCESS) {
		printf("fperr_test: mach_port_allocate failed (%d) — WRONG\n", kr);
		return 0;
	}
	kr = mach_port_insert_right(mach_task_self(), exc_port, exc_port,
				    MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("fperr_test: insert_right failed (%d) — WRONG\n", kr);
		return 0;
	}

	kr = task_set_exception_ports(mach_task_self(), EXC_MASK_ARITHMETIC,
				      exc_port, EXCEPTION_DEFAULT,
				      THREAD_STATE_NONE);
	if (kr != KERN_SUCCESS) {
		printf("fperr_test: task_set_exception_ports failed (%d) — "
		       "WRONG\n", kr);
		return 0;
	}

	/*
	 * ⚠️ A second thread, and not because the fault needs one.  A thread
	 * that faults is suspended inside exception_raise() waiting for its
	 * handler to answer, so a program that faulted on its only thread
	 * would have nobody left to receive the message it had just sent.
	 */
	if (pthread_create(&victim, NULL, the_thread_that_divides, NULL) != 0) {
		printf("fperr_test: pthread_create failed — WRONG\n");
		return 0;
	}

	/*
	 * One message, and then stop asking.  A loop would never come back:
	 * the SIMD arm's thread raises a second exception by construction.
	 */
	(void) mach_msg_server_once(exc_server, 4096, exc_port,
				    MACH_MSG_OPTION_NONE);

	if (exception_count == 0) {
		printf("fperr_test: [%s] no exception arrived%s — WRONG\n",
		       arm_name(),
		       victim_returned
		       ? ", and the thread carried on as if nothing had"
			 " happened"
		       : "");
		return 0;
	}

	want_code = (the_arm == ARM_X87) ? EXPECT_X87 : EXPECT_SSE;

	printf("fperr_test: [%s] an unmasked zero-divide reached the task's "
	       "exception port as exception %d code %u%s\n",
	       arm_name(), exception_type, (unsigned) exception_code0,
	       (exception_type == EXC_ARITHMETIC &&
		(int) exception_code0 == want_code)
	       ? " — EXC_ARITHMETIC, named by the kernel and not by this program"
	       : " — WRONG, that is not the fault this arm raised");

	if (exception_type != EXC_ARITHMETIC ||
	    (int) exception_code0 != want_code)
		return 0;

	if (the_arm == ARM_SSE) {
		/*
		 * 🔑 Nothing to wait for.  #XF is precise, so the thread the
		 * handler answered went back into the same division; it is
		 * parked inside its second exception_raise() and this task is
		 * about to exit out from under it.  That is the expected end
		 * of this arm, said out loud so a reader does not go looking
		 * for the line that never comes.
		 */
		printf("fperr_test: [%s] and the faulting thread stays where a "
		       "precise fault leaves it — expected\n", arm_name());
		return 1;
	}

	/*
	 * The x87 arm's second half.  Bounded, because a thread that does not
	 * come back is the failure this is looking for and must not become a
	 * hang: twenty turns of 50 ms is a second, against a routine whose
	 * remaining work is two instructions.
	 */
	for (i = 0; i < 20 && !victim_returned; i++)
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 50);

	if (!victim_returned) {
		printf("fperr_test: [%s] but the thread never came back: the "
		       "reported condition was left armed in its saved state, "
		       "so the wait it was resumed at faulted again "
		       "(%d exceptions seen) — WRONG\n",
		       arm_name(), exception_count);
		return 0;
	}

	printf("fperr_test: [%s] and the thread resumed past the wait that "
	       "faulted, on one exception\n", arm_name());
	return 1;
}

int
main(int argc, char **argv)
{
	int	ok;

	if (argc > 1 && strcmp(argv[1], "sse") == 0)
		the_arm = ARM_SSE;
	else if (argc <= 1 || strcmp(argv[1], "x87") == 0)
		the_arm = ARM_X87;
	else {
		printf("fperr_test: usage: fperr_test [x87|sse]\n");
		return 2;
	}

	printf("fperr_test: %s arm starting\n", arm_name());

	ok = run_the_arm();

	printf("fperr_test: %s arm %s\n", arm_name(), ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
