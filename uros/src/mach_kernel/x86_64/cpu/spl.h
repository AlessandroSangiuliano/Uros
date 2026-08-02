/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Interrupt priority, per processor and in software (#409, with #322).
 *
 * ── Why this is not a write to the task-priority register ─────────────
 *
 * The local APIC has a register for exactly this: the task priority blocks
 * every vector whose priority class is at or below it, in hardware, per
 * processor. The obvious implementation of spl is to program it.
 *
 * The 32-bit kernel did that first (#311) and then stopped (#322), for a
 * measured reason: the register is memory-mapped, so every spl transition
 * is an MMIO access, and under virtualisation an MMIO access to the APIC is
 * a trip out of the guest — about eighteen microseconds under qemu's
 * virtual APIC. spl is called on nearly every path in a kernel. Paying a VM
 * exit for a priority change is paying an interrupt's worth of time to
 * decide whether to take an interrupt.
 *
 * So the register stays at zero, every vector is allowed to arrive, and the
 * level is enforced where it is cheap: a byte in this processor's own block,
 * reached through %gs in one instruction. An interrupt that arrives while
 * its class is masked is acknowledged, recorded, and run when the level
 * drops.
 *
 * ⚠️ The hardware path is not deleted, it is unused — the levels below are
 * the APIC's own priority classes, so `splx` could program the register
 * instead and mean the same thing. That is what makes the choice reversible
 * and measurable rather than a fork in the design.
 *
 * ── The levels are the vector space ───────────────────────────────────
 *
 * A vector's priority class is its top four bits, which is the hardware's
 * definition and not ours. So a level *is* a class, an interrupt is blocked
 * when its class is at or below the current level, and the assignment of
 * vectors to devices is the assignment of priorities. There is no second
 * table to keep in step.
 *
 * ⚠️ Deferring an edge-triggered interrupt acknowledges it to the interrupt
 * controller without servicing the device. The device goes on asserting, so
 * nothing is lost — but a device that needs its condition cleared before it
 * will interrupt again is a device that must not be deferred, and the day
 * one exists this needs to know about it.
 *
 * ⚠️ And a replayed handler is handed a frame with only its vector valid,
 * because the registers of the moment it should have run in are gone. That
 * is a contract on handlers — see trap_replay_vector() — and it is the
 * second reason a handler cannot simply be marked deferrable without being
 * read.
 */

#ifndef _X86_64_CPU_SPL_H_
#define _X86_64_CPU_SPL_H_

#include <stdint.h>

typedef unsigned int spl_t;

/*
 * Everything through, and nothing maskable through.
 *
 * SPLHI is fifteen and not sixteen on purpose: class fifteen is where the
 * cross-processor calls and the timer live, and a level that blocked those
 * would let one processor stop answering the others while it held a lock —
 * which is a deadlock rather than a priority.
 */
#define SPL0		0u
#define SPL_DEVICE	4u	/* the legacy lines, at vector 0x40 and up */
#define SPLHI		14u

/* The class a vector belongs to, which is the hardware's own definition. */
#define spl_of_vector(v)	((unsigned)(v) >> 4)

/* This processor's current level. */
spl_t splget(void);

/*
 * Raise or lower to `level`, returning what it was.
 *
 * Lowering runs whatever arrived while it was raised, in order of falling
 * priority, before it returns — so a caller that lowers has the same view it
 * would have had if nothing had been deferred.
 */
spl_t splx(spl_t level);

static inline spl_t splhi(void)
{
	return splx(SPLHI);
}

static inline spl_t spl0(void)
{
	return splx(SPL0);
}

/*
 * Whether an arriving vector must wait, and the note that it did.
 *
 * Called from the dispatch path with interrupts already off. `spl_defer`
 * records the vector and answers whether it was taken — false means the
 * caller should run the handler now.
 */
int spl_defer(unsigned vector);

/*
 * ── The names the machine-independent tree calls (#453) ───────────────
 *
 * <kern/spl.h> declares each of these parenthesised -- `extern spl_t
 * (splsched)(void)' -- precisely so that a machine may make them macros, and
 * here they are one: a level and a call to splx(), which is a byte written
 * through %gs.  A function per name would be a call into another translation
 * unit to pass a constant.
 *
 * ⚠️ What the levels are is worth reading rather than assuming, because i386
 * is not the guide it looks like.  There, spl.S sends splclock, splvm,
 * splsched and splhigh to the *same* label, SPL7, and spltty, splnet and
 * splimp to another, SPL6.  The names outnumber the levels by three to one,
 * and have since before this kernel existed: they record which subsystem a
 * caller thought it was protecting, not a distinct priority.
 *
 * ⚠️⚠️ This is a BRIDGE, and it is written down as one rather than dressed
 * up as a design.
 *
 * It is kept only because the callers were written against it -- 27 sites
 * call splsched and 11 splhigh -- and a level that blocked less than i386's
 * would silently change what each of them excludes, on a tree whose SMP
 * locking has not been re-examined for this machine.  It is not kept because
 * it is good.
 *
 * What a modern design would do differently, and what it would be worth:
 *
 *   - splsched at SPLHI means taking a scheduler lock blocks the TIMER.
 *     That is where clock drift and scheduling latency come from, and no
 *     kernel written this century does it: the modern shape is a per-CPU
 *     run-queue lock with interrupts disabled across a few instructions,
 *     not a priority raised across a whole critical section.
 *   - spl protects by priority, which excludes nothing on another
 *     processor.  On a machine with 64 of them it is the locks that do the
 *     work and spl is only about this processor's own interrupts -- so most
 *     of these calls are asking for something narrower than they say.
 *
 * Both are real changes with correctness consequences, they belong after the
 * MI tree links and runs here, and they want a measurement in front of them.
 * Until then the bridge stays and says what it is.
 *
 * The classes come from the vector space (see above), so `device' is where
 * the legacy lines live and SPLHI is everything maskable.
 */
#define	splhigh()	splx(SPLHI)
#define	splsched()	splx(SPLHI)
#define	splclock()	splx(SPLHI)
#define	splvm()		splx(SPLHI)

#define	spltty()	splx(SPL_DEVICE)
#define	splnet()	splx(SPL_DEVICE)
#define	splimp()	splx(SPL_DEVICE)
#define	splbio()	splx(SPL_DEVICE)

#define	spllo()		splx(SPL0)

/*
 * The two that are not levels at all: they close and open the interrupt
 * flag itself, for the few places that need the processor to take nothing
 * whatever -- an IDT reload, the last steps of a shutdown.
 *
 * ⚠️ sploff() answers the level, like its neighbours, so that a caller can
 * pair it with splx(); the interrupt flag it also cleared is restored by
 * splon(), which is why the two must be used together and not crossed with
 * splx().  splon() answers nothing, and that asymmetry is in <kern/spl.h>
 * too: there is no previous flag state to hand back that the caller did not
 * already have.
 */
spl_t	sploff(void);
void	splon(spl_t level);

/* How many interrupts this processor has deferred and replayed, for the
 * tests and, later, for whoever wants to know whether the levels are
 * costing anything. */
uint64_t spl_deferred_count(void);
uint64_t spl_replayed_count(void);

#endif	/* _X86_64_CPU_SPL_H_ */
