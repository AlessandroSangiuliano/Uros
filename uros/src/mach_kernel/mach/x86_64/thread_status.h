/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What a thread's registers look like from outside — x86-64 (#408, moved here
 * by #416).
 *
 * ── Why this file, and not x86_64/thread/state.h ──────────────────────
 *
 * These structures were defined in the kernel's own thread/state.h, which was
 * the right place for everything except them.  thread_get_state() and
 * thread_set_state() carry them across the port interface, so a debugger
 * reads them, an exception handler in another task reads them, and libmach
 * declares them — all compiled separately, none of them able to see a header
 * inside the kernel's machine-dependent tree.
 *
 * The stub generator is what made it obvious: <mach/thread_status.h> reaches
 * for <mach/machine/thread_status.h>, and for this target there was nothing
 * there.  A definition every consumer needs, living where only one of them
 * can find it, is a definition in the wrong file.
 *
 * The kernel's state.h now includes this and keeps what is genuinely its
 * own — the conversions to and from the trap frame, and the checks on bases
 * arriving from userland.
 *
 * ── The reasoning behind the shape (kept from #408) ───────────────────
 *
 * The obvious port takes struct i386_thread_state and makes each field
 * sixty-four bits.  That would carry four segment selectors — ds, es, and the
 * pair fs and gs — into an architecture where segmentation does not exist.
 * In long mode ds and es are ignored entirely, and fs and gs matter only
 * through their *bases*, which live in MSRs and are not what a selector
 * names.
 *
 * A debugger handed those fields would find them meaningless, and userland
 * handed them would fill them in — with a constant, which is exactly how #259
 * happened.  So the selectors are gone and fs_base and gs_base are here
 * instead, as the sixty-four-bit addresses they actually are.  cs and ss
 * stay, alone among the segments, because their low two bits are the
 * privilege level and nothing else records that.
 */

#ifndef	_MACH_X86_64_THREAD_STATUS_H_
#define _MACH_X86_64_THREAD_STATUS_H_

#ifndef	ASSEMBLER

#include <stdint.h>

/*
 * The unit the interface counts in.  Mach passes thread state as an array of
 * natural_t and a count of them, which is why every structure here has to be
 * a whole number of these — asserted in the kernel's state.h, which is where
 * there is a compiler willing to check.
 */
typedef unsigned int state_word_t;

#define x86_64_THREAD_STATE		1
#define x86_64_FLOAT_STATE		2
#define x86_64_EXCEPTION_STATE		3
#define THREAD_STATE_NONE		4

/*
 * The general-purpose registers, the instruction pointer, the flags, the two
 * segment selectors that still mean something, and the two segment bases that
 * replaced the ones that do not.
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
 * never run — the difference between "these are the registers" and "these are
 * whatever the buffer held".
 *
 * A flavour's count has to be a compile-time constant, and the extended
 * processor state is not one — it grows with the features the processor has.
 * So this carries the legacy 512-byte image alone, which is the first 512
 * bytes of the save area under both FXSAVE and XSAVE and is why one flavour
 * serves both.  The vector state above it needs a flavour of its own, and
 * saying so is the difference between a decision and an omission.
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

#endif	/* ASSEMBLER */

#endif	/* _MACH_X86_64_THREAD_STATUS_H_ */
