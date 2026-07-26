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
	 * The bases come from the machine, not from the frame, because the
	 * frame has nowhere to keep them: they are MSRs, and nothing pushes
	 * them.  Read here rather than saved on entry so that the answer is
	 * the one that is true, not the one that was true.
	 *
	 * ⚠️ Which makes this correct only while the thread being asked about
	 * is the one running.  A thread state read of a *stopped* thread has
	 * to take these from wherever the switch left them, and that place
	 * arrives with the scheduler.
	 */
	state->fs_base = rdmsr(MSR_FS_BASE);
	state->gs_base = rdmsr(MSR_KERNEL_GS_BASE);
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
		return THREAD_STATE_REFUSED;

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
	return THREAD_STATE_OK;
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
