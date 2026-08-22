/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The three arms of #467, each asked of a real task (#467).
 *
 * #467 is where a fault stops being a halt and becomes something the kernel
 * does on behalf of a thread.  It has three arms, and the issue says why they
 * could not be written before #422: each of them needs a `vm_map_t' that comes
 * from a thread, and until bootstrap ran as init there was no thread to have
 * one.  The ring-3 probe in syscall/probe.S is not a substitute -- it is a page
 * and a stack pointer entered with iretq, with no task around it.
 *
 * So this is a task.  It runs as a bundle entry beside the name server, prints
 * one line per arm, and each line is a value rather than a verdict wherever a
 * value says more.
 *
 * ⚠️ The three are deliberately in this order, because each one that passes is
 * a precondition for reading the next honestly: a program that cannot fault on
 * its own memory and carry on cannot be trusted to report anything at all.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/message.h>
#include <mach/exception.h>
#include <mach/task_special_ports.h>
#include <mach/vm_prot.h>

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
 * What ring 3 writes and reads back, and it is a pattern rather than a flag.
 *
 * A page arriving from vm_allocate is zero, so a test that stored 1 and found
 * 1 would also pass on a kernel that never let the store happen and handed
 * back a different zero page.  A value that could not have been there by
 * accident is the difference between "the store landed" and "something is
 * readable here".
 */
#define WITNESS		0x5eeded1eafULL

/*
 * Where the second arm faults, and why this address.
 *
 * The lower half is the task's to have and this task does not have this piece
 * of it: nothing has been vm_allocate'd here, so the fault is a real
 * KERN_INVALID_ADDRESS rather than a protection question, and EXC_BAD_ACCESS
 * is what the task's handler must be told.
 *
 * ⚠️ Not a null pointer, and not a kernel address.  Zero is where a hundred
 * other defects also land, so a report naming zero would not distinguish this
 * arm from any of them; and an address in the upper half would be refused by
 * the processor before the kernel had an opinion, which tests the hardware
 * rather than #467.
 */
#define UNOWNED		((vm_offset_t) 0x0000200000000000ULL)

static mach_port_t	exc_port;
static volatile int	exception_seen;
static volatile int	exception_type;
static volatile natural_t exception_code0;

/*
 * ── Arm one: a fault on memory the task owns and has never touched ────
 *
 * vm_allocate maps a range and populates nothing; the first store into it is a
 * page fault that vm_fault() is supposed to resolve, after which the very same
 * instruction runs again and succeeds.  Before #467 this halted the machine.
 *
 * Two pages rather than one, because the second is what the third arm needs
 * untouched, and allocating them together is what makes them adjacent.
 */
static vm_address_t	region;
static vm_size_t	region_size;

static int
arm_one_fault_and_resume(void)
{
	volatile unsigned long long	*p;
	kern_return_t			kr;

	region_size = 2 * vm_page_size;
	region = 0;

	kr = vm_allocate(mach_task_self(), &region, region_size, TRUE);
	if (kr != KERN_SUCCESS) {
		printf("fault_test: [1] vm_allocate failed (%d) — WRONG\n", kr);
		return 0;
	}

	/*
	 * The fault.  Nothing here checks for it: if it is not resolved this
	 * store never completes and there is no line after this one, which is
	 * itself the report on a kernel that cannot do it.
	 */
	p = (volatile unsigned long long *) region;
	*p = WITNESS;

	if (*p != WITNESS) {
		printf("fault_test: [1] wrote 0x%llx, read 0x%llx — WRONG\n",
		       (unsigned long long) WITNESS, (unsigned long long) *p);
		return 0;
	}

	printf("fault_test: [1] a fault on an unmapped-but-valid page resolved "
	       "and the instruction resumed, leaving 0x%llx\n",
	       (unsigned long long) *p);
	return 1;
}

/*
 * ── Arm three: the kernel faults, in ring 0, on the task's memory ─────
 *
 * The kernel copies a message out of the sender's buffer with one copyinmsg()
 * of the whole send_size (ipc/ipc_kmsg.c).  So a message whose header sits on a
 * page this task has touched and whose tail runs onto a page it has NOT is a
 * copyin, in ring 0, across a mapping that is real and not yet resident.
 *
 * That is the arm the issue words as "copyin on a user page that is mapped but
 * not resident succeeds instead of returning an error", and it is the one with
 * the sharpest failure: before #467 the fault-recovery table answered first,
 * the copy returned an error, and mach_msg told the caller the task did not
 * have memory it does in fact have.
 *
 * ⚠️ The tail is never written by this program.  A page becomes resident by
 * being touched, so a test that filled the tail with a pattern would destroy
 * the very condition it is testing.  Zero is therefore the only content it can
 * have, and zero is what the receive checks for -- which is sound here because
 * the receive buffer is scribbled with a non-zero pattern first, so zeros in it
 * afterwards came from the message and not from never having been written.
 */
#define SCRIBBLE	0xA5

