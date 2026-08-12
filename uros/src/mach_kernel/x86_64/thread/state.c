/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What a thread's registers look like from outside (#408, MD contract 3/6).
 *
 * The shape, and why it is not a widened i386_thread_state, is in
 * <thread/state.h>.
 */

#include <stdint.h>

#include <cpu/desc.h>
#include <cpu/regs.h>
#include <pmap/layout.h>
#include <thread/state.h>
#include <trap/trap.h>

void thread_state_from_frame(const struct trap_frame *frame,
			     uint64_t fs_base, uint64_t gs_base,
			     struct x86_64_thread_state *state)
{
	state->rax = frame->rax;
	state->rbx = frame->rbx;
	state->rcx = frame->rcx;
	state->rdx = frame->rdx;
	state->rdi = frame->rdi;
	state->rsi = frame->rsi;
	state->rbp = frame->rbp;
	state->rsp = frame->rsp;
	state->r8  = frame->r8;
	state->r9  = frame->r9;
	state->r10 = frame->r10;
	state->r11 = frame->r11;
	state->r12 = frame->r12;
	state->r13 = frame->r13;
	state->r14 = frame->r14;
	state->r15 = frame->r15;

	state->rip = frame->rip;
	state->rflags = frame->rflags;
	state->cs = frame->cs;
	state->ss = frame->ss;

	/*
	 * The bases are the CALLER's to supply, because the frame has nowhere
	 * to keep them -- they are MSRs, and nothing pushes them.
	 *
	 * 🔥 This used to read them with rdmsr(), under a note saying that was
	 * correct only while the thread being asked about was the one running.
	 * It was not: act_machine_get_state() is asked about a thread that is
	 * STOPPED, by definition -- thread_get_state() waits for it to block
	 * before asking -- so the answer was the running kernel's bases, and
	 * gs_base was this processor's per-CPU block.  A kernel base is exactly
	 * what the write direction must refuse (#440), so every read-modify-
	 * write of a stopped thread's state was refused.
	 *
	 * A parameter rather than a read, so that each caller says where its
	 * answer comes from: the pcb for a real thread, a chosen value for the
	 * self-test that has no thread at all.
	 */
	state->fs_base = fs_base;
	state->gs_base = gs_base;
}

/*
 * The flag bits a thread may choose for itself: the arithmetic results, the
 * direction flag, and the single-step flag a debugger sets.
 *
 * Everything else is refused by omission — most importantly the two I/O
 * privilege bits, which would hand out access to every port on the machine,
 * and the nested-task flag, which changes what a return instruction does.
 */
#define RFLAGS_USER_SETTABLE	0x0DD5UL	/* CF PF AF ZF SF TF DF OF */
#define RFLAGS_ALWAYS_ONE	0x0002UL	/* reserved, and must be set */

/*
 * The frame a thread is born with (#422).
 *
 * 🔥 On i386 this has always existed and is why the loader works there.  Its
 * pcb carries the user frame INSIDE it, and pcb_init() fills the selectors in
 * at activation creation under a comment that says exactly what it is for:
 * "Guarantee that the bootstrapped thread will be in user mode."  So on that
 * machine a thread that has never run still HAS registers, and
 * thread_get_state() on it answers.
 *
 * Here the frame is a POINTER into the kernel stack, and it used to be null
 * until somebody wrote one.  act_machine_get_state() refuses a null frame --
 * correctly, there is nothing to read -- so thread_get_state() on a fresh
 * thread returned KERN_INVALID_ARGUMENT.
 *
 * ⚠️ Which nobody saw, because the loader does not look.  bootstrap's
 * set_regs() reads the state, changes rip and rsp, and writes it back, and it
 * ignores both return values -- the same eleven lines on both machines, and
 * correct on the one where the read answers.  Here the read failed, the
 * structure it was to fill stayed as the stack had left it, and fifteen
 * general registers of somebody else's locals were written into the frame of
 * a task about to enter ring 3.
 *
 * The selectors and rflags were never at risk: thread_state_to_frame() imposes
 * those below and always has.  That is precisely why this was quiet -- the
 * thread started at the right address, on the right stack, at ring 3, with
 * interrupts on, and with rubbish in rbx.
 *
 * So a thread is given its frame when it is given its stack, and this is the
 * one place that says what an untouched one contains.
 */
void thread_frame_init(struct trap_frame *frame)
{
	unsigned i;

	for (i = 0; i < sizeof *frame; i++)
		((volatile unsigned char *) frame)[i] = 0;

	frame->cs     = USER_CS_RPL3;
	frame->ss     = USER_DS_RPL3;
	frame->rflags = RFLAGS_ALWAYS_ONE | RFLAGS_IF;
}

int thread_state_bases_ok(const struct x86_64_thread_state *state)
{
	return va_is_canonical(state->fs_base) && va_is_user(state->fs_base)
	    && va_is_canonical(state->gs_base) && va_is_user(state->gs_base);
}

int thread_state_to_frame(const struct x86_64_thread_state *state,
			  struct trap_frame *frame)
{
	/*
	 * Before anything is written, because a frame half built from a
	 * request that is about to be refused is worse than either answer.
	 */
	if (!thread_state_bases_ok(state))
		return 0;			/* refused, frame untouched */

	frame->rax = state->rax;
	frame->rbx = state->rbx;
	frame->rcx = state->rcx;
	frame->rdx = state->rdx;
	frame->rdi = state->rdi;
	frame->rsi = state->rsi;
	frame->rbp = state->rbp;
	frame->rsp = state->rsp;
	frame->r8  = state->r8;
	frame->r9  = state->r9;
	frame->r10 = state->r10;
	frame->r11 = state->r11;
	frame->r12 = state->r12;
	frame->r13 = state->r13;
	frame->r14 = state->r14;
	frame->r15 = state->r15;

