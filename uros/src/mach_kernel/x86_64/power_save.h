/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <power_save.h> for x86-64 (#450, #461).
 *
 * Whether an idle processor may stop instead of spinning.
 *
 * This answered 0 until #461, and the zero was a statement of fact rather than
 * a default: there was no idle path in x86_64/ at all, so kern/sched_prim.c's
 * three `#if POWER_SAVE' calls to machine_idle_wake() had nothing to reach.
 * Declaring the names here to make the header look complete would have moved
 * the failure from the compiler to the linker, and a build that links is not
 * the place to discover that a processor never halts.
 *
 * It said it would flip in the same change that gave the target an idle loop.
 * That change is #461 -- the application processors are in the scheduler now,
 * and three of them turning a `pause' loop at full rate is not a machine at
 * rest.  The implementation is x86_64/cpu/idle.c, and the declarations arrive
 * with it, as this header said they would.
 *
 * ⚠️ i386 also exports sched_idle_hlt, cleared by the -S boot flag.  It stays
 * absent here: the halt is closed by a Dekker pair rather than by a flag
 * somebody can turn off, and a control whose only use is to work around a race
 * is a race that has not been fixed.
 */

#ifndef _X86_64_POWER_SAVE_H_
#define _X86_64_POWER_SAVE_H_

#define POWER_SAVE	1

#ifndef __ASSEMBLER__

#include <stdint.h>
#include <cpu/spl.h>		/* #526: SPL0 */

/*
 * #526: the level the idle loop waits at, whatever level it found.  The idle
 * thread holds nothing when a pass starts, so zero is always right for it --
 * and a raised level it restored instead was held for ever: the processor's
 * own tick deferred, and on the master every timeout in the machine with it.
 */
#define	MACHINE_IDLE_SPL	SPL0

/*
 * #526: the idle loop found this processor's level above MACHINE_IDLE_SPL as
 * it went to wait.  Recorded, with the raise from zero that left it there,
 * for machine_idle() to say once; called at splsched, so it only records.
 */
void		machine_idle_found_level(unsigned level);

/* One fruitless pass of the idle loop; may halt once it has had enough. */
void		machine_idle(int mycpu);

/* The idle stint is over -- this processor has a thread to run. */
void		machine_idle_exit(int mycpu);

/*
 * Knock, because the target may be halted.  Called by thread_setrun() at
 * splsched, immediately after publishing next_thread and the DISPATCHING
 * state -- the ordering matters and is explained in x86_64/cpu/idle.c.
 */
void		machine_idle_wake(int cpu);

/*
 * How many times a processor has halted, and how many doorbells it has sent.
 *
 * Counted because the failure this mechanism can have is silent by nature: a
 * processor that halts and is never woken looks exactly like a processor with
 * nothing to do.  Naps that never grow mean the halt is not being reached;
 * naps far larger than the machine's knocks mean processors are waking for
 * something else, which is a cost nobody asked for.
 */
uint64_t	machine_idle_naps(int cpu);
uint64_t	machine_idle_knocks(int cpu);

#endif	/* __ASSEMBLER__ */

#endif /* _X86_64_POWER_SAVE_H_ */
