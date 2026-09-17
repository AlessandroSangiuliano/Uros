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

/*
 * The switch frame: what context_switch_raw() leaves at ctx.rsp, as indices
 * of 64-bit words, in the order it pops them.  pushfq first and %r15 last,
 * so counting up from the saved stack pointer the registers come out
 * reversed.
 *
 * ⚠️ Here and not private to context.c, because a switched-out thread is
 * READ as well as resumed: the debugger and the idle census walk a blocked
 * thread's stack from its saved %rbp (#425, #558).  The debugger used to
 * spell those two words as saved[5] and saved[7], a second copy of this
 * layout that context.S could have moved without either noticing.
 */
#define CTX_R15		0
#define CTX_R14		1
#define CTX_R13		2	/* a fresh thread's entry point   */
#define CTX_R12		3	/* its argument                   */
#define CTX_RBX		4
#define CTX_RBP		5	/* where a backtrace starts       */
#define CTX_RFLAGS	6	/* the interrupt flag it resumes with */
#define CTX_RETURN	7	/* where the switch's `ret` goes  */

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
	void    *fpu_area;		/* and its floating-point state    */
	/*
	 * 🔴 WHETHER THE SWITCH HAS TO MOVE THAT STATE (#561).
	 *
	 * The area is allocated for every thread and stays allocated -- the
	 * invariant this file's neighbour defends is that "a thread with a pcb
	 * has somewhere to save its registers", and act_machine_get_state()
	 * reads it without checking for null.  What is conditional is not the
	 * MEMORY, it is the WORK: saving and restoring on every switch costs
	 * 228 ns a round trip (#554), and a thread that never executes a vector
	 * instruction has nothing there worth moving.
	 *
	 * 🔑 DECLARED, NOT INFERRED.  The compiler cannot emit a vector
	 * instruction into this kernel -- it is built -mgeneral-regs-only, and
	 * the binary has none -- but a human can write one by hand, and one
	 * does: fpu_stress.c holds a pattern in all sixteen registers on
	 * purpose.  So a kernel thread does not get this by a rule about
	 * kernel threads; it gets it by asking, and scripts/kernel-vector-check
	 * fails the build if vector instructions appear outside the files
	 * allowed to have them.
	 *
	 * ⚠️ AND THE SECURITY ARGUMENT IS ABOUT THE OTHER BOUNDARY.  While a
	 * thread with this clear runs, the registers still hold the last user
	 * thread's state -- that is not a leak, because the kernel is the one
	 * reading them and before ANY user thread runs its own state is
	 * restored.  What must never come back is the lazy scheme, where a USER
	 * thread runs with another user's registers behind a #NM that
	 * speculation can step around (CVE-2018-3665, #560).
	 */
	int	 fpu_switch;		/* 0: nothing to move on a switch  */
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
		  void (*entry)(void *), void *arg, void *fpu_area);

/*
 * Say that this thread will execute vector instructions, so the switch must
 * carry its state (#561).  A user thread is told this when it is created; a
 * kernel thread has to ask, because nothing can tell from the outside.
 */
void context_needs_vector_state(struct context *ctx);

/*
 * And the exemption, which has ONE caller: threads of the kernel task, which
 * never return to ring 3.  Read the comment on the definition before adding a
 * second -- being wrong here corrupts another thread's registers in silence.
 */
void context_exempt_vector_state(struct context *ctx);

/*
 * How many switches happened and how many of them did not have to move vector
 * state (#561).  Read by quiet_census: an exemption nobody can see fire cannot
 * be told apart from a no-op.
 */
extern unsigned long	context_fpu_exempted;

/*
 * 🔴 AND HOW OFTEN THE EXEMPTION FIRES, WHICH IS OFF (#561).
 *
 *	cmake -DUROS_CONTEXT_FPU_COUNT=ON
 *
 * Counting it means three increments on EVERY context switch, for ever, to
 * learn something that is learned once -- and the thing being counted is a
 * saving of about 114 ns, so an instrument on that path can cost more than its
 * subject.  It was on while #561 was being written, it found that the first
 * version was a no-op, and then it came off.
 *
 * ⚠️ Per-processor when it is on (see <cpu/percpu.h>): three globals written by
 * every processor on the switch path would bounce one cache line between them,
 * which is the measurement eating the thing measured.
 *
 * What stays on is free and catches the failure this found: the line
 * kern/startup.c prints once, and the `fpu=' on each thread in quiet_census.
 */
#ifndef	CONTEXT_FPU_COUNT
#define	CONTEXT_FPU_COUNT	0
#endif

#if	CONTEXT_FPU_COUNT
void context_fpu_counts(unsigned long long *switches,
			unsigned long long *saves_skipped,
			unsigned long long *restores_skipped);
#endif


/*
 * Fill in a context for the thread that is *already running* — the boot
 * path, which became a thread by being switched away from.
 *
 * There is nothing to prepare for resuming it: a running thread's saved
 * state is wherever the switch will put it.  What it does need beforehand
 * is somewhere for its floating-point state to go and a record of its stack,
 * because the first switch away reads both.
 */
void context_become_current(struct context *ctx, uint64_t stack_top,
			    void *fpu_area);

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