	frame->rip = state->rip;

	/*
	 * And here the copying stops.
	 *
	 * These three fields decide privilege, and they arrive from whoever
	 * holds a port to this thread.  A code segment naming ring 0 would be
	 * a request to execute as the kernel; I/O privilege in the flags would
	 * be a request for every port on the machine; interrupts disabled
	 * would be a request the machine cannot take the processor back.
	 *
	 * So they are not copied.  The selectors are imposed, and the flags
	 * are reduced to the bits a thread is entitled to choose, with the two
	 * the architecture requires added back.
	 *
	 * The round trip is therefore deliberately not the identity, and a
	 * test that demanded it would be demanding the hole.
	 */
	frame->cs = USER_CS_RPL3;
	frame->ss = USER_DS_RPL3;
	frame->rflags = (state->rflags & RFLAGS_USER_SETTABLE)
		      | RFLAGS_ALWAYS_ONE | RFLAGS_IF;

	/*
	 * ⚠️ fs_base and gs_base are checked above and go no further, because
	 * there is nowhere here to put them: they are MSRs, and this builds a
	 * frame.  Applying them needs a thread to apply them to, which arrives
	 * with the scheduler.  The check is here now so that it is already the
	 * guard when the second half lands, rather than something to remember.
	 */
	return 1;				/* applied */
}

/*
 * The legacy image is the first 512 bytes of the save area under FXSAVE and
 * under XSAVE alike — the extended components live above it — which is what
 * lets one flavour serve both without asking which instruction wrote it.
 */
void float_state_from_area(const void *area, struct x86_64_float_state *state)
{
	const uint8_t *src = area;

	state->valid = 1;
	state->reserved = 0;

	for (unsigned i = 0; i < sizeof(state->fx_image); i++)
		state->fx_image[i] = src[i];
}

void float_state_to_area(const struct x86_64_float_state *state, void *area)
{
	uint8_t *dst = area;

	if (!state->valid)
		return;

	for (unsigned i = 0; i < sizeof(state->fx_image); i++)
		dst[i] = state->fx_image[i];
}

void exception_state_from_frame(const struct trap_frame *frame,
				struct x86_64_exception_state *state)
{
	state->trapno = frame->vector;
	state->err = frame->error;

	/*
	 * Only a page fault has one, and CR2 holds whatever the last one was
	 * — so reporting it for any other vector would be reporting an
	 * unrelated address with every appearance of relevance.
	 */
	state->faultvaddr = frame->vector == T_PAGE_FAULT ? read_cr2() : 0;
}

/*
 * Flavor to size, in natural_t words (#453).
 *
 * <kern/exception.c> indexes this directly with the flavor a server asked
 * for, so the array is indexed by flavor number and entry zero is the hole
 * where no flavor lives.  THREAD_STATE_NONE has no state and so has size
 * zero, which is what makes "none" answerable through the same table as the
 * rest instead of through a special case at every call site.
 *
 * The assertion below is the point of writing it as an array with a checked
 * length: adding a flavor to <mach/x86_64/thread_status.h> without adding
 * its size here would otherwise read one past the end and return whatever
 * followed, as a word count, to code about to copy that many words.
 */
unsigned int state_count[] = {
	/* no flavor 0 */		0,
	/* x86_64_THREAD_STATE */	x86_64_THREAD_STATE_COUNT,
	/* x86_64_FLOAT_STATE */	x86_64_FLOAT_STATE_COUNT,
	/* x86_64_EXCEPTION_STATE */	x86_64_EXCEPTION_STATE_COUNT,
	/* THREAD_STATE_NONE */		0,
};

_Static_assert(sizeof(state_count) / sizeof(state_count[0])
	       == THREAD_STATE_NONE + 1,
	       "state_count[] and the flavor list in "
	       "<mach/x86_64/thread_status.h> have drifted apart");

/*
 * And that every size in it fits the buffer the exception path puts on its
 * stack (#408).
 *
 * <kern/exception.c> declares `natural_t state[THREAD_MACHINE_STATE_MAX]',
 * reads state_count[flavor] into the count, and hands both to
 * act_machine_get_state().  Nothing between those three lines compares the
 * table against the buffer, so a flavor whose size exceeded the maximum would
 * be written past the end of a kernel stack array — by a flavor number that
 * arrives from whoever registered the exception port.
 *
 * The assertion in <thread/state.h> does not cover this: it ties
 * THREAD_MACHINE_STATE_MAX to what a *message* can carry, and
 * THREAD_MACHINE_STATE_MAX is itself defined as the float flavor's count, so
 * a new and larger flavor would satisfy it while overflowing the buffer.
 * These are the other direction, one per flavor, so that adding one stops the
 * build until the maximum is raised with it.
 */
_Static_assert(x86_64_THREAD_STATE_COUNT <= THREAD_MACHINE_STATE_MAX,
	       "the thread state flavor no longer fits the buffer "
	       "kern/exception.c reads it into");
_Static_assert(x86_64_FLOAT_STATE_COUNT <= THREAD_MACHINE_STATE_MAX,
	       "the float state flavor no longer fits the buffer "
	       "kern/exception.c reads it into");
_Static_assert(x86_64_EXCEPTION_STATE_COUNT <= THREAD_MACHINE_STATE_MAX,
	       "the exception state flavor no longer fits the buffer "
	       "kern/exception.c reads it into");
