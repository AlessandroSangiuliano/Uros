/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What a thread's registers look like from outside (#408, MD contract 3/6).
 *
 * This is not an internal layout.  thread_get_state() and thread_set_state()
 * hand these across the port interface, so a debugger reads them, an
 * exception handler in another task reads them, libmach declares them, and
 * every one of those is compiled separately and shipped separately.  Getting
 * the shape wrong is not a refactor away; it is a flag day.
 *
 * ── The precedent, which is #259 and not the one the issue cites ──────
 *
 * #408 points at #275 as the history of getting this wrong.  That is a
 * mis-citation — #275 is the tty and job-control work — and the real
 * precedent is worth more than the wrong one, so: #259.
 *
 * libposix-uros used to build i386 thread state by hand for the threads it
 * bootstrapped, with the %gs selector written in as a constant in three
 * places.  It worked until real thread-local storage arrived, at which point
 * every one of those threads faulted the moment it touched libc — because
 * the constant had been right for one arrangement and was now a description
 * of nothing.
 *
 * The lesson is not "be careful with segment registers".  It is that **an
 * exported structure with a field userland can plausibly fill in by hand is
 * an invitation to do exactly that**, and the invitation is accepted.  So
 * the design question for each field is not "does this register exist" but
 * "what happens when somebody writes a constant here".
 *
 * ── Which is why this is not a widened i386_thread_state ──────────────
 *
 * The obvious port takes struct i386_thread_state and makes each field
 * sixty-four bits.  That would carry four segment selectors — ds, es, and
 * the pair fs and gs — into an architecture where segmentation does not
 * exist.  In long mode ds and es are ignored entirely, and fs and gs matter
 * only through their *bases*, which are in MSRs and are not what a selector
 * names.
 *
 * A debugger handed those fields would find them meaningless, and userland
 * handed them would fill them in — with a constant, exactly as #259 did.
 *
 * So the selectors are gone and fs_base and gs_base are here instead, as the
 * sixty-four-bit addresses they actually are.  cs and ss stay, alone among
 * the segments, because their low two bits are the privilege level and that
 * is a real fact about a thread that nothing else records.
 *
 * ── Fixed size, and what that costs ───────────────────────────────────
 *
 * A flavour's count has to be a compile-time constant: the interface passes
 * an array of natural-sized words and a number of them.  The extended
 * processor state is not a compile-time size — it grows with the features
 * the processor has — so it cannot be this flavour, and the floating-point
 * flavour here carries the legacy 512-byte image alone.
 *
 * That image is the first 512 bytes of the save area under both FXSAVE and
 * XSAVE, which is why one flavour serves both.  The vector state above it
 * needs a flavour of its own, and saying so here is the difference between a
 * decision and an omission.
 */

#ifndef _X86_64_THREAD_STATE_H_
#define _X86_64_THREAD_STATE_H_

#include <stdint.h>

#include <mach/machine/thread_state.h>

/*
 * The exported shape lives in <mach/machine/thread_status.h> (#416): these
 * structures cross the port interface, so a debugger, an exception handler in
 * another task and libmach all declare them, and none of those can reach a
 * header inside the kernel's machine-dependent tree.  What stays here is what
 * is genuinely the kernel's own — the conversions to and from the trap frame,
 * and the refusal of bases that arrive from outside.
 */
#include <mach/machine/thread_status.h>

/*
 * Whole numbers of words, checked rather than assumed.  A structure that is
 * not one silently truncates on the way through an interface that counts in
 * them, and the missing bytes are at the end — which is where the registers
 * nobody tests are.
 */
_Static_assert(sizeof(struct x86_64_thread_state) % sizeof(state_word_t) == 0,
	       "thread state is not a whole number of words");
_Static_assert(sizeof(struct x86_64_float_state) % sizeof(state_word_t) == 0,
	       "float state is not a whole number of words");
_Static_assert(sizeof(struct x86_64_exception_state) % sizeof(state_word_t) == 0,
	       "exception state is not a whole number of words");

/*
 * And the sizes themselves, pinned.  Not because these numbers are sacred,
 * but because the day one of them changes is a day every separately compiled
 * reader of this structure has to be rebuilt — so it should be a decision
 * somebody makes, announced by a failing build, and not a consequence of
 * adding a field.
 */
_Static_assert(sizeof(struct x86_64_thread_state) == 176, "thread state size changed");
_Static_assert(sizeof(struct x86_64_float_state) == 520, "float state size changed");

/*
 * And that the interface can actually carry the largest of them (#416).
 *
 * <mach/machine/thread_state.h> gives the stub generator a literal, because
 * that header is read by the preprocessor while it digests a .defs file and
 * has no structure in scope to measure.  This is the other end of that
 * number: a flavour that outgrows it does not fail to travel, it travels
 * short — the count comes back smaller than it went out and the registers at
 * the end are simply absent.  So the two are tied together here, and the
 * kernel stops building rather than shipping a truncating interface.
 */
_Static_assert(THREAD_MACHINE_STATE_MAX <= THREAD_STATE_MAX,
	       "a state flavour is larger than a message can carry");

