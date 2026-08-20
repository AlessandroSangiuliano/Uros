/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Per-CPU data, reached through %gs (#409/#408).
 *
 * Long mode drops the segmentation i386 used for this and replaces it with
 * a base held in an MSR, so %gs:0 is simply "this CPU's block" with no
 * descriptor involved.  The region it lives in is the per-CPU area of
 * docs/uros_design.md ch.11 §11.2, which the layout has reserved since #407
 * and nothing has used until now.
 *
 * Shared ground between two contracts: the trap path needs it because
 * swapgs has to have something to swap to, and the context switch needs it
 * for everything that is "this CPU's" rather than "this thread's".  #409
 * lands it; #408 reviews it, as the issue asks.
 */

#ifndef _X86_64_CPU_PERCPU_H_
#define _X86_64_CPU_PERCPU_H_

/*
 * The offsets the entry path needs, before anything C.  Assembly cannot ask
 * the compiler where a field is, so the numbers live here and the assertions
 * below keep them honest.
 */
#define PERCPU_SELF		0
#define PERCPU_CPU_ID		8
#define PERCPU_KERNEL_RSP	16
#define PERCPU_USER_RSP		24
#define PERCPU_PREEMPT_LEVEL	48

#ifndef __ASSEMBLER__

#include <stdint.h>

struct percpu {
	/*
	 * First on purpose: %gs-relative addressing can reach a field, but
	 * getting the block's own address needs it stored somewhere, and
	 * offset zero makes that one load.
	 */
	struct percpu *self;

	uint32_t cpu_id;
	uint32_t reserved;

	/*
	 * The two words the syscall entry touches, kept together and kept
	 * near the front.
	 *
	 * SYSCALL does not switch the stack — that is the kernel's job, and
	 * it is the first thing the entry does, before it has anywhere to put
	 * anything.  So the stack to switch *to* and the place to park the
	 * one being switched *from* both have to be reachable through %gs
	 * alone, and they are read and written within a few instructions of
	 * each other on the hottest path in the system.  Adjacent means one
	 * cache line pays for both.
	 */
	uint64_t kernel_rsp;		/* what a syscall switches to */
	uint64_t user_rsp;		/* where the user's waits meanwhile */

	/*
	 * Who was running here immediately before the thread that is running
	 * here now (#453).
	 *
	 * This is how switch_context() keeps the promise its interface makes.
	 * Mach's switch_context() answers with the thread that was running
	 * before the CALLER RESUMED -- not the one it was handed, which the
	 * caller already has.  When A switches away to B, A's answer is not
	 * known yet: it is whoever eventually switches back to A, and that may
	 * be a thread on a processor that does not exist yet.
	 *
	 * So the switcher leaves its own identity here on the way out, and the
	 * thread it resumes reads it on the way in.  The write and the read
	 * are on the SAME processor -- the one doing the switch -- with the
	 * switch between them, which is why a per-CPU slot is enough and no
	 * lock is needed: nothing else can run here in between.
	 *
	 * ⚠️ Read after the switch returns, never before.  Before the switch
	 * it holds the previous switch's answer, which is a real thread
	 * pointer and therefore a wrong answer rather than an obviously
	 * missing one.
	 */
	void	*prev_thread;

	/*
	 * Which thread this processor is running.
	 *
	 * Kept here rather than derived from the stack pointer, the way some
	 * kernels do by masking it down to the base of the stack: that trick
	 * needs kernel stacks aligned to their own size, and these are
	 * allocated by the machine-independent side, which makes no such
	 * promise (#453).
	 */
	void	*active_thread;

	/*
	 * How many reasons this processor has not to change hands (#461).
	 *
	 * ⚠️ THE KERNEL BELIEVED IT ALREADY HAD THIS.  <kern/cpu_data.h> defines
	 * disable_preemption() as nothing and get_preemption_level() as the
	 * constant 0 whenever MACH_RT is off, which it is here -- so every
	 * caller that thought it was holding preemption off was holding nothing
	 * off, and the guard #459 wrote into the trap return (`do not preempt
	 * inside a critical section') tested a constant.
	 *
	 * It cost a deadlock, and the shape is worth keeping: a thread is
	 * preempted while holding a spin lock; the other processors take the
	 * same lock from interrupt context, where the gate has already cleared
	 * IF; and now nobody can make progress, because the only thing that
	 * would release the lock is a thread that no processor is free to
	 * schedule.  Four processors, one instruction, interrupts off.
	 * Measured at one boot in six before idle processors halted, and four in
	 * six after -- a halted processor is one fewer that might have run the
	 * holder.
	 *
	 * Here rather than in cpu_data[] because it is read on the trap return
	 * path, which has %gs and would otherwise pay for an array index first.
	 */
	uint32_t preemption_level;
	uint32_t reserved_preempt;

	/*
	 * The interrupt priority level, and what arrived while it was raised
	 * (#409/#322).
	 *
	 * Here rather than in a static array indexed by processor, because the
	 * whole point of the software level is that reading and writing it
	 * costs one %gs-relative instruction — an array would cost the index
	 * first, and the index is the thing %gs already is.
	 *
	 * One bit per vector, so deferral is exact: a class-wide flag would
	 * replay every vector in a class because one of them arrived.
	 */
	uint32_t ipl;
	uint32_t reserved_ipl;
	uint64_t pending[4];
	uint64_t deferred;
	uint64_t replayed;

	/*
	 * Which address space this processor has in CR3 (#439).
	 *
	 * A `struct pmap *', kept as void * so that the whole pmap interface
	 * does not have to be visible to everything that includes this — the
	 * same reason <pmap/tlb.h> forward-declares it.
	 *
	 * 🔑 It exists so that pmap_activate() can maintain the per-pmap
	 * processor set from ONE place.  The alternative was to have the
	 * caller supply both the space being left and the space being entered,
	 * and that is the shape this tree keeps getting hurt by: two halves
	 * that must agree, kept in step by whoever remembers.  A processor
	 * already knows what it is running; asking it costs one %gs-relative
	 * load and cannot disagree with itself.
	 *
	 * ⚠️ Added at the END.  The offsets asserted below are what the
	 * assembly entry paths use, and a field inserted above them would move
	 * every one of them silently.
	 */
	void *loaded_pmap;
};

/*
 * Checked against the structure rather than trusted: a field added in the
 * middle would otherwise move the offsets above silently, and the first
 * symptom would be a syscall running on a stack that is not a stack.
 */
_Static_assert(__builtin_offsetof(struct percpu, self) == PERCPU_SELF,
	       "percpu self moved");
_Static_assert(__builtin_offsetof(struct percpu, cpu_id) == PERCPU_CPU_ID,
	       "percpu cpu_id moved");
_Static_assert(__builtin_offsetof(struct percpu, kernel_rsp) == PERCPU_KERNEL_RSP,
	       "percpu kernel_rsp moved");
_Static_assert(__builtin_offsetof(struct percpu, preemption_level)
	       == PERCPU_PREEMPT_LEVEL, "percpu preemption_level moved");
_Static_assert(__builtin_offsetof(struct percpu, user_rsp) == PERCPU_USER_RSP,
	       "percpu user_rsp moved");

/*
 * Two halves, and the split is not tidiness.
 *
 * percpu_alloc() maps a processor's page, which means editing the kernel's
 * page tables.  percpu_activate() fills the block and points %gs at it,
 * which touches only that processor's own page and its own MSR.
 *
 * If a processor did both for itself on the way in, every one of them would
 * be editing the same page tables at the same moment — two arriving
 * together could both find an intermediate table missing and both build
 * one.  So the boot processor allocates for everybody before waking anyone,
 * and an arriving processor only activates.  Prepare, then release.
 *
 * ⚠️ percpu_activate() must run after the last load of %gs as a segment
 * register — the GDT setup does one — because that zeroes the base it
 * writes.  Ordered the other way, %gs:0 reads address zero, which early in
 * boot is mapped, so it fails quietly rather than faulting.
 */
void percpu_alloc(uint32_t cpu_id);
void percpu_activate(uint32_t cpu_id);

/* This CPU's block, via the pointer it keeps at offset zero. */
static inline struct percpu *percpu(void)
{
	struct percpu *p;

	__asm__ volatile("movq %%gs:0, %0" : "=r"(p));
	return p;
}


/*
 * This processor's local APIC id, without paying CPUID for it (#439).
 *
 * 🔥 cpu_apic_id() in <cpu/regs.h> asks the hardware, and asking costs a
 * CPUID -- unconditionally serialising, and under KVM an exit to the
 * hypervisor every single time.  It was on the shootdown path and on the
 * address-space switch, and it cost about 45% of a copy-on-write fault on ONE
 * processor: a measurement that says nothing about shootdowns at all, because
 * a machine with one processor sends none.
 *
 * percpu_activate() is handed the APIC id and stores it, so the answer is
 * already here and reading it is one %gs-relative load.  CPUID remains the
 * right call exactly once per processor -- when there is no block yet to read
 * it from, which is how it got in there.
 */
static inline uint32_t percpu_apic_id(void)
{
	return percpu()->cpu_id;
}

/*
 * Entering and leaving a spin-lock section (#461).
 *
 * Plain increments, not atomics: the counter belongs to one processor and is
 * only ever written by it.  What it has to survive is an INTERRUPT on this
 * processor between the read and the write, and a read-modify-write to a
 * %gs-relative address is a single instruction, which an interrupt cannot
 * land inside.
 *
 * ⚠️ Every acquire needs its release.  A counter left standing disables
 * preemption on this processor for good, and the symptom is not a crash but a
 * processor that quietly stops sharing -- so the pair belongs in the lock
 * package and nowhere else.
 */
static inline void percpu_preempt_disable(void)
{
	__asm__ volatile("incl %%gs:%c0"
			 : : "i"(PERCPU_PREEMPT_LEVEL) : "memory");
}

static inline void percpu_preempt_enable(void)
{
	__asm__ volatile("decl %%gs:%c0"
			 : : "i"(PERCPU_PREEMPT_LEVEL) : "memory");
}

static inline uint32_t percpu_preempt_level(void)
{
	uint32_t d;

	__asm__ volatile("movl %%gs:%c1, %0"
			 : "=r"(d) : "i"(PERCPU_PREEMPT_LEVEL));
	return d;
}

#endif	/* __ASSEMBLER__ */

#endif	/* _X86_64_CPU_PERCPU_H_ */
