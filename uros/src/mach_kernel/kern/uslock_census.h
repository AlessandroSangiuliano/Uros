/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Does the MP inter-processor interrupt land on a processor that is holding a
 * simple lock?  (#486)
 *
 * ipi_mp_handler() asks that question already, at i386/lapic.c:
 *
 *	sched_safe = (get_preemption_level() == 0);
 *
 * and cannot answer it.  On i386 get_preemption_level() is `return (0)' --
 * <kern/cpu_data.h>, the arm taken when a machine supplies neither MACH_RT nor
 * MACHINE_PREEMPTION_LEVEL -- so sched_safe is a constant, the compiler folds
 * the branch away, and ast_check() and hertz_tick() run unconditionally.  The
 * guard was added by #316 as a fix, which is to say after a problem was seen;
 * it has never once been able to fire.
 *
 * This is the observable it needed.  It changes no behaviour: it counts, and
 * the count decides what the fix is.
 *
 * ── What the answer would mean ──
 *
 * The hazard is not general re-entrancy.  It is one lock, enumerated from the
 * source rather than feared in the abstract:
 *
 *   - ast_check() takes no simple lock at all.  It raises spl, reads
 *     need_ast[] and csw_needed(), and returns.  The MP_AST branch is safe.
 *   - hertz_tick() -> thread_quantum_update() takes thread->lock in three
 *     places, and all three are simple_lock_try() since #317 -- which is the
 *     same hazard, found and closed once already, on the other lock.
 *   - It also takes pset->quantum_adj_lock (kern/priority.c) with a BLOCKING
 *     simple_lock(), outside any spl raise, and that lock has exactly one
 *     acquirer in the whole kernel: that line.
 *
 * So the deadlock this can produce is a processor spinning on a lock it holds
 * itself: inside thread_quantum_update() from its own LAPIC timer, holding
 * quantum_adj_lock, when the MP IPI arrives -- vector 0xF1, above splsched's
 * TPR class, therefore not masked -- and re-enters hertz_tick().  Cross-CPU
 * contention on that lock is ordinary and resolves; self-re-entry does not.
 *
 * ── Why a depth and a PC, and not the lock address ──
 *
 * The address of thread->lock is a field inside a heap object and resolves to
 * no symbol.  The address of the instruction that ACQUIRED it resolves with
 * nm(1) to the function that holds it, which is the thing being asked about.
 *
 * ── The blind window, said rather than left to be discovered ──
 *
 * 🔴 The depth is incremented AFTER the acquisition succeeds, because a
 * counter raised before a hw_lock_try() would count failures.  So an interrupt
 * landing between hw_lock_try() returning true and the increment reads zero
 * while the lock is in fact held.  A few instructions per acquisition.
 *
 * ⚠️ Which makes the two outcomes carry different weight, and the difference
 * has to be respected when the result is read:
 *
 *	non-zero  =  proof.  The IPI does cut into lock holders.
 *	zero      =  evidence, with a known blind window.  Not proof.
 *
 * ── Scope ──
 *
 * i386 only, by construction rather than by choice: x86-64 supplies its own
 * usimple_lock (x86_64/sync/lock.c, #453), so kern/lock.c does not compile the
 * one this instruments there.  x86-64 has the preemption level for real (#461)
 * and asks this question with it; #462 is what remains open there.
 */

#ifndef	_KERN_USLOCK_CENSUS_H_
#define	_KERN_USLOCK_CENSUS_H_

#include <cpus.h>			/* NCPUS */
#include <kern/cpu_number.h>

#ifndef	USLOCK_CENSUS
#define	USLOCK_CENSUS	0
#endif

#if	USLOCK_CENSUS

/*
 * How many acquisition sites deep the per-processor stack goes, and how many
 * distinct ones the report will name.  Both small on purpose: a processor
 * holding more than a handful of simple locks at once is itself the finding,
 * and the overflow counter says so rather than silently truncating.
 */
#define	USLOCK_CENSUS_DEPTH	16
#define	USLOCK_CENSUS_SITES	8

struct uslock_census_cpu {
	/* Live state, maintained by every acquire and release on this CPU. */
	int		depth;
	unsigned int	overflow;	/* depth ran past the stack	     */
	unsigned int	underflow;	/* a release with nothing held	     */
	unsigned long	pc[USLOCK_CENSUS_DEPTH];

	/*
	 * The narrow question, and the one the deadlock is actually about.
	 *
	 * 🔴 The broad depth above is fooled by any acquisition this kernel
	 * never balances, and there is one: user_bootstrap() takes info->lock
	 * and then calls thread_bootstrap_return(), which does not return.
	 * The processor that ran it reads "a lock is held" for the rest of the
	 * boot, and every sample it takes says 100%.  A running counter over
	 * arbitrary code cannot be made immune to that.
	 *
	 * These two can.  Both bracket straight-line spans inside one function
	 * on one processor with no blocking point in them, so they are
	 * balanced by construction rather than by everyone remembering.
	 */
	int		in_tick;	/* inside thread_quantum_update()    */
	int		in_qadj;	/* holding pset->quantum_adj_lock    */

	/* What ipi_mp_handler() saw when it was entered. */
	unsigned int	entries;	/* IPI handler entries observed	     */
	unsigned int	unsafe;		/* ...of which with a lock held	     */
	unsigned int	tick_hits;	/* ...of which inside the tick	     */
	unsigned int	qadj_hits;	/* ...of which on the deadlock	     */

	/*
	 * What the try-lock that replaced the blocking one actually caught.
	 * qadj_averted is the one that matters: a failed acquisition on a
	 * processor already inside the span is the self-deadlock not happening.
	 */
	unsigned int	qadj_miss;	/* try failed (some processor held)  */
	unsigned int	qadj_averted;	/* ...and it was THIS one, nested    */
	unsigned int	max_depth;	/* deepest ever seen at entry	     */
	unsigned long	site_pc[USLOCK_CENSUS_SITES];
	unsigned int	site_count[USLOCK_CENSUS_SITES];
	unsigned int	site_lost;	/* unsafe entries past the table     */
	unsigned int	reports;	/* how many times we have printed    */
} __attribute__((aligned(64)));

extern struct uslock_census_cpu	uslock_census[NCPUS];

/*
 * Zero until the machine says otherwise.  usimple_lock() runs long before %gs
 * carries a valid CPU_DATA descriptor, and cpu_number() reads through %gs --
 * #482 learned that the hard way, as a triple fault inside the page-fault
 * handler.  Arming is a single store, done once the per-CPU area is real.
 */
extern volatile int		uslock_census_ready;

extern void	uslock_census_arm(void);
extern void	uslock_census_sample(void);
extern void	uslock_census_report(void);

static __inline__ struct uslock_census_cpu *
uslock_census_slot(void)
{
	unsigned int	cpu;

	if (!uslock_census_ready)
		return ((struct uslock_census_cpu *) 0);
	cpu = (unsigned int) cpu_number();
	if (cpu >= (unsigned int) NCPUS)
		return ((struct uslock_census_cpu *) 0);
	return (&uslock_census[cpu]);
}

static __inline__ void
uslock_census_acquired(unsigned long pc)
{
	struct uslock_census_cpu	*c = uslock_census_slot();

	if (c == (struct uslock_census_cpu *) 0)
		return;
	if (c->depth >= 0 && c->depth < USLOCK_CENSUS_DEPTH)
		c->pc[c->depth] = pc;
	else
		c->overflow++;
	c->depth++;
}

static __inline__ void
uslock_census_released(void)
{
	struct uslock_census_cpu	*c = uslock_census_slot();

	if (c == (struct uslock_census_cpu *) 0)
		return;
	if (c->depth <= 0) {
		c->underflow++;
		return;
	}
	c->depth--;
}

/*
 * The two narrow spans.  thread_quantum_update() is what the MP_CLOCK branch
 * of ipi_mp_handler() re-enters, and pset->quantum_adj_lock is the one lock in
 * it that is still taken with a BLOCKING simple_lock() -- thread->lock has been
 * simple_lock_try() since #317, which is this same hazard found and closed once
 * already.  quantum_adj_lock has exactly one acquirer in the whole kernel, so a
 * processor that meets it while already holding it is spinning on itself.
 */
static __inline__ void
uslock_census_tick_enter(void)
{
	struct uslock_census_cpu	*c = uslock_census_slot();

	if (c != (struct uslock_census_cpu *) 0)
		c->in_tick++;
}

static __inline__ void
uslock_census_tick_leave(void)
{
	struct uslock_census_cpu	*c = uslock_census_slot();

	if (c != (struct uslock_census_cpu *) 0 && c->in_tick > 0)
		c->in_tick--;
}

static __inline__ void
uslock_census_qadj_enter(void)
{
	struct uslock_census_cpu	*c = uslock_census_slot();

	if (c != (struct uslock_census_cpu *) 0)
		c->in_qadj++;
}

static __inline__ void
uslock_census_qadj_missed(void)
{
	struct uslock_census_cpu	*c = uslock_census_slot();

	if (c == (struct uslock_census_cpu *) 0)
		return;
	c->qadj_miss++;
	/*
	 * qadj_enter() has already counted this attempt, so a nesting depth
	 * above one means the interrupted code on this very processor is
	 * inside the span too.  Before the try-lock, that is where it stopped.
	 */
	if (c->in_qadj > 1)
		c->qadj_averted++;
}

static __inline__ void
uslock_census_qadj_leave(void)
{
	struct uslock_census_cpu	*c = uslock_census_slot();

	if (c != (struct uslock_census_cpu *) 0 && c->in_qadj > 0)
		c->in_qadj--;
}

#else	/* USLOCK_CENSUS */

/*
 * All six compile to nothing, including the report.  A disabled facility that
 * still leaves a function in the kernel for nobody to call is the shape of
 * defect this very issue is about (kernel_preempt_check), so it does not get
 * introduced here in the same breath: with the census off, the kernel's .text
 * is byte-identical, and that is checked rather than asserted.
 */
#define	uslock_census_acquired(pc)
#define	uslock_census_released()
#define	uslock_census_arm()
#define	uslock_census_sample()
#define	uslock_census_report()
#define	uslock_census_tick_enter()
#define	uslock_census_tick_leave()
#define	uslock_census_qadj_enter()
#define	uslock_census_qadj_leave()
#define	uslock_census_qadj_missed()

#endif	/* USLOCK_CENSUS */

#endif	/* _KERN_USLOCK_CENSUS_H_ */
