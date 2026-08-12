/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Where a new task starts, and on what stack (#422).
 *
 * This is the whole machine-dependent part of creating a server: bootstrap
 * has already made the task, mapped the image and found the entry point, and
 * what is left is to say it in the words this processor understands.
 *
 * ⚠️ Written beside i386's rather than shared with it, and the reason is not
 * the register names.  The two differ in what the ABI requires AT the entry
 * point, and that is a rule about a number rather than a spelling -- see the
 * alignment note below, where i386 subtracts sixteen and this subtracts eight.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_host.h>	/* vm_wire: it is a HOST call, not a vm one */
#include <mach/x86_64/thread_status.h>
#include <mach/x86_64/vm_param.h>

#include "bootstrap.h"

/*
 * A stack for the new task.
 *
 * Sixty-four kilobytes, as on i386.  It is the initial thread's stack and
 * nothing here grows it: a server that wants more asks vm_allocate for it,
 * and libpthreads gives its own threads their own (#425).
 */
#define	STACK_SIZE		(64*1024)

/*
 * ⚠️ EIGHT, where i386 subtracts sixteen, and it is not a translation of the
 * same rule to a wider word.
 *
 * The System V ABI says rsp must be sixteen-byte aligned AT a call
 * instruction, so that a function sees it eight past a boundary once its own
 * return address is on the stack.  A thread the kernel starts has no return
 * address: it is entered by a jump, not a call.  So the value the entry point
 * must see is the one a callee sees -- eight past sixteen -- and starting it
 * on a boundary would leave every SSE spill in that function misaligned.
 *
 * i386's sixteen is the same reasoning with a four-byte return address and a
 * different history; copying the number across would have been right by
 * accident on the width and wrong on the meaning.
 */
#define	STACK_ENTRY_OFFSET	0x8

void
set_regs(mach_port_t		host_port,
	 task_port_t		user_task,
	 thread_port_t		user_thread,
	 struct loader_info	*lp,
	 unsigned long		mapend,
	 vm_size_t		arg_size)
{
	vm_offset_t			stack_start;
	vm_offset_t			stack_end;
	struct x86_64_thread_state	regs;
	unsigned int			reg_size;

	/*
	 * The stack goes at the top of the task's address space, below
	 * whatever the caller has reserved.  VM_MAX_ADDRESS here is the
	 * canonical hole rather than four gigabytes -- 0x0000800000000000 --
	 * so the stack lands where a 64-bit user stack belongs.
	 */
	stack_end = mapend;
	if (!stack_end)
		stack_end = VM_MAX_ADDRESS;
	stack_start = stack_end - STACK_SIZE;

	(void) vm_allocate(user_task, &stack_start,
			   (vm_size_t)(stack_end - stack_start), FALSE);

	if (mapend)
		(void) vm_wire(host_port, user_task, stack_start,
			       (vm_size_t)(stack_end - stack_start),
			       VM_PROT_READ|VM_PROT_WRITE);

	/*
	 * Read the state the kernel built for a fresh thread, change the two
	 * words that make it ours, and put it back.  ⚠️ Read first and not
	 * zeroed: the segment selectors, rflags and everything the flavour
	 * carries beyond the general registers are the kernel's to choose,
	 * and a zeroed frame would hand ring 3 a null code segment.
	 */
	reg_size = x86_64_THREAD_STATE_COUNT;
	(void) thread_get_state(user_thread, x86_64_THREAD_STATE,
				(thread_state_t)&regs, &reg_size);

	regs.rip = lp->entry_1;
	regs.rsp = stack_end - STACK_ENTRY_OFFSET;

	(void) thread_set_state(user_thread, x86_64_THREAD_STATE,
				(thread_state_t)&regs, reg_size);

	(void) arg_size;
}
