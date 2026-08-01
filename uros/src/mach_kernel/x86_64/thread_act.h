/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <machine/thread_act.h> for x86-64 (#408).
 *
 * The process control block: everything a thread needs to be put down and
 * picked up again, and the one machine-dependent member the MI activation
 * carries.  kern/thread_act.h expands MachineThrAct inside struct thread_act
 * and the tree reaches the block as thr_act->mact.pcb.
 *
 * ── What is in it, and what i386 has that this does not ──────────────
 *
 * i386's pcb carries four things: two interrupt-state frames, the saved
 * user state, a machine-state block (io_tss, LDT, FPU save area, v86
 * assist) and a lock.  Three of those do not survive the move:
 *
 *   the v86 assist   is virtual-8086 mode, which long mode does not have.
 *   the LDT          long mode ignores the segment bases a per-thread LDT
 *                    existed to set; a thread that wants a base sets FS or
 *                    GS through the MSRs, and #408's state conversion
 *                    already carries those.
 *   the io_tss       on i386 the TSS is switched per thread to carry the
 *                    I/O permission bitmap.  Here the TSS is per-CPU and
 *                    holds the interrupt stacks (IST), so it cannot also be
 *                    a per-thread object.  A thread that needs I/O ports
 *                    will want the bitmap copied into the current CPU's TSS
 *                    on switch — a decision for whoever brings
 *                    i386_io_port_add across, not something to reserve
 *                    space for now.
 *
 * What is left is what x86_64/thread/ already builds: the switch context,
 * and the user frame the trap entry left behind.
 *
 * ⚠️ `context` holds the FPU area pointer already, so the pcb does not carry
 * a second one.  Two places to look for the same state is how one of them
 * ends up stale.
 */

#ifndef _X86_64_THREAD_ACT_H_
#define _X86_64_THREAD_ACT_H_

#include <kern/lock.h>
#include <thread/context.h>
#include <trap/trap.h>

typedef struct pcb {
	/*
	 * Where this thread is when it is not running: the kernel stack
	 * pointer its saved registers sit above, the top of that stack, and
	 * its floating-point area.  context_switch() reads and writes only
	 * this.
	 */
	struct context		ctx;

	/*
	 * The frame trap entry pushed on the way in from ring 3, or NULL for
	 * a thread that has never been in user mode.  This is what
	 * thread_get_state reads and thread_set_state writes; it points into
	 * the thread's own kernel stack rather than being a copy, so there is
	 * one place where the user's registers live and no second copy to
	 * reconcile on the way out.
	 */
	struct trap_frame	*user;

	decl_simple_lock_data(,lock)
} *pcb_t;

/*
 * The storage and the pointer both, as on i386: the block lives in the
 * activation, and `pcb` points at it.  The indirection is what the MI tree
 * uses, and keeping it means code that says thr_act->mact.pcb reads the same
 * on either machine.
 */
typedef struct MachineThrAct {
	struct pcb	xxx_pcb;
	pcb_t		pcb;
} MachineThrAct, *MachineThrAct_t;

/*
 * Whether a swapped-out thread's user stack gets unwired.
 *
 * i386 answers TRUE because the user stacks of collocated servers are wired
 * there.  Nothing is collocated here and nothing wires a user stack, so
 * unwiring on swap-out would be an operation with no subject.  It becomes a
 * real question if collocated servers arrive; until then the honest answer
 * is that there is nothing to unwire.
 */
#define THREAD_SWAP_UNWIRE_USER_STACK	FALSE

#endif /* _X86_64_THREAD_ACT_H_ */
