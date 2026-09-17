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
 * The frame a never-run thread is resumed from.  The first eight words are
 * the switch frame, CTX_R15..CTX_RETURN in <thread/context.h>; this is a
 * forgery of what context_switch_raw() expects to find there, and it has to
 * be good enough that a thread which has never run and one that was
 * interrupted are indistinguishable to it.
 *
 * Ten words rather than the eight the switch consumes.  The ninth is a
 * deliberate zero where context_thread_start()'s return address would be,
 * so that a backtrace stops at the thread's own beginning rather than
 * walking into whatever the stack held in a previous life.  The tenth is
 * alignment: the ABI wants the stack sixteen-byte aligned at a call, and
 * the thread's first call is the one context_thread_start() makes.
 */
#define CTX_CALLER	8	/* the zero that ends a backtrace */
#define CTX_WORDS	10

/*
 * What a thread starts with in RFLAGS: interrupts on, and bit 1, which the
 * architecture requires to be set.  Nothing else -- no direction flag (the
 * ABI requires it clear at every call), no trap flag, no I/O privilege.
 *
 * It has to be stated because RFLAGS is now saved and restored across a
 * switch, so a thread that has never run needs a value to be resumed with,
 * and zero is not one: it would start the thread with interrupts disabled,
 * which is the exact failure the saving was added to close.
 */
#define CTX_RFLAGS_INITIAL	0x202ULL

/* #561: how often the vector state did NOT have to move.  See quiet_census. */
unsigned long	context_fpu_switches;
unsigned long	context_fpu_saves_skipped;
unsigned long	context_fpu_restores_skipped;
unsigned long	context_fpu_exempted;	/* contexts told they need nothing */

void context_become_current(struct context *ctx, uint64_t stack_top,
			    void *fpu_area)
{
	ctx->rsp = 0;			/* the first switch away writes it */
	ctx->kernel_stack_top = stack_top;
	ctx->fpu_area = fpu_area;
	/*
	 * 🔴 THE CONTEXT THE MACHINE IS ALREADY RUNNING IN, and it keeps the
	 * state moving (#561).  This is the boot context adopting the first
	 * thread: whatever is in the vector registers at that moment is not
	 * something this code put there, so it is not something it may decide
	 * is worthless.
	 */
	ctx->fpu_switch = 1;
}

void context_needs_vector_state(struct context *ctx)
{
	ctx->fpu_switch = 1;
}

/*
 * 🔴 THE ONLY WAY TO TURN IT OFF, AND IT HAS ONE CALLER ON PURPOSE (#561).
 *
 * Being wrong here is silent: a thread that executes a vector instruction
 * with this clear corrupts whatever the registers held, which is some other
 * thread's state, and nothing reports it.  So the exemption is not a
 * judgement any caller may make -- it is made once, for threads of the kernel
 * task, which cannot return to ring 3 and therefore execute only code this
 * build compiles without vector instructions.  A kernel thread that wants
 * them says so with context_needs_vector_state() and gets them.
 *
 * ⚠️ The default is the expensive one, set by context_init(), because a
 * context whose flag was never written would otherwise inherit whatever the
 * zone element last held -- and zalloc() does not zero.  Getting that wrong
 * in the safe direction costs 228 ns; in the other it costs correctness.
 */
void context_exempt_vector_state(struct context *ctx)
{
	ctx->fpu_switch = 0;
	context_fpu_exempted++;
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

	/*
	 * The expensive answer by default (#561).  See
	 * context_exempt_vector_state() for who is allowed to change it and
	 * why the default is this way round.
	 */
	ctx->fpu_switch = 1;

	frame[CTX_R15] = 0;
	frame[CTX_R14] = 0;
	frame[CTX_R13] = (uint64_t)(uintptr_t)entry;
	frame[CTX_R12] = (uint64_t)(uintptr_t)arg;
	frame[CTX_RBX] = 0;
	frame[CTX_RBP] = 0;
	frame[CTX_RFLAGS] = CTX_RFLAGS_INITIAL;
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
	/*
	 * 🔴 THE VECTOR STATE MOVES FOR WHOEVER DECLARED IT, AND FOR NOBODY
	 * ELSE (#561).
	 *
	 * This was unconditional, and #554 measured what that costs: 228 ns on
	 * a block-and-wake round trip, of which the restore is 165 -- more than
	 * the whole gap that issue was opened about.  Every thread paid it,
	 * including the ones that have never executed a vector instruction and
	 * never will, which on this target is every thread of the kernel task
	 * bar the ones that ask.
	 *
	 * 🔑 The area is still there in both cases: what is conditional is the
	 * work, not the memory, so act_machine_get_state() and every other
	 * reader still find somewhere to read.
	 *
	 * ⚠️ WHY THIS IS NOT THE LAZY SCHEME.  A thread whose flag is clear runs
	 * with the previous thread's values still in the registers -- and it is
	 * kernel code, which the build forbids to touch them and a checker
	 * enforces.  A USER thread never does: its own state is restored before
	 * it runs, every time.  The hole in lazy FPU (CVE-2018-3665, #560) is
	 * exactly the case this does not create.
	 */
	/*
	 * Counted, because an optimisation nobody can see fire is an
	 * optimisation nobody can tell from a no-op (#561).  Plain adds on a
	 * path that is already serialised by the switch itself; they are a
	 * report, not an accounting.
	 */
	context_fpu_switches++;

	if (old->fpu_switch && old->fpu_area != 0)
		fpu_save(old->fpu_area);
	else
		context_fpu_saves_skipped++;

	if (fresh->fpu_area == 0)
		panic("thread: switching to a thread with nowhere to restore "
		      "its FPU state from");

	if (fresh->fpu_switch)
		fpu_restore(fresh->fpu_area);
	else
		context_fpu_restores_skipped++;

	context_switch_raw(&old->rsp, fresh->rsp);
}