static int
arm_three_copyin_not_resident(void)
{
	mach_port_t		port = MACH_PORT_NULL;
	mach_msg_header_t	*msg;
	unsigned char		*rcv;
	mach_msg_size_t		send_size;
	vm_address_t		rcv_region = 0;
	kern_return_t		kr;
	mach_msg_return_t	mr;
	unsigned int		i;
	unsigned int		zeros;

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &port);
	if (kr != KERN_SUCCESS) {
		printf("fault_test: [3] mach_port_allocate failed (%d) — WRONG\n",
		       kr);
		return 0;
	}
	(void) mach_port_insert_right(mach_task_self(), port, port,
				      MACH_MSG_TYPE_MAKE_SEND);

	/*
	 * The header goes near the end of the first page -- the one arm one
	 * touched -- so that a message a little larger than what is left runs
	 * onto the second, which nothing has touched.
	 */
	send_size = (mach_msg_size_t) (vm_page_size / 2);
	msg = (mach_msg_header_t *) (region + vm_page_size - (vm_page_size / 4));

	msg->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	msg->msgh_size = send_size;
	msg->msgh_remote_port = port;
	msg->msgh_local_port = MACH_PORT_NULL;
	msg->msgh_id = 0x467;

	/* A buffer to receive into, scribbled so that zeros in it mean something. */
	kr = vm_allocate(mach_task_self(), &rcv_region,
			 (vm_size_t) (2 * vm_page_size), TRUE);
	if (kr != KERN_SUCCESS) {
		printf("fault_test: [3] vm_allocate failed (%d) — WRONG\n", kr);
		return 0;
	}
	rcv = (unsigned char *) rcv_region;
	for (i = 0; i < 2 * vm_page_size; i++)
		rcv[i] = SCRIBBLE;

	mr = mach_msg(msg, MACH_SEND_MSG, send_size, 0,
		      MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS) {
		printf("fault_test: [3] the send returned 0x%x — WRONG, the "
		       "kernel could not copy in a page the task owns\n", mr);
		return 0;
	}

	mr = mach_msg((mach_msg_header_t *) rcv, MACH_RCV_MSG, 0,
		      (mach_msg_size_t) (2 * vm_page_size), port,
		      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS) {
		printf("fault_test: [3] the receive returned 0x%x — WRONG\n", mr);
		return 0;
	}

	/*
	 * Count the tail that came from the untouched page.  It has to be
	 * zeros, and it has to be the right number of them: a copy that stopped
	 * at the page boundary would leave the scribble behind it.
	 */
	zeros = 0;
	for (i = sizeof(mach_msg_header_t); i < send_size; i++)
		if (rcv[i] == 0)
			zeros++;

	printf("fault_test: [3] a message whose tail lay on an untouched page "
	       "copied in whole: %u of %u bytes arrived, %u zero%s\n",
	       (unsigned) send_size, (unsigned) send_size, zeros,
	       (zeros == send_size - sizeof(mach_msg_header_t))
	       ? " — the whole tail" : " — WRONG, the copy stopped short");

	return zeros == send_size - sizeof(mach_msg_header_t);
}

/*
 * ── Arm two: a fault on memory the task does NOT own ──────────────────
 *
 * vm_fault() will refuse this one, and refusing is the right answer: the arm
 * is about what happens next.  The machine-independent exception() path is
 * supposed to find the port the task named and send it a message describing
 * the fault, so that a debugger or a personality server hears about it instead
 * of the machine stopping.
 *
 * ⚠️ Two threads, and not because the fault needs one.  A thread that faults
 * is suspended inside exception_raise() waiting for its handler to answer, so
 * a program that faulted on its only thread would have nobody left to receive
 * the message it just sent.  The faulting thread is therefore the disposable
 * one and the main thread is the handler.
 */
static volatile int	victim_resumed;

static void *
the_thread_that_faults(void *arg)
{
	volatile unsigned long *p = (volatile unsigned long *) UNOWNED;
	unsigned long		got;

	(void) arg;

	/*
	 * A read rather than a write, so that a kernel which resolved this by
	 * accident would have to have invented a mapping rather than merely
	 * been generous about permissions.
	 *
	 * It does not come back until the handler below has made the address
	 * good and answered: that round trip IS the arm.
	 */
	got = *p;

	victim_resumed = 1;
	printf("fault_test: [2] and the thread resumed at the instruction that "
	       "faulted, reading 0x%lx from the page its handler mapped\n", got);
	return NULL;
}

/*
 * What exc_server calls when the message arrives.  MIG demands this name and
 * this shape; it is the whole of the handler.
 */
