/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Where a new pthread starts (#425).
 *
 * pthread_create has already made the kernel thread and found it a stack; this
 * says what state it wakes up in.  It is the twin of the bootstrap server's
 * set_regs -- the same question asked about a thread inside a task rather than
 * about the first thread of a new one.
 */

#include "pthread_internals.h"

/*
 * ⚠️ The argument goes in a REGISTER, and that is the whole difference from
 * i386's version of this file.
 *
 * i386 builds the frame by hand:
 *
 *	*--sp = (int) thread;	  the argument
 *	*--sp = 0;		  a fake return address for it to sit above
 *
 * because on that machine an argument is a stack slot, so the only way to
 * hand one to a function that is about to be entered by a jump is to write it
 * where a `call' would have left it.  Here the first argument is %rdi and
 * nothing goes on the stack at all, so both stores disappear.
 *
 * ⚠️ The fake return address does NOT disappear with them, and it is not
 * cargo.  routine() never returns -- it ends in pthread_exit -- but its
 * PROLOGUE runs, and the ABI's alignment rule is stated at the call: a
 * function is entered with rsp eight past a sixteen-byte boundary, because
 * `call' has just pushed eight bytes.  A thread entered by a jump has had no
 * call, so a word is pushed here to put the stack in the state the compiler
 * assumed when it laid the function out.  Without it every aligned spill in
 * routine() is eight bytes out, and the first `movaps' to a local takes a
 * general protection fault.
 */
void
_pthread_setup(pthread_t thread,
	       void (*routine)(pthread_t),
	       vm_address_t vsp)
{
	struct x86_64_thread_state state;
	struct x86_64_thread_state *ts = &state;
	kern_return_t r;
	unsigned int count;
	unsigned long *sp = (unsigned long *) vsp;

	/*
	 * Read what the kernel built for a fresh thread before changing it:
	 * the selectors and rflags belong to the kernel, and a zeroed frame
	 * would hand ring 3 a null code segment.
	 */
	count = x86_64_THREAD_STATE_COUNT;
	MACH_CALL(thread_get_state(thread->kernel_thread,
				   x86_64_THREAD_STATE,
				   (thread_state_t) &state,
				   &count),
		  r);

	sp = (unsigned long *) ((unsigned long) sp & ~15UL);
	*--sp = 0;			/* the return address that never was */

	ts->rip = (unsigned long) routine;
	ts->rdi = (unsigned long) thread;	/* the argument, in a register */
	ts->rsp = (unsigned long) sp;
	ts->rbp = 0;				/* outermost frame */

	MACH_CALL(thread_set_state(thread->kernel_thread,
				   x86_64_THREAD_STATE,
				   (thread_state_t) &state,
				   x86_64_THREAD_STATE_COUNT),
		  r);
}
