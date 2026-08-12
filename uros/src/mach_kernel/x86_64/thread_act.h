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

#include <kern/assert.h>	/* Assert(), inside USER_REGS() below */
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

	/*
	 * The segment bases this thread uses, which the frame has nowhere to
	 * keep: they are MSRs and no trap pushes them (#422).
	 *
	 * 🔥 thread_state_from_frame() used to answer with rdmsr() -- the
	 * bases of whatever thread is running -- which is right only when that
	 * is the thread being asked about.  Reading a STOPPED thread reported
	 * the kernel's own gs_base, and the write direction is obliged to
	 * refuse a kernel base (#440), so a read-modify-write of another
	 * thread's state was refused every time.  bootstrap's loader does
	 * exactly that read-modify-write, checks neither return value, and on
	 * i386 never needed to: the name server entered ring 3 with the whole
	 * frame still zero, at rip 0.
	 *
	 * ⚠️ Stored and reported, but NOT yet loaded on a context switch --
	 * that is the second half of TLS on this machine and is not here.  So
	 * a thread that sets a base does not get it applied; what it gets is a
	 * consistent answer when it reads back, which is what the round trip
	 * needs and what the loader was silently failing.
	 */
	uint64_t		fs_base;
	uint64_t		gs_base;

	decl_simple_lock_data(,lock)
} *pcb_t;

#define	PCB_NULL	((pcb_t) 0)

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

/*
 * ── Reaching a thread's user registers (#453) ─────────────────────────
 *
 * The machine-independent tree asks four questions of an activation, and
 * they all resolve to the frame trap entry left behind.
 *
 * ⚠️ USER_REGS() answers a pointer that is NULL for a thread which has never
 * left the kernel -- a kernel thread, or a user thread between creation and
 * its first return to ring 3.  i386 cannot produce that answer, because
 * there the frame is a member of the pcb and so always has an address.  Here
 * it is a pointer into the thread's own stack, which is the arrangement that
 * gives us one copy of the user's registers instead of two, and the price is
 * that the caller has to have a thread that has been to user space.  Every
 * machine-independent caller does -- they run on the exception and system
 * call paths, which start in ring 3 -- and the assertion carried inside the
 * macro is there so that a future one which does not says so immediately,
 * rather than dereferencing NULL several frames further on.
 *
 * The check is written against Assert() rather than assert(), because
 * assert() expands to a statement and USER_REGS() has to stay an expression
 * -- i386's is one, and the machine-independent callers use it inside
 * initialisers and casts.  Under MACH_ASSERT off it becomes the plain member
 * access and costs nothing.
 */
#if	MACH_ASSERT
#define	USER_REGS(ThrAct)						\
	(((ThrAct)->mact.pcb->user != (struct trap_frame *) 0 ? (void) 0 \
	  : Assert(__FILE__, __LINE__, "USER_REGS: thread has no user frame")), \
	 (ThrAct)->mact.pcb->user)
#else	/* MACH_ASSERT */
#define	USER_REGS(ThrAct)	((ThrAct)->mact.pcb->user)
#endif	/* MACH_ASSERT */

#define	act_machine_state_ptr(ThrAct)	((thread_state_t) USER_REGS(ThrAct))

/*
 * Came from ring 3?  The saved code segment says so, and it is the segment
 * rather than the address that decides -- a kernel address in rip proves
 * nothing about where the trap came from.
 *
 * i386 also tests EFL_VM here, for virtual-8086 mode.  Long mode does not
 * have virtual-8086 at all, so there is no second case to ask about; the
 * absence is architectural rather than unfinished.
 */
#define	is_user_thread(ThrAct)	((USER_REGS(ThrAct)->cs & 3) != 0)

#define	user_pc(ThrAct)		(USER_REGS(ThrAct)->rip)
#define	user_sp(ThrAct)		(USER_REGS(ThrAct)->rsp)

/*
 * Nothing to synchronise: system call emulation is a per-task vector the MI
 * code edits under the task lock, and i386 needs this hook only because its
 * emulation vector is reachable through the LDT, which is per-thread and has
 * to be re-pointed on every thread of the task.  There is no LDT here -- the
 * decision recorded in x86_64/cpu/desc.c -- so there is nothing that could
 * be out of date.
 *
 * Defined as (void)(task) rather than empty so that a call site keeps
 * evaluating nothing but still type-checks its argument.
 */
#define	syscall_emulation_sync(task)	((void)(task))

/*
 * Flavor to size in words, indexed by flavor number.  Defined in
 * x86_64/thread/state.c, next to the flavors it measures.
 */
extern unsigned int	state_count[];

#endif /* _X86_64_THREAD_ACT_H_ */
