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
#include <mach/mig_errors.h>

#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "exc_server.h"

/*
 * The two provocations, in fperr_provoke_<arch>.S beside this file.
 *
 * Each returns the unit's own status register as it stood after the division:
 * the x87 status word, and MXCSR.  🔑 Only reached when the processor did NOT
 * report the error -- and that is exactly the case where a bare "no exception
 * arrived" explains nothing.  A flag set with its mask clear says the unit
 * flagged it and nothing carried it; the mask still set says the unmasking
 * never survived to the division.  Two different defects, one silence.
 */
extern unsigned int	fperr_provoke_x87(void);
extern unsigned int	fperr_provoke_sse(void);

/* CPUID leaf 0x40000000: twelve characters of hypervisor signature. */
extern void		fperr_cpuid_hv(unsigned int *out3);

/*
 * 🔴 Under TCG an unmasked SIMD zero-divide sets ZE in MXCSR and the processor
 * does NOT raise #XF.  Under KVM, the same kernel, it does -- measured both
 * ways while #515 was being written.  So the SIMD arm cannot pass on the
 * emulator, and a test that called that a kernel defect on every default run
 * would be teaching its reader to ignore it.
 *
 * ⚠️ Reported as SKIPPED and never as PASS, and guarded twice: the unit must
 * have FLAGGED the error (ZE set, ZM clear), which is everything up to the
 * processor's own decision having worked.  If the flag is not there, the
 * unmasking or the division is at fault and the arm stays a failure.
 */
static int
running_under_tcg(void)
{
	unsigned int	sig[3];

	fperr_cpuid_hv(sig);
	return memcmp(sig, "TCGTCGTCGTCG", 12) == 0;
}

enum arm { ARM_X87, ARM_SSE };

static enum arm		the_arm;
static mach_port_t	exc_port;

static volatile int		exception_count;
static volatile int		exception_type;
static volatile natural_t	exception_code0;
static volatile int		victim_returned;
static volatile unsigned int	victim_status;

