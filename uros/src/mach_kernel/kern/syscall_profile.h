/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Where a Mach trap's time goes, phase by phase (#411, for #392).
 *
 * #392 has to decide whether register-IPC (#391) earns its complexity, and the
 * only thing that can decide it is the fraction of a message's time that the
 * two copies actually are.  Everything else in the path is the fixed floor
 * register-IPC does not touch, and a floor is not something to guess at.
 *
 * This is #411's half of that: the trap path instrumented so that #392 adds
 * marks inside mach_msg and nothing here has to be rebuilt.  The two
 * boundaries #392 cannot reach from machine-independent code -- the entry
 * stub, and the return after the trap function has returned -- are the reason
 * this cannot simply be written later, in ipc/.
 *
 * ── Why this is not fault_profile with different names ────────────────
 *
 * 🔴 <kern/fault_profile.h> keeps its cursor PER PROCESSOR, and says why it is
 * allowed to: "the fast copy-on-write path does not block".  That is true
 * there and false here.  A mach_msg receive parks the thread; the hand-off
 * switches to the receiver.  A per-processor cursor would be picked up by
 * whichever thread the processor ran next, and the sample would be an
 * arithmetic result computed from two different threads' timestamps -- which
 * is not a wrong number so much as a number about nothing.
 *
 * So the sample lives on the THREAD.  The per-CPU slot is used for exactly one
 * hand-off, from the assembly entry to the first C instruction that can reach
 * current_thread(), which is the same trick and the same justification the
 * entry already uses for the user stack pointer: safe across two instructions
 * for a reason rather than by luck.
 */

#ifndef	_KERN_SYSCALL_PROFILE_H_
#define	_KERN_SYSCALL_PROFILE_H_

#include <kern/macro_help.h>

/*
 * Off, and off is the shipping configuration.
 *
 *	cmake -DUROS_SYSCALL_PROFILE=ON
 *
 * ⚠️ AND BUILD IT before believing anything it says.  The bodies below are not
 * compiled while this is 0, and code that is not compiled does not know it is
 * broken.  <kern/fault_profile.h> carries this warning because it happened
 * there; it is repeated rather than referenced because the person who needs it
 * is reading this file.
 */
#ifndef	SYSCALL_PROFILE
#define	SYSCALL_PROFILE	0
#endif

/*
 * The phases, in the order a Mach trap walks them.
 *
 * 🔑 TWO, and #392 asks for six.  Three of the four it asks for that are not
 * here -- name→port resolution, and the two copies -- are subdivisions of
 * SP_BODY, and they are deliberately NOT declared as empty phases; the fourth
 * is the return, which is measured elsewhere for the reason below.
 *
 * A phase that is declared and never marked prints a zero, and a zero in a
 * breakdown reads as "this costs nothing" rather than as "nobody measured
 * this".  That is the instrument lying about its own coverage, which is worse
 * than the instrument being coarse.  SP_BODY is coarse and honest: it is
 * everything between the dispatcher entering the trap function and that
 * function returning, and #392's first act is to split it -- which is what
 * #482 learned when a bucket named for allocation turned out to be 44% a
 * debugging scan.  A bucket whose number is too big for its name gets divided.
 */
#define	SP_ENTRY	0	/* SYSCALL -> the trap function's first C     */
#define	SP_BODY		1	/* the trap function itself (#392 splits this) */
#define	SP_PHASES	2

/*
 * 🔴 And the return is NOT one of them, which is a decision and not an
 * omission.
 *
 * #392's phase six runs from the trap function returning to the SYSRET, and
 * every instruction of it is assembly.  Reaching this structure from there
 * means putting the offset of a field of `struct thread' into entry.S -- and
 * <x86_64/syscall/syscall.c> already refuses exactly that, in as many words,
 * for the Mach trap table: an entry path carrying another structure's layout
 * as constants is #448's shape, and the day the structure gains a field the
 * assembly reads whatever is now at that offset, correctly, for ever.
 *
 * So the return is measured where it can be measured honestly: entry.S times
 * it against itself and accumulates into the PER-PROCESSOR block, which
 * assembly may reach by an offset that IS static-asserted against the
 * structure.  It is reported as an aggregate beside the table rather than as
 * a column inside it.
 *
 * 🔑 Nothing is lost by that.  A per-sample column exists to be correlated
 * with the message -- a big copy makes a big slice -- and the return path
 * runs the same instructions whatever the message was.  A number that cannot
 * vary with its subject does not need to be attributed to one.
 */

