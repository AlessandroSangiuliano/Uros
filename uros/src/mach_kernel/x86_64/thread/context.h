/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Switching from one thread to another (#408, MD contract 3/6).
 *
 * ══ Why this is six registers and not sixteen ═════════════════════════
 *
 * A context switch looks like it must save everything, and it does not.
 * From the point of view of the code that calls it, a switch is a function
 * call: control leaves, other things happen, control comes back.  The
 * System V ABI already says what survives a call and what does not, and the
 * compiler has already spilled anything it wanted to keep.
 *
 * So the switch preserves exactly the callee-saved set — rbx, rbp and
 * r12-r15 — and nothing else.  The caller-saved registers are not saved
 * because their owner already knows they are gone.  Saving them would be
 * doing work on behalf of code that has explicitly said it does not need it.
 *
 * That is the same reasoning as the syscall contract in
 * <syscall/syscall.h>, arrived at independently and pointing the same way:
 * both are the kernel declining to preserve what the ABI has already
 * declared dead.
 *
 * What makes it a *switch* rather than a call is one instruction in the
 * middle — the stack pointer changes, so the values popped afterwards are a
 * different thread's, and the return goes wherever that thread was.  A
 * thread is, at this level, precisely a stack pointer.
 *
 * ══ What is not here ══════════════════════════════════════════════════
 *
 * Floating-point and vector state, which is its own decision and its own
 * increment.  The register file above is what the ABI requires; the
 * extended state is larger than all of it put together and is not saved by
 * the same means or on the same schedule.
 */

#ifndef _X86_64_THREAD_CONTEXT_H_
#define _X86_64_THREAD_CONTEXT_H_

#ifndef __ASSEMBLER__

#include <stdint.h>

/*
 * What the kernel needs to resume a thread that is not running.
 *
 * One word for the register file, because the registers live on the
 * thread's own stack while it is switched out and the stack pointer is
 * where they are.  The second word is not for resuming this thread — it is
 * for the *entry paths*, which need to know where to land when a syscall or
 * a trap arrives while this thread is the current one.
 */
struct context {
	uint64_t rsp;			/* where its saved registers are   */
	uint64_t kernel_stack_top;	/* where an entry from ring 3 lands */
};

/*
 * Prepare a context that has never run, so that switching to it arrives at
 * `entry(arg)`.
 *
 * The stack is written to look exactly as though the thread had been
 * switched out — six saved registers and a return address — because that is
 * the only shape the switch knows how to resume.  A thread that has never
 * run and one that was interrupted must be indistinguishable to it, or
 * there would be two ways to resume and one of them would be rare.
 *
 * `stack_top` is the high end; stacks grow down.
 */
void context_init(struct context *ctx, uint64_t stack_top,
		  void (*entry)(void *), void *arg);

/*
 * Switch: save this thread's registers on its stack, take the other's.
 *
 * Returns — eventually, and on whatever processor is running `old` when
 * somebody switches back to it, which may not be this one.  That is worth
 * saying out loud, because everything after the call is running in a
 * different world from everything before it.
 */
void context_switch(struct context *old, struct context *fresh);

/*
 * The raw form, in assembly: save at *old_rsp, resume from new_rsp.
 *
 * context_switch() is this plus the bookkeeping that must accompany it —
 * chiefly telling this processor which stack an entry from ring 3 should
 * land on now.  Nothing outside thread/ should need the raw form; it is
 * declared because the C wrapper calls it.
 */
void context_switch_raw(uint64_t *old_rsp, uint64_t fresh_rsp);

#endif	/* __ASSEMBLER__ */

#endif	/* _X86_64_THREAD_CONTEXT_H_ */