static void *
the_thread_that_divides(void *arg)
{
	(void) arg;

	if (the_arm == ARM_X87)
		victim_status = fperr_provoke_x87();
	else
		victim_status = fperr_provoke_sse();

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

/*
 * What run_the_arm() answers.  Three outcomes and not two: a skip is not a
 * pass, and calling it one would be the instrument lying in the quiet
 * direction — the direction this project keeps having to correct.
 */
#define ARM_FAILED	0
#define ARM_PASSED	1
#define ARM_SKIPPED	2

static const char *
arm_name(void)
{
	return the_arm == ARM_X87 ? "x87" : "SIMD";
}

/*
 * Receive ONE exception message, or say that none came.
 *
 * 🔴 Hand-written rather than mach_msg_server_once(), and the reason is the
 * only reason worth writing one: that routine passes MACH_MSG_TIMEOUT_NONE to
 * the trap, so a kernel that delivers no exception leaves this program blocked
 * for ever with "starting" as its last word.  Which is exactly what the first
 * run of this test did on x86-64 -- where no exception is EXPECTED, because
 * CR0.NE is clear -- and a hang is not a result.  "No exception arrived" is a
 * finding, and an instrument that cannot report it cannot be trusted when it
 * reports anything else.
 *
 * ⚠️ The reply has to be sent.  A thread suspended in exception_raise() waits
 * for it, and the x87 arm's second half is whether that thread comes back.
 */
#define RECEIVE_MS	3000

static mach_msg_return_t
receive_one_exception(void)
{
	union {
		mach_msg_header_t	hdr;
		mig_reply_error_t	err;
		char			pad[4096];
	} req, rep;
	mach_msg_return_t	mr;

	mr = mach_msg(&req.hdr, MACH_RCV_MSG|MACH_RCV_TIMEOUT, 0,
		      (mach_msg_size_t) sizeof req, exc_port,
		      RECEIVE_MS, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS)
		return mr;

	(void) exc_server(&req.hdr, &rep.hdr);

	if (rep.hdr.msgh_remote_port == MACH_PORT_NULL)
		return MACH_MSG_SUCCESS;

	return mach_msg(&rep.hdr, MACH_SEND_MSG, rep.hdr.msgh_size, 0,
			MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

static int
run_the_arm(void)
{
	pthread_t		victim;
	kern_return_t		kr;
	mach_msg_return_t	mr;
	int			want_code;
	int			i;

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &exc_port);
	if (kr != KERN_SUCCESS) {
		printf("fperr_test: mach_port_allocate failed (%d) — WRONG\n", kr);
		return ARM_FAILED;
	}
	kr = mach_port_insert_right(mach_task_self(), exc_port, exc_port,
				    MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("fperr_test: insert_right failed (%d) — WRONG\n", kr);
		return ARM_FAILED;
	}

	kr = task_set_exception_ports(mach_task_self(), EXC_MASK_ARITHMETIC,
				      exc_port, EXCEPTION_DEFAULT,
				      THREAD_STATE_NONE);
	if (kr != KERN_SUCCESS) {
		printf("fperr_test: task_set_exception_ports failed (%d) — "
		       "WRONG\n", kr);
		return ARM_FAILED;
	}

	/*
	 * ⚠️ A second thread, and not because the fault needs one.  A thread
	 * that faults is suspended inside exception_raise() waiting for its
	 * handler to answer, so a program that faulted on its only thread
	 * would have nobody left to receive the message it had just sent.
	 */
	if (pthread_create(&victim, NULL, the_thread_that_divides, NULL) != 0) {
		printf("fperr_test: pthread_create failed — WRONG\n");
		return ARM_FAILED;
	}

	/*
	 * One message, and then stop asking.  A loop would never come back:
	 * the SIMD arm's thread raises a second exception by construction.
	 */
	mr = receive_one_exception();

	if (exception_count == 0) {
		/*
		 * 🔑 The verdict is decided BEFORE anything is printed, and
		 * that ordering is not cosmetic: the run harness judges a boot
		 * by grepping its log for WRONG, so a line saying WRONG
		 * followed by a line explaining that this is expected reads as
		 * a failure to everything that reads logs rather than people.
		 */
		int	excused = (the_arm == ARM_SSE &&
				   (victim_status & 0x204) == 0x004 &&
				   victim_returned &&
				   running_under_tcg());

		printf("fperr_test: [%s] no exception arrived in %d ms (%s)"
		       " — %s\n",
		       arm_name(), RECEIVE_MS,
		       (mr == MACH_RCV_TIMED_OUT)
		       ? "the receive timed out" : "the receive failed",
		       excused ? "SKIPPED, see below" : "WRONG");

		if (!victim_returned) {
			printf("fperr_test: [%s] and the dividing thread has "
			       "not come back either, so it is somewhere this "
			       "program cannot see\n", arm_name());
			return ARM_FAILED;
		}
		/*
		 * 🔑 The absence with a value behind it.  The unit's own
		 * register says which of the two silences this is.
		 */
		if (the_arm == ARM_X87)
			printf("fperr_test: [%s] the dividing thread carried "
			       "on, and the x87 status word it left is 0x%04x "
			       "— %s\n", arm_name(), victim_status,
			       (victim_status & 0x04)
			       ? "ZE IS SET: the unit flagged the error and "
				 "nothing reported it, which is CR0.NE clear"
			       : "ZE is clear: the division never faulted at "
				 "all, so the unmasking did not take");
		else
			printf("fperr_test: [%s] the dividing thread carried "
			       "on, and the MXCSR it left is 0x%04x — %s\n",
			       arm_name(), victim_status,
			       (victim_status & 0x200)
			       ? "ZM is STILL SET: the unmasking did not "
				 "survive to the division"
			       : ((victim_status & 0x04)
				  ? "ZE is set and ZM is clear: the unit "
				    "flagged the error and the processor did "
				    "not raise #XF"
				  : "ZE is clear with ZM clear: the division "
				    "did not divide by zero"));

		if (excused) {
			printf("fperr_test: [%s] and CPUID 0x40000000 says "
			       "TCG, which does not raise #XF for an unmasked "
			       "SIMD exception — SKIPPED, not passed: run it "
			       "under -enable-kvm to ask the question\n",
			       arm_name());
			return ARM_SKIPPED;
		}
		return ARM_FAILED;
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
		return ARM_FAILED;

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
		return ARM_PASSED;
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
		return ARM_FAILED;
	}

	printf("fperr_test: [%s] and the thread resumed past the wait that "
	       "faulted, on one exception\n", arm_name());
	return ARM_PASSED;
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

	printf("fperr_test: %s arm %s\n", arm_name(),
	       (ok == ARM_PASSED) ? "PASS"
	       : (ok == ARM_SKIPPED) ? "SKIPPED" : "FAIL");
	return (ok == ARM_FAILED) ? 1 : 0;
}
