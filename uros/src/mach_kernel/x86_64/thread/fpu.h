/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Floating-point and vector state (#408, MD contract 3/6).
 *
 * ══ The decision: eager, not lazy ═════════════════════════════════════
 *
 * The 32-bit kernel saves this state lazily.  On a switch it sets CR0.TS;
 * the first floating-point instruction the new thread executes raises a
 * device-not-available fault, and the handler saves whoever owned the
 * registers and loads whoever wants them.  A thread that never touches the
 * unit never pays.
 *
 * x86-64 does not do that, and the reasons are not preference.
 *
 * **The premise is false here.**  Lazy saving pays off when most threads do
 * not use the unit.  Under the System V AMD64 ABI, floating-point arguments
 * are passed in XMM registers, and the compiler uses them for structure
 * copies and inlined string work in code that has no floating point in it
 * at all.  Essentially every thread touches the register file within a few
 * instructions of starting.  So the saving never arrives, and what is left
 * is a trap on every switch — the lazy path costing more than the thing it
 * was avoiding.
 *
 * **And it is a disclosure vulnerability.**  Between a switch and the fault
 * that would service it, the registers still hold the previous thread's
 * values, and a processor may read them speculatively before the fault is
 * delivered — CVE-2018-3665.  The architectural answer from the vendor is
 * to restore eagerly.  A design that is slower *and* leaks is not a trade.
 *
 * **The hardware does the laziness properly now.**  XSAVEOPT and its
 * relatives write only the components that have actually changed, which is
 * the optimisation CR0.TS was reaching for, decided by the processor from
 * what it knows rather than by the kernel from a trap.
 *
 * ══ What this means for the kernel's own code ═════════════════════════
 *
 * The kernel is compiled without SSE (`-mgeneral-regs-only`) and stays that
 * way.  Under the lazy model that was a requirement — kernel SSE would
 * either fault or quietly corrupt a user thread's registers.  Under this
 * one it is merely a good idea, and it keeps its value: the kernel never
 * dirties the state it is holding for someone else, so a switch is the only
 * place any of this happens.
 */

#ifndef _X86_64_THREAD_FPU_H_
#define _X86_64_THREAD_FPU_H_

#ifndef __ASSEMBLER__

#include <stdint.h>

/*
 * Turn the unit on for this processor and decide how the state will be
 * moved.  Per processor: these are control registers, and an application
 * processor arrives with its own copies as reset left them.
 */
void fpu_init(void);

/*
 * How much room one thread's state needs, and how well aligned.
 *
 * Not a constant, because it is not one: with XSAVE the size comes from the
 * processor and grows with the features enabled — 576 bytes for the legacy
 * registers, more once vectors are in the picture.  Asking is the only way
 * to be right on a part that has more of them than this one.
 */
uint64_t fpu_area_size(void);

#define FPU_AREA_ALIGN	64

/* Whether the extended form is in use, or the fixed 512-byte legacy one. */
int fpu_uses_xsave(void);

/*
 * Which instruction actually moves the state, by name.
 *
 * Reported rather than assumed, because the three differ in what they cost
 * and the difference is invisible from outside: a kernel taking the legacy
 * path on a processor that offered better would look exactly like one that
 * had chosen it.  That is how the optimised form went unnoticed here for a
 * day — the emulated processor in the test loop does not offer it.
 */
const char *fpu_save_instruction(void);

/*
 * Move one thread's state out of the registers, and another's in.
 *
 * The area must be FPU_AREA_ALIGN-aligned and at least fpu_area_size()
 * bytes — the instructions fault rather than misbehave if it is not, which
 * is the right way round.
 */
void fpu_save(void *area);
void fpu_restore(const void *area);

/*
 * Give an area the state a thread should start with, rather than whatever
 * the memory held.
 *
 * A thread that begins with another thread's registers is a disclosure with
 * extra steps, and one that begins with uninitialised memory is worse: the
 * restore instruction rejects some bit patterns and accepts others as
 * exceptions waiting to be raised at the first arithmetic.
 */
void fpu_area_init(void *area);

/*
 * One area per thread, allocated because its size is what this processor
 * needs rather than a constant, and aligned because XSAVE raises #GP on an
 * area that is not (#453).  fpu_area_alloc() returns it already initialised.
 */
void *fpu_area_alloc(void);
void  fpu_area_free(void *area);

#endif	/* __ASSEMBLER__ */

#endif	/* _X86_64_THREAD_FPU_H_ */
