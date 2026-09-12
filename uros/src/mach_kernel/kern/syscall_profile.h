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
 *
 * ── #392: the body, split ─────────────────────────────────────────────
 *
 * #411 left one bucket called SP_BODY covering everything the trap actually
 * does, and said in as many words that #392's first act is to divide it.  This
 * is that division: the six phases #392 asks for, with the hand-off divided
 * further because the measurement that reopened this issue said to.
 *
 * 🔑 WHY FURTHER.  Comparing x86-64 against the 20/06 i386 baseline on one
 * machine, under one accelerator, at one clock, the gap did not track message
 * SIZE at all -- 4096 bytes is within noise of a null message on every row:
 *
 *	comb   1.16x slower, and FASTER at four processors
 *	inter  1.38x	slow  1.46x	intra  1.48x
 *	trap mach_null 3.7x FASTER    port alloc+destroy 3.4x FASTER
 *
 * ❌ The first reading of that table was "the gap tracks how many THREAD
 * SWITCHES the path does", with comb as the row that does not switch.  That is
 * WRONG and the benchmark's own source says so: comb is the only suite that
 * issues a combined send-and-receive, so it is the only one that reaches this
 * hot path at all -- and reaching it means switching DIRECTLY to the receiver.
 * The other three send and receive in two separate traps, which cannot take
 * this path and wake their peer through the run queue instead.
 *
 * 🔑 So the axis is not how many switches, it is WHICH MECHANISM: a direct
 * hand-off against a run-queue round trip.  comb, the row that uses the
 * hand-off, is the row that degraded least.
 *
 * Either way the hand-off needs dividing -- a column folding "claim the
 * receiver", "the switch" and "the sleep" together cannot separate what is
 * inside it.  [#482: a bucket whose number is too big for its name gets
 * divided, not explained]
 *
 * ⚠️ And the two copies get a column each, named for the copy and not for the
 * function around it, because their SUM is the ceiling on what register-IPC
 * (#391) could ever reclaim.  Everything else in the table is the floor #391
 * does not touch.
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
 * The phases, in the order the combined send-and-receive hot path walks them.
 *
 * 🔴 SP_BODY IS LAST AND IT IS THE RESIDUE, not "the trap".  Every mark below
 * names the stretch it CLOSES, so a path that skips a mark leaves that stretch
 * with whatever mark closes next -- and a trap that takes none of them, because
 * it fell off the hot path at the first test, lands wholly in SP_BODY.  That is
 * the honest arrangement: the slow path is not silently averaged into columns
 * named after fast-path work, it sits in a column named after nothing in
 * particular, where it can be recognised.
 *
 * ⚠️ A phase that is declared and never marked prints a zero, and a zero reads
 * as "this is free" rather than as "nobody measured this".  Which is why the
 * one phase #392 asks for that cannot be marked from C -- the return, from the
 * trap function returning to the SYSRET -- is NOT declared here.  See below.
 */
#define	SP_ENTRY	0	/* SYSCALL -> the trap function's first C     */
/*
 * Getting a buffer, and filling it.  Two columns and not one: ikm_cache_get()
 * is the allocation the mmot path already avoids, and #392's own text says so
 * -- "the naive L4 win is already half-present here".  Half-present is a claim
 * with a number, and this is the number.
 */
#define	SP_GET		1	/* a buffer for the message: ikm_cache_get   */
#define	SP_COPYIN	2	/* copyinmsg: 🔥 THE SEND-SIDE COPY           */
#define	SP_RESOLVE	3	/* name -> port: cache, lock-free, or table  */
#define	SP_QUEUE	4	/* rights, queue limits, the two mqueue locks */
/*
 * The hand-off in three, because the issue's "receiver claim + handoff
 * (switch_context, thread_dispatch)" is one phrase covering three costs that
 * scale with entirely different things:
 *
 *   SP_CLAIM  -- the scheduler state change under thread_lock, the sender put
 *		  on the reply queue, the receiver taken off the destination
 *		  queue.  Locks and list surgery.
 *   SP_WAIT   -- the sender is off the processor.  🔴 NOT a cost of the
 *		  mechanism: it is the other end doing its work.  It is here so
 *		  that the other columns can be a share of something real, and
 *		  it is the column that must never be added to the others.
 *   SP_SWITCH -- from "about to call switch_context" to "running as the thread
 *		  that was switched to, past thread_dispatch".  The address
 *		  space change, the register file, the stack.
 *
 * 🔑 SP_SWITCH is the only one of the twelve that CANNOT be cut from a single
 * thread's own timestamps, because from the sender's side the switch and the
 * sleep are one interval -- switch_context() returns when somebody switches
 * back.  So the thread giving up the processor stamps the clock into the
 * INCOMING thread's sample, and the incoming thread charges the interval on
 * the other side of that stamp to the switch.  The two halves of the number are
 * taken on two different threads and that is what makes it a number.
 */
#define	SP_CLAIM	5	/* claim the receiver, park the sender       */
#define	SP_WAIT		6	/* off the processor -- the OTHER end's time */
#define	SP_SWITCH	7	/* 🔥 switch_context + thread_dispatch        */
#define	SP_RESUME	8	/* back with a reply: ith_state, the trailer */
#define	SP_COPYOUT	9	/* the header translated: ports -> names     */
#define	SP_PUT		10	/* copyoutmsg: 🔥 THE RECEIVE-SIDE COPY       */
#define	SP_BODY		11	/* everything the marks above did not name   */
#define	SP_PHASES	12

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
#define	SP_MAX_DUMPS	16

/*
 * 🔥 AND WHICH WINDOWS THOSE ARE, which is not a detail: with the cap alone
 * they are the FIRST eight, and the first eight windows of a thread's life are
 * the wrong hundred traps.
 *
 * The first run of this split proved it by printing twelve columns of zeroes.
 * Every dump said "16 of 64 traps this thread made" and every phase inside
 * mach_msg was 0 -- not because the hot path is never taken, but because the
 * first sixty-four traps a thread makes are its STARTUP: looking a port up,
 * registering with the name server, none of them a combined send-and-receive.
 * The benchmark's ten thousand iterations begin after them and the instrument
 * had already stopped printing.
 *
 * 🔑 So the windows are spaced geometrically -- 1, 2, 4, 8, 16, 32, 64, 128 --
 * which costs the same eight dumps and reaches trap two thousand.  Early
 * behaviour and steady-state behaviour both get looked at, and neither is
 * assumed to stand for the other.
 *
 * ⚠️ A dump cannot name the suite it landed in.  It names the window and the
 * trap numbers, and the log's own suite headers say where those fall.
 *
 * 🔑 Sixteen dumps and not eight, for a reason that is about the SUBJECT and
 * not about the instrument: ipc_bench's busiest thread makes tens of thousands
 * of traps, and only one of its suites uses a combined send-and-receive -- the
 * only kind of trap that can reach the hand-off.  Stopping at window 128 stops
 * before it.  Threads that make fewer traps than that still print fewer dumps;
 * the cap costs nothing where it is not reached.
 */
#define	SP_WINDOW_DUE(w)	((w) != 0 && ((w) & ((w) - 1)) == 0)

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
	/*
	 * When the processor was handed to this thread, written by whichever
	 * thread gave it up.  🔑 The one field of this structure a DIFFERENT
	 * thread writes, and the reason is above SP_CLAIM: it is the only way
	 * the switch can be separated from the sleep.
	 *
	 * ⚠️ Unsynchronised on purpose.  The writer is the thread that is about
	 * to stop running on this processor and the reader is the thread that is
	 * about to start; there is no third party and no ordering question.  A
	 * lock here would be a lock taken inside the interval being measured.
	 */
	uint64_t	switch_in;
	/*
	 * Whose breakdown this is.
	 *
	 * 🔥 Added because a run printed two hundred dumps that were
	 * indistinguishable from each other, and the question that mattered --
	 * WHICH of the twenty-five threads these sixteen traps belonged to --
	 * had no answer anywhere in the output.  Only one kind of thread in
	 * that run takes the hot path; without a name, its breakdown could not
	 * be found among the ones that do not.
	 */
	const void	*self;
	uint32_t	slice[SP_PHASES];
	uint32_t	sample[SP_SAMPLES][SP_PHASES];
	uint32_t	total[SP_SAMPLES];
	/*
	 * Whether the sample in that slot went through the hand-off.
	 *
	 * 🔴 Kept per sample rather than filtered at the door, because #411
	 * learned the hard way that discarding samples discards the subject: the
	 * first version of this profile threw away every trap that slept and
	 * printed nothing at all, since in an RPC somebody always sleeps.  So
	 * nothing is discarded -- the window is described instead.  A median
	 * taken over a window mixing hand-offs with traps that fell off the hot
	 * path at the first test describes neither.
	 */
	uint8_t		hot[SP_SAMPLES];
	uint32_t	nsamples;
	uint32_t	nwindows;	/* how many have been filled so far  */
	uint32_t	ndumps;
	uint32_t	ndropped;	/* a trap opened while one was open  */
	uint32_t	nblocked;	/* how many of them went to sleep    */
	uint32_t	waiting;	/* off the processor right now       */
	uint32_t	nseen;		/* profiled traps this thread made   */
	uint32_t	open;		/* a sample is being built           */
	uint32_t	took_handoff;	/* this sample reached the switch    */
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
	p->waiting = 0;
	p->took_handoff = 0;
	p->first = entry_tsc;
	p->cursor = entry_tsc;
	for (i = 0; i < SP_PHASES; i++)
		p->slice[i] = 0;
}

