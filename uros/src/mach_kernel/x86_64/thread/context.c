/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Switching from one thread to another (#408, MD contract 3/6).
 */

#include <stdint.h>

#include <cpu/percpu.h>
#include <thread/context.h>
#include <thread/fpu.h>
#include <trap/trap.h>

extern void context_thread_start(void);

/*
 * The frame a never-run thread is resumed from, in the order
 * context_switch_raw() pops it.  Read the two together: this is a forgery
 * of what that code expects to find, and it has to be good enough that a
 * thread which has never run and one that was interrupted are
 * indistinguishable to it.
 *
 * Nine words rather than the seven the switch consumes.  The eighth is a
 * deliberate zero where context_thread_start()'s return address would be,
 * so that a backtrace stops at the thread's own beginning rather than
 * walking into whatever the stack held in a previous life.  The ninth is
 * alignment: the ABI wants the stack sixteen-byte aligned at a call, and
 * the thread's first call is the one context_thread_start() makes.
 */
#define CTX_R15		0
#define CTX_R14		1
#define CTX_R13		2	/* the entry point   */
#define CTX_R12		3	/* its argument      */
#define CTX_RBX		4
#define CTX_RBP		5
#define CTX_RETURN	6	/* where the switch's `ret` goes */
#define CTX_CALLER	7	/* the zero that ends a backtrace */
#define CTX_WORDS	9

void context_become_current(struct context *ctx, uint64_t stack_top,
			    void *fpu_area)
{
	ctx->rsp = 0;			/* the first switch away writes it */
	ctx->kernel_stack_top = stack_top;
	ctx->fpu_area = fpu_area;
}

void context_init(struct context *ctx, uint64_t stack_top,
		  void (*entry)(void *), void *arg, void *fpu_area)
{
	/*
	 * ⚠️ Below the reserved user frame, not at the top of the stack.
	 * <trap/trap.h> says why: those bytes belong to the trap frame,
	 * which act_machine_set_state() may write while this thread is
	 * running kernel code on the same stack.
	 */
	uint64_t rsp = KERNEL_STACK_USER_FRAME(stack_top) - CTX_WORDS * 8;
	uint64_t *frame = (uint64_t *)(uintptr_t)rsp;

	if (stack_top & 0xF)
		panic("thread: a stack that is not sixteen-byte aligned");

	frame[CTX_R15] = 0;
	frame[CTX_R14] = 0;
	frame[CTX_R13] = (uint64_t)(uintptr_t)entry;
	frame[CTX_R12] = (uint64_t)(uintptr_t)arg;
	frame[CTX_RBX] = 0;
	frame[CTX_RBP] = 0;
	frame[CTX_RETURN] = (uint64_t)(uintptr_t)context_thread_start;
	frame[CTX_CALLER] = 0;
	frame[CTX_CALLER + 1] = 0;

	ctx->rsp = rsp;
	ctx->kernel_stack_top = stack_top;
	ctx->fpu_area = fpu_area;

	/*
	 * A thread does not inherit whatever the memory held.  Starting with
	 * another thread's registers would be a disclosure with extra steps,
	 * and starting with uninitialised memory is worse — the restore
	 * instruction accepts some patterns as exceptions waiting to happen.
	 */
	fpu_area_init(fpu_area);
}

void context_switch(struct context *old, struct context *fresh)
{
	/*
	 * Tell this processor where an entry from ring 3 lands now.
	 *
	 * The syscall path reads this out of the per-CPU block before it has
	 * a stack to look anything up with, so it has to be correct *before*
	 * the switch rather than discovered after it.  Until now it held the
	 * processor's single privilege-transition stack, which was right only
	 * while there was one thread to arrive on it.
	 */
	/*
	 * ⚠️ Below the reserved user frame, the same as the initial stack this
	 * file already sets that way (#474).  The syscall entry starts using
	 * this as a stack; the top belongs to the frame a trap builds and
	 * pcb->user names.
	 */
	percpu()->kernel_rsp = KERNEL_STACK_USER_FRAME(fresh->kernel_stack_top);

	/*
	 * Both halves before the switch, because after it this code is
	 * running as a different thread and `old` is no longer us.  The
	 * register file is separate from the stack, so moving it here is not
	 * early — it is the only place it can be.
	 */
	/*
	 * Two different questions, and they were one check until the first
	 * boot asked them (#458).
	 *
	 * An outgoing context with no FPU area is legitimate exactly once per
	 * processor: load_context() starts the first thread and there is no
	 * thread being left behind, so it hands a zeroed context whose null
	 * area means "nothing to save".  Saving anyway would write a register
	 * file through a pointer nobody owns.
	 *
	 * An incoming context with no FPU area is never legitimate.  The
	 * thread is about to run, and the first instruction that touches a
	 * vector register would restore from nowhere.
	 */
	if (old->fpu_area != 0)
		fpu_save(old->fpu_area);

	if (fresh->fpu_area == 0)
		panic("thread: switching to a thread with nowhere to restore "
		      "its FPU state from");

	fpu_restore(fresh->fpu_area);

	context_switch_raw(&old->rsp, fresh->rsp);
}
