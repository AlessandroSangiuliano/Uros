/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Where a copy-on-write fault spends its cycles (#482).
 *
 * #407 measured the whole: 5,790 cycles for one copy-on-write fault, of which
 * the page copy is 228.  That is the useful part of the number and the end of
 * what it can say -- whatever the other 96% is, it is not the copy, and every
 * optimisation aimed at the copy works on a twenty-fifth of the problem.  This
 * says which of the remaining phases is worth touching.
 *
 * 🔑 It also exists to check a premise rather than to admire one.  #439
 * narrowed the shootdown on the ARGUMENT that the growth from one processor to
 * eight was the broadcast: the copy did not move a cycle while the fault did,
 * and the shootdown was the one part whose cost is a function of processor
 * count.  The argument was good and the result was 35,490 -> 5,790, and it is
 * still an argument.  A phase measured at one processor and at eight either
 * confirms it or names what else grows.
 *
 * ── The mechanism, and why it cannot report slices that do not add up ──
 *
 * One cursor per processor.  fault_profile_begin() writes a timestamp; every
 * fault_profile_mark() takes another, charges the difference to the phase that
 * was open, and becomes the new cursor.  So the sum of the slices IS the last
 * timestamp minus the first, by construction, and there is no arithmetic that
 * could disagree with itself.  The done-when asks for slices that sum to the
 * whole; this makes producing any other kind impossible rather than asking the
 * reader to check.  [feedback: convert gotchas to mechanism]
 *
 * ⚠️ A phase is charged wherever the cursor happens to be, so a mark that is
 * not reached leaves its predecessor's slice covering the gap.  The fast
 * copy-on-write path is a straight line through all twelve; every other route
 * through vm_fault() runs the same marks and simply never commits, because
 * committing is gated on the copy-on-write branch having been taken.
 *
 * ── What is NOT in here, said rather than left to be discovered ──
 *
 * 🔴 The assembly entry stub.  FP_ENTRY starts at the first C instruction of
 * the trap handler, so the swapgs, the frame save and IRETQ are outside every
 * slice.  They are not lost: cow_test times the same fault from ring 3, and
 * the difference between its whole and this one is exactly that assembly.  Two
 * instruments, and the gap between them is the third measurement.
 *
 * 🔴 rdtsc is not free and this calls it thirteen times per fault.  The dump
 * reports the cost of one back-to-back pair, measured on the machine that is
 * running, beside the count of marks -- so the reader subtracts an instrument
 * they were told the size of, instead of trusting a fault path that has been
 * quietly widened by the act of watching it.  [feedback: the instrument lies]
 */

#ifndef	_KERN_FAULT_PROFILE_H_
#define	_KERN_FAULT_PROFILE_H_

#include <kern/macro_help.h>

/*
 * Off, and off is the shipping configuration.
 *
 * Turning it on puts a timestamp on every trap this kernel takes -- the hook
 * has to be at the top of trap_dispatch(), because "trap entry" is a phase and
 * a phase cannot be measured from after it -- and twelve more inside the fault
 * path.  Worth paying while measuring, not worth shipping.
 *
 *	cmake -DUROS_FAULT_PROFILE=ON
 *
 * ⚠️ AND BUILD IT before believing anything it says.  The bodies below are not
 * compiled while this is 0, and code that is not compiled does not know it is
 * broken.  <sync/mutex_trace.h> carries the same warning for the same reason,
 * and it is there because it happened.
 */
#ifndef	FAULT_PROFILE
#define	FAULT_PROFILE	0
#endif

/*
 * The phases, in the order the fast copy-on-write path walks them.
 *
 * Named for the work rather than for the function, except where the function
 * IS the work: FP_PROTECT and FP_ENTER are named after pmap_page_protect() and
 * PMAP_ENTER because those two are the reason this issue exists -- they are
 * the calls in the path that end in a shootdown, and #482 was opened to find
 * out what their share is at one processor and at eight.
 *
 * 🔥 TWO shootdown sites, not one.  The issue's own list says "the shootdown"
 * in the singular; the code says otherwise.  pmap_page_protect() severs every
 * mapping of the SOURCE page across every space that had it, and PMAP_ENTER
 * installs the copy over whatever the faulting space had there.  #439 narrowed
 * both, and a measurement that folded them together could not say which
 * narrowing paid.
 */
#define	FP_ENTRY	0	/* first C instruction of the handler -> vm_fault() */
#define	FP_MAPLOCK	1	/* vm_map_lock_read()                    */
#define	FP_LOOKUP	2	/* vm_map_lookup_locked()                */
#define	FP_CHAIN	3	/* the walk down the shadow chain        */
/*
 * ⚠️ FP_OBJLOCK is a bucket and not a stretch of the path: the fast
 * copy-on-write path takes and drops object locks in four separate places, and
 * a slice is charged from each.  The issue asks for the locks "separately from
 * what they protect", which is what this is; it is not one contiguous phase and
 * the number is a total rather than a duration.  Same for FP_QUEUES.
 */
#define	FP_OBJLOCK	4	/* vm_object lock/unlock/paging, every one */
#define	FP_ALLOC	5	/* vm_page_alloc() for the new top page  */
#define	FP_COPY		6	/* vm_page_copy() -- the known 228       */
#define	FP_PROTECT	7	/* pmap_page_protect(): SHOOTDOWN        */
#define	FP_COLLAPSE	8	/* vm_object_collapse()                  */
#define	FP_ENTER	9	/* PMAP_ENTER(): SHOOTDOWN               */
#define	FP_QUEUES	10	/* page queues, wakeups, the unlocks     */
#define	FP_RETURN	11	/* vm_fault() returned -> end of handler */
#define	FP_PHASES	12

/*
 * How many faults are kept before the breakdown is printed.
 *
 * ⚠️ Kept, not summed.  A running sum can only ever report a mean, and a mean
 * is what reported a 45% uniprocessor regression that did not exist while #439
 * was being measured: under a hypervisor the tail is the host descheduling the
 * guest, and it moves the mean and not the median.  So the samples are held
 * whole and the dump sorts them.  [technique: measurement discipline]
 *
 * Sixteen because the median of sixteen is worth having and cow_test's timed
 * burst is eight -- one dump would otherwise straddle two of its arms and
 * describe neither.
 */
#define	FP_SAMPLES	16

/*
 * A cap on how many times a processor prints.  The boot takes copy-on-write
 * faults long after the test is done with them, and an instrument that fills
 * the log is one whose output nobody reads: 16,390 lines a boot is what
 * mach_print did, and the condition that produced them had been right once.
 */
#define	FP_MAX_DUMPS	8

#if	FAULT_PROFILE

#include <stdint.h>
#include <cpus.h>			/* NCPUS */
#include <kern/cpu_number.h>

/*
 * One per processor, on its own cache line.
 *
 * ⚠️ Per-processor and not per-thread, which is a choice with a cost: a fault
 * that blocks can resume on another processor, and the cursor would then be
 * somebody else's.  The fast copy-on-write path does not block -- it is the
 * path that exists BECAUSE it does not have to talk to a pager -- so what is
 * left to guard against is a nested trap, and `owner' below is what guards it.
 */
struct fault_profile_cpu {
	uint64_t	cursor;		/* timestamp of the most recent mark */
	/*
	 * Where the sample started, kept only so the dump can compare the sum
	 * of the slices against the interval they were cut out of.
	 *
	 * 🔑 The two are equal by construction -- each mark charges (now -
	 * cursor) and then becomes the cursor, so the sum telescopes to
	 * last minus first -- which is exactly why the comparison is worth
	 * printing: the only way it can fail is a slice that did not fit in
	 * its 32 bits, and that is a fault which sat somewhere for a second.
	 * Asserting the whole value rather than the plausible one is what
	 * found three of the four defects in #472/#474/#477.
	 */
	uint64_t	first;
	const void	*owner;		/* the trap frame this sample is for */
	uint32_t	slice[FP_PHASES];	/* the sample being built    */
	uint32_t	cow;		/* the copy-on-write branch was taken */
	uint32_t	nsamples;	/* how many are in `sample'          */
	uint32_t	ndropped;	/* a nested trap took the cursor     */
	uint32_t	nslow;		/* copy-on-write via vm_fault_page() */
	uint32_t	ndumps;
	uint32_t	sample[FP_SAMPLES][FP_PHASES];
	uint32_t	total[FP_SAMPLES];
	char		pad[64];	/* nothing else shares this line     */
} __attribute__((aligned(64)));

extern struct fault_profile_cpu	fault_profile[NCPUS];

/*
 * The clock.
 *
 * 🔴 NOT cpuid+rdtsc, which is the textbook way to serialise this and is a VM
 * exit: #439 measured one at 1,920 cycles against 1 for the ordinary read --
 * a third of an entire copy-on-write fault, charged to whichever phase was
 * open.  An instrument that costs a third of its subject is not measuring it.
 *
 * lfence orders the earlier loads and costs a handful of cycles.  It is SSE2,
 * so it is written only for the target that is guaranteed to have it; the
 * other x86 target takes the unserialised read, which is honest for what runs
 * there -- nothing commits a sample on i386, because the hook that opens one
 * lives in the x86-64 trap path.
 */
static __inline__ uint64_t
fault_profile_tsc(void)
{
	uint32_t	lo, hi;

#if	defined(__x86_64__)
	__asm__ __volatile__("lfence; rdtsc" : "=a" (lo), "=d" (hi) :: "memory");
#elif	defined(__i386__)
	__asm__ __volatile__("rdtsc" : "=a" (lo), "=d" (hi) :: "memory");
#else
#error	"fault_profile has no time source on this machine"
#endif
	return ((uint64_t) hi << 32) | lo;
}

/*
 * Whether asking which processor this is, is a question with an answer yet.
 *
 * 🔥 THIS FLAG IS WHY THE KERNEL BOOTS, and the reason is sharper than "not
 * initialised yet".  cpu_number() reads the id out of this processor's per-CPU
 * block through %gs, and before percpu_activate() the segment base is zero --
 * so the read goes to address zero.  The received wisdom, written down after
 * #439, is that this CANNOT FAIL: address zero is identity-mapped early in
 * boot, so it returns a byte of the interrupt vector table as a processor
 * number, and a plausible answer is worse than none.
 *
 * That is true only while the kernel is running in the kernel's own address
 * space.  x86_64/boot/boot_c.c builds a test pmap whose lower half is EMPTY --
 * it prints "lower half empty" and then runs several self-tests inside it --
 * and in that space address zero is not mapped at all.  So there cpu_number()
 * does not lie, it FAULTS: a page fault raised inside the trap handler, which
 * re-enters the trap handler, which reads %gs again.  The machine went silent
 * after 71 lines of a 256-line boot with nothing printed, because there was no
 * path left that could print.
 *
 * 🔑 So the guard cannot be a bound on the answer -- the first version was,
 * and it changed nothing.  It has to be a question that does not go through
 * %gs at all: a word in the kernel's own image, which every address space maps
 * because they all share the kernel half.
 *
 * The NCPUS bound below stays as the second line, for an application processor
 * that traps between coming up and reaching its own percpu_activate(): there
 * the low half IS mapped, so the read is the old failure mode -- a plausible
 * number -- and a plausible number is what a bound is for.
 */
extern volatile int	fault_profile_ready;

/*
 * This processor's slot, or nothing at all.
 *
 * Returning nothing is the honest answer rather than a convenient one: before
 * the per-CPU block exists there is no processor identity to charge a sample
 * to, so there is no sample.
 */
static __inline__ struct fault_profile_cpu *
fault_profile_slot(void)
{
	unsigned int	cpu;

	if (!fault_profile_ready)
		return (struct fault_profile_cpu *) 0;

	cpu = (unsigned int) cpu_number();
	if (cpu >= (unsigned int) NCPUS)
		return (struct fault_profile_cpu *) 0;
	return &fault_profile[cpu];
}

/*
 * Open a sample, owned by this trap's frame.  Called from the trap handler's
 * first C instruction, for every trap -- most of which are not copy-on-write
 * faults and will simply never commit.
 *
 * 🔑 The frame pointer is the nesting guard, and it is one because it costs
 * nothing to be right.  The alternative -- a depth counter -- has to be
 * decremented on every one of trap_dispatch()'s ten exits, and the exit that
 * gets forgotten is the one that poisons every later sample silently.  Here an
 * interrupt taken inside the fault path arms a sample of its own with its own
 * frame, and the outer commit finds an owner that is not its own and drops
 * itself.  Nothing has to be remembered anywhere.
 *
 * ⚠️ That the outer sample is DROPPED and not merely inaccurate is the point.
 * The nested handler's cycles really did pass inside whatever phase was open,
 * so charging them there is true and misleading at once -- the phase is not
 * what spent them.  The count of drops is worth reading on its own: it says
 * how often a copy-on-write fault gets interrupted.
 */
static __inline__ void
fault_profile_begin(const void *token)
{
	struct fault_profile_cpu	*fp = fault_profile_slot();
	int				i;

	if (fp == (struct fault_profile_cpu *) 0)
		return;

	for (i = 0; i < FP_PHASES; i++)
		fp->slice[i] = 0;
	fp->cow = 0;
	fp->owner = token;
	fp->cursor = fp->first = fault_profile_tsc();
}

/*
 * Close the phase that was open and charge it.  `phase' names the slice being
 * CLOSED, not the one being entered, so a mark reads as "that much went here".
 */
static __inline__ void
fault_profile_mark(int phase)
{
	struct fault_profile_cpu	*fp = fault_profile_slot();
	uint64_t			now;

	if (fp == (struct fault_profile_cpu *) 0)
		return;

	now = fault_profile_tsc();
	fp->slice[phase] += (uint32_t) (now - fp->cursor);
	fp->cursor = now;
}

/*
 * This fault took the copy-on-write branch, so the shape of the sample being
 * built is the shape the breakdown describes.  Nothing else commits.
 */
static __inline__ void
fault_profile_cow(void)
{
	struct fault_profile_cpu	*fp = fault_profile_slot();

	if (fp != (struct fault_profile_cpu *) 0)
		fp->cow = 1;
}

/*
 * ... and this one left the fast path after saying so -- vm_page_alloc()
 * refused, or the loop broke out into vm_fault_page().  The sample is a
 * fragment and a fragment must not be committed as a whole.
 */
static __inline__ void
fault_profile_abandon(void)
{
	struct fault_profile_cpu	*fp = fault_profile_slot();

	if (fp != (struct fault_profile_cpu *) 0)
		fp->cow = 0;
}

extern void	fault_profile_commit(const void *token);
extern void	fault_profile_slow(void);
extern void	fault_profile_dump(void);

/*
 * Said once, by the machine, when asking which processor this is has become a
 * question with an answer.  Everything above is inert until then.
 */
#define	FP_READY()	(fault_profile_ready = 1)
#define	FP_BEGIN(t)	fault_profile_begin(t)
#define	FP_MARK(p)	fault_profile_mark(p)
#define	FP_COW()	fault_profile_cow()
#define	FP_ABANDON()	fault_profile_abandon()
#define	FP_COMMIT(t)	fault_profile_commit(t)
#define	FP_SLOW()	fault_profile_slow()

#else	/* !FAULT_PROFILE */

#define	FP_READY()	MACRO_BEGIN MACRO_END
#define	FP_BEGIN(t)	MACRO_BEGIN MACRO_END
#define	FP_MARK(p)	MACRO_BEGIN MACRO_END
#define	FP_COW()	MACRO_BEGIN MACRO_END
#define	FP_ABANDON()	MACRO_BEGIN MACRO_END
#define	FP_COMMIT(t)	MACRO_BEGIN MACRO_END
#define	FP_SLOW()	MACRO_BEGIN MACRO_END

/*
 * Compiled in both ways, and it tells the truth in the one that is not built.
 *
 * 🔑 A kernel without the instrument must not be able to look like a kernel
 * with it and nothing to say.  So the name exists either way, and the version
 * that was not built says so instead of printing an empty table.
 */
extern void	fault_profile_dump(void);

#endif	/* FAULT_PROFILE */

#endif	/* _KERN_FAULT_PROFILE_H_ */
