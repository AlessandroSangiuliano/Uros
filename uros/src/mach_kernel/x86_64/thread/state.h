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

/*
 * The unit the interface counts in.  Mach passes thread state as an array of
 * natural_t and a count of them, which is why every structure here has to be
 * a whole number of these — asserted below rather than hoped for.
 */
typedef unsigned int state_word_t;

#define x86_64_THREAD_STATE		1
#define x86_64_FLOAT_STATE		2
#define x86_64_EXCEPTION_STATE		3
#define THREAD_STATE_NONE		4

/*
 * The general-purpose registers, the instruction pointer, the flags, the two
 * segment selectors that still mean something, and the two segment bases
 * that replaced the ones that do not.
 */
struct x86_64_thread_state {
	uint64_t rax, rbx, rcx, rdx;
	uint64_t rdi, rsi, rbp, rsp;
	uint64_t r8,  r9,  r10, r11;
	uint64_t r12, r13, r14, r15;
	uint64_t rip;
	uint64_t rflags;
	uint64_t cs;			/* its low two bits are the ring */
	uint64_t ss;
	uint64_t fs_base;		/* thread-local storage lives here */
	uint64_t gs_base;
};

#define x86_64_THREAD_STATE_COUNT \
	(sizeof(struct x86_64_thread_state) / sizeof(state_word_t))

/*
 * The floating-point and SSE registers, in the layout the processor itself
 * writes.  `valid` distinguishes a thread that has state from one that has
 * never run — the difference between "these are the registers" and "these
 * are whatever the buffer held".
 */
struct x86_64_float_state {
	uint32_t valid;
	uint32_t reserved;
	uint8_t  fx_image[512];
};

#define x86_64_FLOAT_STATE_COUNT \
	(sizeof(struct x86_64_float_state) / sizeof(state_word_t))

/*
 * Why a thread stopped, for the handler that has to decide what to do about
 * it.  Separate from the registers because it is not a register: it does not
 * round-trip, and setting it would mean nothing.
 */
struct x86_64_exception_state {
	uint64_t trapno;
	uint64_t err;
	uint64_t faultvaddr;
};

#define x86_64_EXCEPTION_STATE_COUNT \
	(sizeof(struct x86_64_exception_state) / sizeof(state_word_t))

#define MACHINE_THREAD_STATE		x86_64_THREAD_STATE
#define MACHINE_THREAD_STATE_COUNT	x86_64_THREAD_STATE_COUNT
#define THREAD_MACHINE_STATE_MAX	x86_64_FLOAT_STATE_COUNT

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
 */
void thread_state_to_frame(const struct x86_64_thread_state *state,
			   struct trap_frame *frame);

/* The floating-point flavour, out of a save area and back into one. */
void float_state_from_area(const void *area, struct x86_64_float_state *state);
void float_state_to_area(const struct x86_64_float_state *state, void *area);

/* Why it stopped, out of the frame that says so. */
void exception_state_from_frame(const struct trap_frame *frame,
				struct x86_64_exception_state *state);

#endif	/* _X86_64_THREAD_STATE_H_ */