kern_return_t
catch_exception_raise(mach_port_t exception_port, mach_port_t thread,
		      mach_port_t task, int exception,
		      exception_data_t code, mach_msg_type_number_t codeCnt)
{
	(void) exception_port;
	(void) task;

	vm_address_t	fix = UNOWNED;

	(void) exception_port;
	(void) thread;

	exception_type = exception;
	exception_code0 = (codeCnt > 0) ? code[0] : 0;
	exception_seen = 1;

	/*
	 * Make the address good, then answer, which is what a handler is FOR.
	 *
	 * ⚠️ Not thread_terminate(), and the difference is a kernel panic.
	 * Terminating a thread that is stopped inside exception_raise() runs
	 * the activation return handlers, and x86-64's act_machine_return() is
	 * still the stub #453 left: `this machine has no RPC activation chain
	 * to return through'.  Found by this test doing exactly that, on four
	 * processors; see the issue it opened.
	 *
	 * Repairing and resuming is also the better demonstration.  Answering
	 * KERN_SUCCESS sends the thread back to the instruction that faulted,
	 * so a run that prints the line below has shown the whole round trip --
	 * fault, message, handler, reply, resume -- which is what a pager or a
	 * debugger does and what the arm is really about.
	 */
	if (vm_allocate(task, &fix, vm_page_size, FALSE) != KERN_SUCCESS) {
		printf("fault_test: [2] the handler could not map 0x%lx — WRONG\n",
		       (unsigned long) UNOWNED);
		return KERN_FAILURE;
	}

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
arm_two_exception_to_the_task(void)
{
	pthread_t	victim;
	kern_return_t	kr;

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &exc_port);
	if (kr != KERN_SUCCESS) {
		printf("fault_test: [2] mach_port_allocate failed (%d) — WRONG\n",
		       kr);
		return 0;
	}
	kr = mach_port_insert_right(mach_task_self(), exc_port, exc_port,
				    MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("fault_test: [2] insert_right failed (%d) — WRONG\n", kr);
		return 0;
	}

	kr = task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS,
				      exc_port, EXCEPTION_DEFAULT,
				      THREAD_STATE_NONE);
	if (kr != KERN_SUCCESS) {
		printf("fault_test: [2] task_set_exception_ports failed (%d)"
		       " — WRONG\n", kr);
		return 0;
	}

	if (pthread_create(&victim, NULL, the_thread_that_faults, NULL) != 0) {
		printf("fault_test: [2] pthread_create failed — WRONG\n");
		return 0;
	}

	/*
	 * One message, and then stop asking.  mach_msg_server_once returns
	 * after it has demultiplexed exactly one, which is all this arm has to
	 * see; a loop would leave the program with nothing to print.
	 */
	(void) mach_msg_server_once(exc_server, 4096, exc_port,
				    MACH_MSG_OPTION_NONE);

	if (!exception_seen) {
		printf("fault_test: [2] no exception arrived — WRONG\n");
		return 0;
	}

	/*
	 * Wait for the thread the handler sent back, so that the line it prints
	 * is inside this arm rather than racing the summary below it.
	 */
	(void) pthread_join(victim, NULL);

	printf("fault_test: [2] a fault on 0x%lx reached the task's exception "
	       "port as exception %d code %u%s\n",
	       (unsigned long) UNOWNED, exception_type,
	       (unsigned) exception_code0,
	       (exception_type == EXC_BAD_ACCESS)
	       ? " — EXC_BAD_ACCESS, named by the kernel and not by this program"
	       : " — WRONG, that is not EXC_BAD_ACCESS");

	if (!victim_resumed) {
		printf("fault_test: [2] but the thread never came back — WRONG\n");
		return 0;
	}

	return exception_type == EXC_BAD_ACCESS;
}

int
main(int argc, char **argv)
{
	int	passed = 0;

	printf("fault_test: started (#467)\n");

	/*
	 * #488: what this task was told it is called, and where that came
	 * from -- which is not the stack.
	 *
	 * A server bootstrap loads gets argv by RPC: crt0 calls
	 * bootstrap_arguments() on the task port, and reads a System V frame
	 * off the entry stack only when that fails, which is the execve'd
	 * case.  Printing it here is the consumer that tests that path: a task
	 * that cannot say its own name is a boot-path failure and not a
	 * failure of this test's own subject.
	 */
	if (argc > 0 && argv != 0 && argv[0] != 0)
		printf("fault_test: argv[0] is \"%s\", argc %d (#488)\n",
		       argv[0], argc);
	else
		printf("fault_test: NO arguments — argc %d, argv %p (#488)\n",
		       argc, (void *) argv);

	passed += arm_one_fault_and_resume();
	passed += arm_three_copyin_not_resident();
	passed += arm_two_exception_to_the_task();

	printf("fault_test: %d of 3 arms passed\n", passed);

	/*
	 * ⚠️ Does not exit.  There is no proc_server on this target to reap a
	 * task and nothing waits for this one, so returning would take the
	 * only thread out from under a program whose output the boot log is
	 * still the record of.  It stops here on purpose.
	 */
	for (;;)
		(void) mach_msg_server_once(exc_server, 4096, exc_port,
					    MACH_MSG_OPTION_NONE);

	return 0;
}