/*
 * How many timestamps one trap costs: the one the entry stub takes, plus one
 * per phase.  Derived, so that adding a phase cannot leave the dump reporting
 * an overhead figure computed from a stale literal.
 */
#define	SP_MARKS	(SP_PHASES + 1)

/*
 * How many traps are kept before the breakdown is printed.
 *
 * ⚠️ Kept, not summed: a running sum can only report a mean, and under an
 * emulator the tail is the host descheduling the guest -- it moves the mean
 * and not the median.  The samples are held whole and the dump sorts them.
 *
 * Sixteen, and the number was MEASURED rather than chosen.  It began at
 * sixty-four, reasoning that a Mach trap is cheap enough that a wider window
 * costs nothing -- and a boot then printed nothing at all, because no single
 * thread on this target makes sixty-four profiled traps before the run ends.
 * A window that never fills is an instrument that is silent for a reason its
 * reader cannot see, which is the worst thing an instrument can be.
 *
 * ⚠️ So the count of traps SEEN is printed beside the sixteen.  A breakdown
 * that does not say what fraction of the traffic it looked at invites the
 * reader to assume it looked at all of it.
 */
#define	SP_SAMPLES	16

/*
 * A cap on how many times a thread prints.  A boot makes tens of thousands of
 * traps; an instrument that fills the log is one nobody reads.
 */
#define	SP_MAX_DUMPS	4

/*
 * Which trap the sample is of.
 *
 * 🔴 Not "every trap".  #392 is about the mach_msg hot path, and a breakdown
 * averaged over mach_port_deallocate, mach_thread_self and mach_msg together
 * describes none of them.  A sample is opened only for the trap whose number
 * is armed.
 *
 * ⚠️ A variable and not a constant, so that a debugger or a later console
 * command can point it somewhere else without a rebuild -- but it defaults to
 * urmach_msg, which is what this exists for.  There is no console command
 * today, and saying there was one would be describing a thing that does not
 * exist as though it did.
 */
extern int	syscall_profile_trap;

#if	SYSCALL_PROFILE

#include <stdint.h>

/*
 * One per thread, hung off struct thread, and therefore safe across the
 * blocking this path does by design.
 */
struct syscall_profile_thread {
	uint64_t	cursor;		/* timestamp of the most recent mark */
	/*
	 * Where the sample started.  Kept so the dump can compare the sum of
	 * the slices against the interval they were cut out of: the two are
	 * equal by construction -- each mark charges (now - cursor) and then
	 * becomes the cursor, so the sum telescopes -- which is exactly why
	 * printing the comparison is worth the space.  The only way it can
	 * fail is a slice that did not fit in its 32 bits, and that is a trap
	 * that sat somewhere for a second.
	 */
	uint64_t	first;
	uint32_t	slice[SP_PHASES];
	uint32_t	sample[SP_SAMPLES][SP_PHASES];
	uint32_t	total[SP_SAMPLES];
	uint32_t	nsamples;
	uint32_t	ndumps;
	uint32_t	ndropped;	/* a trap opened while one was open  */
	uint32_t	nseen;		/* profiled traps this thread made   */
	uint32_t	open;		/* a sample is being built           */
};

/*
 * The clock.
 *
 * 🔴 NOT cpuid+rdtsc.  #439 measured a CPUID at 1,920 cycles against 1 for the
 * ordinary read -- and a Mach trap is a few hundred cycles all told, so the
 * textbook serialisation would cost several times the entire subject.  lfence
 * orders the earlier loads for a handful of cycles.
 *
 * ⚠️ x86-64 only, deliberately: lfence is SSE2, and nothing on i386 opens a
 * sample because the hook that opens one is in the x86-64 entry stub.
 */