/*
 * Charge the open stretch to `phase' AT a timestamp somebody else took.
 *
 * 🔑 The telescoping is what makes this safe to expose: every charge is
 * (stamp - cursor) and then cursor becomes stamp, so the slices still sum to
 * last-minus-first whoever read the clock.  What it must never do is move the
 * cursor BACKWARDS -- a stale stamp would make the next slice enormous and the
 * one before it negative-as-unsigned -- so a stamp that is not ahead of the
 * cursor is refused rather than trusted.
 */
static __inline__ void
syscall_profile_mark_at(struct syscall_profile_thread *p, int phase,
			uint64_t stamp)
{
	if (!p->open || stamp <= p->cursor)
		return;

	p->slice[phase] += (uint32_t) (stamp - p->cursor);
	p->cursor = stamp;
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
 * 🔥 The wait is a phase, and getting there took two wrong answers.
 *
 * The first run reported a median Mach trap of three hundred MILLION cycles.
 * The arithmetic was right: that is what a blocking receive costs -- a thread
 * waiting for its next message -- and the body phase was swallowing the sleep.
 *
 * ❌ The first fix was to DISCARD any sample that blocked, reasoning that a
 * receive which sleeps is a different event from the hot path.  A boot then
 * printed nothing at all, which is the measurement that refuted it: in an RPC
 * somebody always blocks.  The client sends and waits for its reply; the
 * server waits for the next request.  Discarding every trap that sleeps
 * discards the entire subject.
 *
 * 🔑 So the sleep is separated rather than avoided.  What #392 needs is not a
 * trap that never waited -- there is no such trap -- it is the wait charged to
 * its own column, so that body means work and the floor means floor.
 *
 * ⚠️ syscall_profile_waiting() is called for the thread being switched AWAY
 * from, not for current_thread(), which by then is somebody else.
 *
 * 🔥 #392: and it stamps the clock into the INCOMING thread's sample, which is
 * the half of the switch measurement the outgoing side owns.  `phase' is what
 * the stretch up to here belongs to -- SP_BODY for an ordinary block, SP_CLAIM
 * for the hand-off, where the work just done has a name.
 */
static __inline__ void
syscall_profile_switch_out(struct syscall_profile_thread *p,
			   struct syscall_profile_thread *next, int phase)
{
	uint64_t	now = syscall_profile_tsc();

	/*
	 * The stamp is written whether or not the OUTGOING thread is being
	 * profiled: the thread coming back is a different thread, and whether
	 * its switch can be measured must not depend on who it displaced.
	 */
	if (next != (struct syscall_profile_thread *) 0)
		next->switch_in = now;

	if (!p->open || p->waiting)
		return;

	syscall_profile_mark_at(p, phase, now);
	p->waiting = 1;
	p->nblocked++;
}

/*
 * And the other end of it, on the thread that has just come back.
 *
 * ⚠️ TWO call sites, because Mach has two ways to resume: a thread with no
 * continuation returns from switch_context(), and a thread with one appears
 * in thread_continue() having never returned from anywhere.  Hooking only the
 * first is the mistake that leaves a server thread's whole sleep charged to
 * whatever phase was open -- and a server thread is the one that always has a
 * continuation.  #392 adds a third: the mmot hand-off calls switch_context()
 * itself, inline, and returns from it inside ipc/mach_msg.c without passing
 * through either of the other two.
 *
 * 🔴 The sleep is closed AT the stamp the other thread took, so everything
 * after that stamp is still open and belongs to the switch.  Closing it at
 * "now" instead is the version that reports the switch as free, which is the
 * one number this whole issue turns on.
 */
static __inline__ void
syscall_profile_resumed(struct syscall_profile_thread *p)
{
	if (!p->open || !p->waiting)
		return;

	/*
	 * ⚠️ A stamp that is not ahead of the cursor means this thread resumed
	 * without a hand-off that could be timed -- it was never switched to by
	 * an instrumented path, or the stamp is from a previous sample.  Then
	 * the two cannot be separated, and the whole interval goes to the WAIT.
	 *
	 * 🔴 That direction is the deliberate one.  The switch is the number
	 * this issue turns on; a fallback that guessed in its favour would be an
	 * instrument arranging its own conclusion.  Understating it costs a
	 * sample, overstating it costs the finding.
	 */
	if (p->switch_in > p->cursor)
		syscall_profile_mark_at(p, SP_WAIT, p->switch_in);
	else
		syscall_profile_mark(p, SP_WAIT);
	p->waiting = 0;
}

/*
 * ... and the switch itself is closed one call later, after thread_dispatch(),
 * because the issue asks for both and because a thread that has been switched
 * to is not yet running its own code until the thread it displaced has been
 * disposed of.
 */
static __inline__ void
syscall_profile_switched(struct syscall_profile_thread *p)
{
	if (!p->open || p->waiting)
		return;

	syscall_profile_mark(p, SP_SWITCH);
}

/*
 * This sample reached the commit point of the hand-off: it is a sample of the
 * path this issue is about, and not of a trap that fell off it.
 */
static __inline__ void
syscall_profile_handoff(struct syscall_profile_thread *p)
{
	if (p->open)
		p->took_handoff = 1;
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
	p->hot[p->nsamples] = (uint8_t) p->took_handoff;

	if (++p->nsamples == SP_SAMPLES) {
		p->nsamples = 0;
		p->nwindows++;
		if (p->ndumps < SP_MAX_DUMPS && SP_WINDOW_DUE(p->nwindows)) {
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
 * The scheduler's lines.  A thread about to give up the processor cannot be a
 * sample of what a trap costs; a thread that has just been given one owes its
 * sample the two halves of the switch.
 *
 * ⚠️ syscall_profile_blocked() takes BOTH threads rather than reading
 * current_thread(), because at that call site the answer would be wrong for
 * one of them and about to be wrong for the other.
 */
struct thread_shuttle;
extern void	syscall_profile_blocked(struct thread_shuttle *old,
					struct thread_shuttle *next,
					int phase);
extern void	syscall_profile_back(void);
extern void	syscall_profile_switched_in(void);
extern void	syscall_profile_took_handoff(void);
extern void	syscall_profile_phase(int phase);

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
#define	SP_BLOCKED(o,n,p) syscall_profile_blocked(o, n, p)
#define	SP_BACK()	syscall_profile_back()
#define	SP_SWITCHED()	syscall_profile_switched_in()
#define	SP_HANDOFF()	syscall_profile_took_handoff()
#define	SP_MARK(p)	syscall_profile_phase(p)

#else	/* !SYSCALL_PROFILE */

#define	SP_ENTER(n)	MACRO_BEGIN MACRO_END
#define	SP_LEAVE()	MACRO_BEGIN MACRO_END
#define	SP_BLOCKED(o,n,p) MACRO_BEGIN MACRO_END
#define	SP_BACK()	MACRO_BEGIN MACRO_END
#define	SP_SWITCHED()	MACRO_BEGIN MACRO_END
#define	SP_HANDOFF()	MACRO_BEGIN MACRO_END
#define	SP_MARK(p)	MACRO_BEGIN MACRO_END

#endif	/* SYSCALL_PROFILE */

#endif	/* _KERN_SYSCALL_PROFILE_H_ */
