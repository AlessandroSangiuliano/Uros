/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Making another processor do something (#438).
 */

#include <stdint.h>

#include <cpu/ipi.h>
#include <cpu/lapic.h>
#include <cpu/regs.h>
#include <cpu/smp.h>
#include <sync/atomic.h>
#include <sync/barrier.h>
#include <sync/lock.h>
#include <trap/trap.h>

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
static volatile uint64_t served[SMP_MAX_CPUS];

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

	served[cpu_apic_id()]++;

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

void ipi_init(void)
{
	hw_lock_init(&call_lock);
	trap_set_handler(IPI_VECTOR_CALL, ipi_call_handler);
	trap_set_handler(IPI_VECTOR_AST, ipi_ast_handler);
}

uint64_t ipi_calls_served(uint32_t apic_id)
{
	if (apic_id >= SMP_MAX_CPUS)
		return 0;

	return atomic_load64(&served[apic_id]);
}

void ipi_call_others(void (*fn)(void *), void *arg)
{
	unsigned targets = smp_online_count() - 1;
	uint64_t spins;

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

	/*
	 * Bounded, because the failure worth catching is an answer that never
	 * comes, and waiting forever for it turns a report into a hang.  The
	 * count is generous: a processor deep in a fault report can take a
	 * long time to get round to this.
	 */
	for (spins = 0; spins < 400000000ULL; spins++) {
		if (atomic_load64(&call_acks) >= targets)
			break;
		cpu_pause();
	}

	if (atomic_load64(&call_acks) < targets)
		panic("ipi: a processor never answered a cross-call");

	hw_lock_unlock(&call_lock);
}

/*
 * The AST interrupt does nothing, and that is the whole of it (#453).
 *
 * The scheduler wanted a processor to reach a point where it checks its
 * pending asynchronous work.  Taking an interrupt and returning from it IS
 * that point -- the check lives on the return path, in machine-independent
 * code, and runs whatever the interrupt was for.  A handler that did
 * something here would be doing it twice.
 */
static void ipi_ast_handler(struct trap_frame *frame)
{
	(void) frame;
	lapic_eoi();
}

void ipi_ast_check(uint32_t apic_id)
{
	lapic_send_ipi(apic_id, IPI_VECTOR_AST);
}
