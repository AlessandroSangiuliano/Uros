/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Making another processor do something (#438).
 */

#include <stdint.h>

#include <kern/misc_protos.h>	/* #461: halt_cpu, panic */
#include <kern/ast.h>		/* #603: ast_check */
#include <kern/cpu_data.h>	/* #638: disable_preemption */

#include <cpu/answer_count.h>	/* #605: who answered, shared with tlb.c */
#include <cpu/ipi.h>
#include <cpu/lapic.h>
#include <cpu/percpu.h>
#include <cpu/regs.h>
#include <cpu/smp.h>
#include <sync/atomic.h>
#include <sync/barrier.h>
#include <sync/lock.h>
#include <time/tsc.h>		/* #638: the widened windows' clock */
#include <trap/trap.h>


/*
 * #603: the AST interrupt back to an EOI and nothing else, as it was.
 */
#ifndef ABLATE_603_IPI_EMPTY
#define ABLATE_603_IPI_EMPTY	0
#endif

/*
 * One call in flight at a time, and the lock is what makes that true.
 *
 * A per-processor mailbox would let several run at once, and is what this
 * will become when there is a caller that wants it.  It is not this: the
 * only caller so far is a shootdown, which needs every processor to answer
 * before it can continue, so a second cross-call could not usefully start
 * while the first is outstanding anyway.  What the single slot buys is that
 * the receiving side has nothing to search — it reads one place, and it is
 * the right one.
 */
static hw_lock_data_t call_lock;

static void (* volatile call_fn)(void *);
static void * volatile call_arg;
static volatile uint64_t call_acks;

/* Per-processor, so a silent one can be named rather than merely counted. */
static struct answer_count served;

/*
 * 🔴 WHO DID NOT ANSWER (#605).
 *
 * The line above promised it, and the panic that fires when an answer never
 * comes named nobody: the counter was read only by the boot self-tests.  So
 * each call photographs its targets' counts before it sends -- under call_lock,
 * one call in flight and so one photograph -- and a call that times out names
 * every target whose count has not moved.  A targeted call photographs the one
 * or two processors it names; a broadcast, every processor that answers one.
 *
 * The walk visits the set bits only: __builtin_ctzll is one instruction here
 * and needs nothing from libgcc, which this kernel does not link (#415).
 */
static uint64_t call_before[SMP_MAX_CPUS];

static void call_photograph(uint64_t targets)
{
	for (uint64_t m = targets; m != 0; m &= m - 1) {
		unsigned id = (unsigned) __builtin_ctzll(m);

		call_before[id] = answer_count_of(&served, id);
	}
}

/* Name each target that did not answer, and say how many did not. */
static unsigned call_name_silent(uint64_t targets)
{
	unsigned silent = 0;

	for (uint64_t m = targets; m != 0; m &= m - 1) {
		unsigned id = (unsigned) __builtin_ctzll(m);

		if (answer_count_of(&served, id) == call_before[id]) {
			printf("ipi: the processor with APIC id %u never answered "
			       "this cross-call (#605)\n", id);
			silent++;
		}
	}
	return silent;
}

/*
 * Wait for `targets' answers, bounded, and on a timeout name every processor
 * in `who' that did not answer before stopping the machine (#605).  The
 * broadcast and the targeted call each had this loop and this panic, differing
 * only in the message; the message is one now, and says what was asked as well
 * as who stayed silent.
 *
 * Bounded, because the failure worth catching is an answer that never comes,
 * and waiting forever for it turns a report into a hang.  The count is
 * generous: a processor deep in a fault report can take a long time to get
 * round to this.
 */
static void ipi_wait_for_acks(uint64_t who, unsigned targets)
{
	uint64_t spins;
	uint32_t me;

	for (spins = 0; spins < CPU_SPIN_BUDGET; spins++) {
		if (atomic_load64(&call_acks) >= targets)
			return;
		cpu_pause();
	}
	if (atomic_load64(&call_acks) >= targets)
		return;

	/*
	 * 🔴 A CALL THAT NAMES ITS OWN SENDER (#638).
	 *
	 * The sender waits with call_lock held, and the hold masks interrupts,
	 * so a target set that includes it can only time out -- and the line
	 * below then reports a processor that "never answered" when it was
	 * never able to.  Both callers strike their own bit before sending, so
	 * the bit can only be here if the strike was made as another processor:
	 * the thread decided who it was and was moved before it sent.  Asked
	 * here, under the lock, where this thread can no longer move.
	 */
	me = percpu_apic_id();
	if (me < SMP_MAX_CPUS && (who & (1ULL << me)) != 0)
		printf("ipi: this cross-call names the processor sending it "
		       "(APIC id %u): its targets were decided on another "
		       "processor, before the thread was moved here (#638)\n",
		       me);

	panic("ipi: %u of %u processors never answered a cross-call "
	      "(targets 0x%llx, %llu answers arrived)",
	      call_name_silent(who), targets, (unsigned long long) who,
	      (unsigned long long) atomic_load64(&call_acks));
}

static void ipi_call_handler(struct trap_frame *frame)
{
	void (*fn)(void *) = call_fn;
	void *arg = call_arg;

	(void)frame;

	/*
	 * The read of the request happens before the work, and the count of
	 * the answer after it — the compiler is the only thing that could
	 * reorder either.  The processor will not: this side does a load
	 * (the request) followed by a store (the acknowledgement), and a
	 * store is never moved ahead of a load on this architecture.
	 */
	barrier();

	if (fn != 0)
		fn(arg);

	answer_count_mark(&served);

	barrier();
	atomic_inc64(&call_acks);

	/*
	 * Last, deliberately.  The controller holds this vector's priority
	 * busy until it is told the interrupt is finished, which means no
	 * second cross-call can arrive while this one is still running —
	 * exactly the property that makes the single slot above safe.
	 */
	lapic_eoi();
}


static void ipi_ast_handler(struct trap_frame *frame);
static void ipi_halt_handler(struct trap_frame *frame);

void ipi_init(void)
{
	hw_lock_init(&call_lock);
	trap_set_handler(IPI_VECTOR_CALL, ipi_call_handler);
	trap_set_handler(IPI_VECTOR_AST, ipi_ast_handler);
	trap_set_handler(IPI_VECTOR_HALT, ipi_halt_handler);
}

uint64_t ipi_calls_served(uint32_t apic_id)
{
	return answer_count_of(&served, apic_id);
}

#if	WIDEN_638_WINDOW
volatile int shootdown_widen_armed;

/* #638's test only: see <cpu/ipi.h>.  Nothing before the TSC is calibrated. */
void shootdown_widen(void)
{
	uint64_t span, t0;

	if (!shootdown_widen_armed || tsc_hz() == 0)
		return;

	span = tsc_hz() / 1000000 * WIDEN_638_US;
	t0 = rdtsc();
	while (rdtsc() - t0 < span)
		cpu_pause();
}
#endif

void ipi_call_others(void (*fn)(void *), void *arg)
{
	unsigned targets = smp_online_count() - 1;
	uint32_t me;
	uint64_t who;

	if (targets == 0)
		return;

	/*
	 * Checked rather than trusted.  A processor that waits for answers
	 * with interrupts off cannot give one, and if a second processor is
	 * meanwhile waiting for this lock, neither will ever move.  That is a
	 * deadlock which needs two processors to ask at the same moment to
	 * appear at all.
	 */
	if (!interrupts_enabled())
		panic("ipi: a cross-call with interrupts off would deadlock");

	hw_lock_lock(&call_lock);

	/*
	 * Every processor that answers a broadcast, less this one -- asked
	 * under the lock, whose hold keeps this thread on this processor
	 * (#638).  Asked before it, "this one" could be the processor the
	 * thread had just left.  The broadcast still reached the right ones,
	 * since the shorthand leaves out whoever sends it, but the photograph
	 * below, and the names a timeout prints, were taken for the wrong
	 * processor.
	 */
	me = percpu_apic_id();
	who = smp_answering_set();
	if (me < SMP_MAX_CPUS)
		who &= ~(1ULL << me);

	call_photograph(who);
	call_fn = fn;
	call_arg = arg;
	atomic_store64(&call_acks, 0);

	/*
	 * The request is in place before the message that points at it, and
	 * that ordering is free here: both are stores, and this architecture
	 * does not move a store ahead of another store.  The barrier is
	 * against the compiler, which has no such scruples — and against a
	 * future where this is read by something whose memory model is
	 * weaker, which is the reason it is spelt smp_wmb() and not a
	 * comment.
	 */
	smp_wmb();

	lapic_broadcast_ipi(IPI_VECTOR_CALL);

	ipi_wait_for_acks(who, targets);

	hw_lock_unlock(&call_lock);
}

#ifndef	ABLATE_638_STRIKE_UNPINNED
#define	ABLATE_638_STRIKE_UNPINNED	0
#endif

/*
 * Send to every processor in `mask', which no longer names this one, and wait
 * for all of them.
 */
static void ipi_call_targets(uint64_t mask, void (*fn)(void *), void *arg)
{
	unsigned targets;
	unsigned id;

	if (mask == 0)
		return;

	/* Same reason as ipi_call_others(): see the comment there. */
	if (!interrupts_enabled())
		panic("ipi: a cross-call with interrupts off would deadlock");

	hw_lock_lock(&call_lock);

	call_photograph(mask);
	call_fn = fn;
	call_arg = arg;
	atomic_store64(&call_acks, 0);

	smp_wmb();

	/*
	 * One message per target, and the loop is the difference from the
	 * broadcast: sending costs a store to the interrupt command register
	 * per processor, so a mask with sixty-three bits set is dearer to send
	 * than one broadcast.  It is still the right shape, because the mask
	 * that matters in practice has one bit or two — and because a
	 * broadcast to sixty-four processors to reach two of them makes the
	 * other sixty-two take an interrupt for nothing.
	 */
	/*
	 * ⚠️ Counted here rather than with __builtin_popcountll(), and the
	 * linker is what said so: gcc turns that builtin into a call to
	 * libgcc's __popcountdi2, and this kernel links no libgcc (#415).  The
	 * loop has to visit every set bit anyway, so counting in it costs an
	 * increment and removes the dependency rather than working around it.
	 */
	targets = 0;
	for (id = 0; id < 64; id++)
		if (mask & (1ULL << id)) {
			lapic_send_ipi((uint32_t) id, IPI_VECTOR_CALL);
			targets++;
		}

	ipi_wait_for_acks(mask, targets);

	hw_lock_unlock(&call_lock);
}

void ipi_call_mask(uint64_t mask, void (*fn)(void *), void *arg)
{
	/*
	 * Never ourselves.  A processor inside this function is not going to
	 * take the interrupt it just sent, so a bit for the caller would be a
	 * target that can never acknowledge — the wait below would spin out
	 * its budget and panic, on a mask that was perfectly correct.
	 *
	 * 🔴 AND "OURSELVES" IS ASKED WHERE IT CANNOT CHANGE (#638).  Struck at
	 * level zero, the bit could be the processor the thread was about to
	 * leave: moved before it took the lock, the thread sent the call to
	 * the processor it had arrived at -- itself -- and waited, interrupts
	 * masked by the hold, for an answer only it could give.  So preemption
	 * goes off before the bit is struck and comes back after the call.
	 * Not call_lock instead: a mask that names only this processor is the
	 * common case, and it would then take the machine's one cross-call
	 * lock to send nothing.  UROS_ABLATE_638_STRIKE_UNPINNED strikes the
	 * bit with preemption on again.
	 */
	if (ABLATE_638_STRIKE_UNPINNED) {
		mask &= ~(1ULL << (percpu_apic_id() & 63));
#if	WIDEN_638_WINDOW
		if (mask != 0)
			shootdown_widen();	/* between deciding and sending */
#endif
		ipi_call_targets(mask, fn, arg);
		return;
	}

#if	WIDEN_638_WINDOW
	shootdown_widen();		/* before deciding: a move is harmless */
#endif
	disable_preemption();
	mask &= ~(1ULL << (percpu_apic_id() & 63));
	ipi_call_targets(mask, fn, arg);
	enable_preemption();
}

/*
 * 🔴 THE AST INTERRUPT RUNS ast_check() (#603).
 *
 * This said the interrupt did nothing and that was the whole of it (#453):
 * "taking an interrupt and returning from it IS that point -- the check
 * lives on the return path".  The return path, trap_take_ast(), looks at
 * need_ast[cpu], and nothing had set it.  ast_check() is what does: it
 * copies the running activation's ASTs into that word -- act_set_apc()
 * reaches only its caller's own processor -- and raises AST_BLOCK when a
 * switch is due.  i386's handler calls it ("MP_AST: cross-CPU reschedule
 * request"); on this machine only the tick did.
 *
 * So cause_ast_check() asked for nothing.  A thread_suspend() of a thread
 * running here waited for this processor's next tick -- act_test's arm
 * seven, at four processors, touched up to 4324 pages inside the call --
 * and so did a preemption asked from another processor.
 */
static void ipi_ast_handler(struct trap_frame *frame)
{
	(void) frame;
	if (!ABLATE_603_IPI_EMPTY)
		ast_check();
	lapic_eoi();
}

void ipi_ast_check(uint32_t apic_id)
{
	lapic_send_ipi(apic_id, IPI_VECTOR_AST);
}

/*
 * The stop interrupt, which does not return (#461).
 *
 * No acknowledgement, because there is nobody left waiting for one, and no
 * end-of-interrupt either: this processor is not going to take another
 * interrupt.  halt_cpu() prints where it was, on the way past, and stops.
 */
static void ipi_halt_handler(struct trap_frame *frame)
{
	(void) frame;
	halt_cpu();
	/*NOTREACHED*/
}

void ipi_halt_others(void)
{
	if (smp_online_count() <= 1)
		return;

	lapic_broadcast_ipi(IPI_VECTOR_HALT);
}
