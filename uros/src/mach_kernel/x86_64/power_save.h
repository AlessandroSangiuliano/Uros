/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <power_save.h> for x86-64 (#450).
 *
 * Whether an idle processor may stop instead of spinning.  i386 answers 1 and
 * declares the three entry points the scheduler calls -- machine_idle,
 * machine_idle_exit, machine_idle_wake.
 *
 * x86-64 answers 0, and that is a statement of fact rather than a default:
 * there is no idle path in x86_64/ at all, so kern/sched_prim.c's three
 * `#if POWER_SAVE` calls to machine_idle_wake() have nothing to reach.
 * Declaring them here to make the header look complete would move the failure
 * from the compiler to the linker, and a build that links is not the place to
 * discover that a processor never halts.
 *
 * This flips to 1 in the same change that gives the target an idle loop --
 * which is a scheduler question (#408 owns thread state and the switch),
 * not a header one.  The declarations belong with the implementation, and
 * arrive together or not at all.
 *
 * ⚠️ i386 also exports sched_idle_hlt, cleared by the -S boot flag.  It is
 * absent here for the same reason: a flag that turns off something that does
 * not run is a control the operator cannot trust.
 */

#ifndef _X86_64_POWER_SAVE_H_
#define _X86_64_POWER_SAVE_H_

#define POWER_SAVE	0

#endif /* _X86_64_POWER_SAVE_H_ */