struct trap_frame;

/*
 * Out of the kernel's own frame and into the exported shape, and back.
 *
 * Two functions rather than a cast, because they are not the same structure
 * and must not become one: the frame is ordered by how the entry stubs push
 * it, and this is ordered for whoever reads it.  Tying them together would
 * mean a change to the entry path silently changing an external ABI.
 */
void thread_state_from_frame(const struct trap_frame *frame,
			     uint64_t fs_base, uint64_t gs_base,
			     struct x86_64_thread_state *state);

/*
 * ⚠️ The reverse direction takes values from *outside* the kernel.
 *
 * A thread's state can be set by anyone holding a port to it, so every field
 * here is attacker-controlled and two of them are privilege: a code segment
 * naming ring 0, or flags asking for I/O privilege or for interrupts to be
 * disabled, would each be a way to obtain something the caller does not
 * have.  So the selectors are not copied but *imposed*, and the flags are
 * masked to the ones a thread is entitled to change.
 *
 * Which means the round trip is deliberately not the identity, and that is
 * the point rather than a defect.
 *
 * ── One field that cannot be imposed, and so must be refused ──────────
 *
 * The selectors are imposed because there is exactly one right answer for
 * each: a thread runs at ring 3, and nothing else is available to it.  The
 * segment bases are the opposite case — the whole purpose of the field is
 * that the thread chooses the value — so there is nothing to substitute, and
 * the only two options are to accept or to say no.
 *
 * Saying no matters because of the swapgs window (#440).  The trap entry for
 * the four vectors that can arrive inside it decides what to do by asking
 * whether the loaded base is a kernel address, and that is a proof only
 * while a base a thread supplied cannot be one.  CR4.FSGSBASE being clear
 * keeps ring 3 from writing one directly; this keeps it from asking the
 * kernel to write one on its behalf.
 *
 * So this returns, and a refused state changes nothing at all: a frame half
 * built from a request that was going to be rejected is worse than either
 * outcome.
 */
/*
 * ⚠️ IT ANSWERS A QUESTION, NOT A STATUS CODE, AND THAT IS THE FIX (#408).
 *
 * This used to answer THREAD_STATE_OK (0) or THREAD_STATE_REFUSED (-1) — the
 * shape of a kern_return_t — and act_machine_set_state() read the answer as a
 * plain boolean:
 *
 *	if (!thread_state_to_frame(state, pcb->user))
 *		return KERN_INVALID_ARGUMENT;
 *
 * which is exactly backwards for a code where zero means yes.  Both answers
 * came out wrong, which is worse than getting one of them wrong: a valid state
 * was WRITTEN to the frame and then reported as KERN_INVALID_ARGUMENT, and a
 * state naming a segment base in the kernel half was refused by the conversion,
 * changed nothing, and was reported as KERN_SUCCESS.  Two halves each
 * reasonable on its own, no compiler able to compare them, standing from the
 * day it was written until something finally called it — #448's shape again.
 *
 * So the two constants are gone rather than renumbered.  A named pair with zero
 * for success is an *invitation* to read the answer as a status code, and the
 * invitation was accepted; renumbering them would have made both spellings
 * agree and left the invitation standing.  What is left is a predicate, like
 * thread_state_bases_ok() below and answering in the same coin: NONZERO if the
 * state was applied, zero if it was refused and the frame was not touched.
 * There is one way to read that and it is right.
 *
 * ⚠️ It is deliberately not carried further into a type a wrong reading could
 * not compile — a one-member structure would do that.  It would buy nothing
 * here: with a predicate, every spelling a caller might reach for already gives
 * the right answer, so there is no wrong reading left for a compiler to catch.
 */
/*
 * Fill in the frame a thread that has never run to ring 3 should have: zeroed
 * general registers, the ring-3 selectors, and interrupts enabled.  Called
 * where a thread gets its kernel stack, so that reading a fresh thread's state
 * answers rather than refusing -- see the note above the definition.
 */
void thread_frame_init(struct trap_frame *frame);

int thread_state_to_frame(const struct x86_64_thread_state *state,
			  struct trap_frame *frame);

/*
 * Whether the segment bases in a state are ones the kernel may be given:
 * canonical, because writing a base that is not faults in the kernel at the
 * moment it is applied, and in the lower half, because the upper half is the
 * kernel's and a base there would make the window check answerable by the
 * caller.
 *
 * Separate from the conversion above so that the day the bases are applied —
 * which needs a thread to apply them to, and so needs the scheduler — the
 * check guarding them is the one already here rather than a second opinion.
 */
int thread_state_bases_ok(const struct x86_64_thread_state *state);

/* The floating-point flavour, out of a save area and back into one. */
void float_state_from_area(const void *area, struct x86_64_float_state *state);
void float_state_to_area(const struct x86_64_float_state *state, void *area);

/* Why it stopped, out of the frame that says so. */
void exception_state_from_frame(const struct trap_frame *frame,
				struct x86_64_exception_state *state);

#endif	/* _X86_64_THREAD_STATE_H_ */
