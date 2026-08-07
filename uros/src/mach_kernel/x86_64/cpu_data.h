/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <machine/cpu_data.h> for x86-64: the preemption level (#461).
 *
 * ── WHY THIS EXISTS WITHOUT MACH_RT ──────────────────────────────────
 *
 * <kern/cpu_data.h> reaches for this header only when MACH_RT is on, and
 * MACH_RT is off here.
 *
 * What MACH_RT is, from this source rather than from memory: the real-time
 * configuration.  conf/files says so in as many words -- "MACH_RT is
 * real-time.  MACH_TR is debugging.  Unfortunate choice of letters." -- and it
 * has two limbs, of which only the first is anywhere near this file.
 *
 *   kernel preemption	 kern/lock.h: with it on, "locks denote critical,
 *			 non-preemptable points in the code".
 *			 i386/AT386/mp/mp.h is where DISABLE_PREEMPTION stops
 *			 expanding to nothing and starts moving a per-processor
 *			 level, and i386/i386_lock.S annotates each primitive
 *			 with what that means for its contract.
 *
 *   real-time IPC	 ipc_object_is_rt(), a real-time attribute on a port,
 *			 and out-of-line data taken from preallocated buffers
 *			 rather than the general allocator (ipc/ipc_init.c).
 *			 IP_RT(port) in kern/rpc.c is that attribute reaching
 *			 every allocation on the RPC path.
 *
 * It is a real axis and not a bundle with something else: conf/AT386/FAST+RT
 * differs from conf/AT386/FAST by the single line `options MACH_RT'.
 *
 * So turning it on is a decision about the whole kernel, across some hundreds
 * of gated lines and both targets, and the second limb is a question about IPC
 * that this system will want to ask properly one day rather than inherit by
 * flipping a switch.  It is not the decision in front of us.  The preemption LEVEL alone is, and this machine needs it for a
 * reason that stands on its own: since #459 this kernel preempts on the way
 * out of any trap, including in ring 0, and something has to be able to say
 * "not here".  So the machine supplies the counter and leaves the rest alone.
 *
 * ── WHAT IT COST TO NOT HAVE IT ──────────────────────────────────────
 *
 * With MACH_RT off, <kern/cpu_data.h> defines disable_preemption() as nothing
 * and get_preemption_level() as the constant 0 -- and the machine-independent
 * kernel calls the first in dozens of places and the trap return tests the
 * second.  Every one of those spans announced a property that nothing was
 * keeping.  It arrived as a deadlock: a thread preempted while holding a spin
 * lock, the other processors reaching for that lock from interrupt context
 * where the gate has already cleared IF, and nothing left able to schedule the
 * holder.  Four processors, one instruction, interrupts off.
 *
 * i386 has the same empty definitions and does not have the same problem: as
 * built, it does not preempt in kernel mode at all.  The machinery is all
 * there -- kernel_preempt_check() in i386/trap.c, called from
 * enable_preemption() in i386/cpu_data.h and from the ENABLE_PREEMPTION macro
 * in i386/AT386/mp/mp.h -- and every one of those sites is inside `#if
 * MACH_RT'.  Switched off, not absent.
 */

#ifndef _X86_64_CPU_DATA_H_
#define _X86_64_CPU_DATA_H_

#include <cpu/percpu.h>

#ifndef __ASSEMBLER__

/*
 * ⚠️ Valid only after percpu_activate() has run on this processor -- the same
 * condition <machine/cpu_number.h> carries, and for the same reason: before it,
 * %gs's base is zero and address zero is mapped this early, so a premature call
 * answers a plausible number instead of faulting.
 *
 * Early boot is single-threaded and has no scheduler, so nothing there can be
 * preempted anyway; the window is real but there is nothing in it.
 */
static __inline__ int get_preemption_level(void)
{
	return (int) percpu_preempt_level();
}

static __inline__ void disable_preemption(void)
{
	percpu_preempt_disable();
}

/*
 * ⚠️ NO AST IS TAKEN HERE, and the machine-independent name suggests otherwise.
 *
 * Mach's enable_preemption() is entitled to switch away on the spot when the
 * level reaches zero with work pending.  Doing that from an arbitrary C call
 * site means synthesising the frame the scheduler expects -- i386 does it by
 * raising a software interrupt -- and every unlock in the kernel becomes a
 * place a thread can vanish, including the ones reached with interrupts off.
 *
 * So the AST is left to the trap return, which already runs it and already has
 * a real frame.  What that costs is latency, and the cost is bounded and
 * small: the clock ticks at 100 Hz, so a quantum that expired inside a critical
 * section is collected within ten milliseconds rather than at the closing
 * bracket.  Written down because it is a deliberate weakening of the interface,
 * not an oversight -- and because the day it is not good enough, the fix is the
 * synthesised frame and not a change here.
 */
static __inline__ void enable_preemption(void)
{
	percpu_preempt_enable();
}

static __inline__ void enable_preemption_no_check(void)
{
	percpu_preempt_enable();
}

static __inline__ void mp_disable_preemption(void)
{
	percpu_preempt_disable();
}

static __inline__ void mp_enable_preemption(void)
{
	percpu_preempt_enable();
}

static __inline__ void mp_enable_preemption_no_check(void)
{
	percpu_preempt_enable();
}

#endif	/* __ASSEMBLER__ */

#endif /* _X86_64_CPU_DATA_H_ */