static __inline__ uint64_t
syscall_profile_tsc(void)
{
	uint32_t	lo, hi;

#if	defined(__x86_64__)
	__asm__ __volatile__("lfence; rdtsc" : "=a" (lo), "=d" (hi) :: "memory");
#else
#error	"syscall_profile has no time source on this machine"
#endif
	return ((uint64_t) hi << 32) | lo;
}

extern void	syscall_profile_dump(struct syscall_profile_thread *);

/*
 * Open a sample from the timestamp the ENTRY STUB took.
 *
 * 🔑 The stub's timestamp and not one taken here.  "Trap entry" is a phase,
 * and a phase cannot be measured from after it: everything the stub does --
 * swapgs, the stack switch, writing the frame -- is precisely the fixed floor
 * #392 exists to weigh against the copies.  Taking the clock at the first C
 * instruction would report that floor as zero and flatter every conclusion
 * drawn from it.
 */
static __inline__ void
syscall_profile_begin(struct syscall_profile_thread *p, uint64_t entry_tsc)
{
	int	i;

	/*
	 * ⚠️ A trap arriving while one is open means the sample in hand cannot
	 * be finished, and finishing it anyway would charge one trap's phases
	 * with another's time.  Counted rather than merged: a count of dropped
	 * samples is a fact about the run, and a merged sample is a lie about
	 * the path.
	 */
	if (p->open) {
		p->ndropped++;
		return;
	}

	p->nseen++;
	p->open = 1;
	p->first = entry_tsc;
	p->cursor = entry_tsc;
	for (i = 0; i < SP_PHASES; i++)
		p->slice[i] = 0;
}

static __inline__ void
syscall_profile_mark(struct syscall_profile_thread *p, int phase)
{
	uint64_t	now;

	if (!p->open)
		return;

	now = syscall_profile_tsc();
	p->slice[phase] += (uint32_t) (now - p->cursor);
	p->cursor = now;
}

/*
 * Close the sample, and print once the window is full.
 */
static __inline__ void
syscall_profile_commit(struct syscall_profile_thread *p)
{
	uint32_t	total = 0;
	int		i;

	if (!p->open)
		return;

	syscall_profile_mark(p, SP_BODY);
	p->open = 0;

	for (i = 0; i < SP_PHASES; i++) {
		p->sample[p->nsamples][i] = p->slice[i];
		total += p->slice[i];
	}
	p->total[p->nsamples] = total;

	if (++p->nsamples == SP_SAMPLES) {
		p->nsamples = 0;
		if (p->ndumps < SP_MAX_DUMPS) {
			p->ndumps++;
			syscall_profile_dump(p);
		}
	}
}

/*
 * The two lines a trap adds, and the only two #392 has to write into
 * ipc/mach_msg.c.
 *
 * ⚠️ Functions and not macros reaching into the thread, so that the caller
 * needs neither <kern/thread.h> nor the machine-dependent header that knows
 * where the entry stub left its timestamp.  A hook whose call site has to
 * include the world is a hook that gets put in the wrong place.
 */
extern void	syscall_profile_enter(int trap_number);
extern void	syscall_profile_leave(void);

/*
 * What the entry stub left in this processor's block, machine-dependent by
 * nature: only the stub knows when the trap began.
 */
extern uint64_t	syscall_profile_entry_tsc(void);

/*
 * And what the return path has cost, which only the entry stub can measure and
 * only the per-processor block can hold — see the note above SP_PHASES.
 */
extern void	syscall_profile_return_cycles(uint64_t *cycles, uint64_t *count);

#define	SP_ENTER(n)	syscall_profile_enter(n)
#define	SP_LEAVE()	syscall_profile_leave()

#else	/* !SYSCALL_PROFILE */

#define	SP_ENTER(n)	MACRO_BEGIN MACRO_END
#define	SP_LEAVE()	MACRO_BEGIN MACRO_END

#endif	/* SYSCALL_PROFILE */

#endif	/* _KERN_SYSCALL_PROFILE_H_ */
