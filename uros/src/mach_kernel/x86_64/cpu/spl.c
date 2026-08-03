/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Interrupt priority, per processor and in software (#409, with #322).
 */

#include <stdint.h>

#include <cpu/lapic.h>
#include <cpu/percpu.h>
#include <cpu/regs.h>
#include <cpu/spl.h>
#include <trap/trap.h>

spl_t splget(void)
{
	return percpu()->ipl;
}

int spl_defer(unsigned vector)
{
	struct percpu *p = percpu();

	if (spl_of_vector(vector) > p->ipl)
		return 0;

	/*
	 * Recorded before the acknowledgement, because between the two this
	 * processor could take another interrupt of a higher class and lower
	 * the level on its way out — finding a note that is not yet there.
	 */
	p->pending[vector >> 6] |= 1ULL << (vector & 63);
	p->deferred++;
	return 1;
}

/*
 * Run everything recorded whose class is now allowed, highest first.
 *
 * Highest first because that is the order they would have arrived in if
 * nothing had been deferred, and a caller that lowers the level is entitled
 * to the same view it would have had.
 *
 * ⚠️ Interrupts are disabled across each handler and the level is left where
 * the caller put it: a replayed handler that raised and lowered again would
 * otherwise re-enter this loop and run the rest of the queue underneath
 * itself.
 */
static void spl_replay(struct percpu *p)
{
	for (;;) {
		unsigned found = 0;
		int any = 0;

		for (unsigned v = 255; v != 0; v--) {
			if (spl_of_vector(v) <= p->ipl)
				break;		/* everything below is masked */
			if (p->pending[v >> 6] & (1ULL << (v & 63))) {
				found = v;
				any = 1;
				break;
			}
		}

		if (!any)
			return;

		p->pending[found >> 6] &= ~(1ULL << (found & 63));
		p->replayed++;
		trap_replay_vector(found);
	}
}

spl_t splx(spl_t level)
{
	struct percpu *p = percpu();
	spl_t old = p->ipl;
	int enabled = interrupts_enabled();

	if (level == old)
		return old;

	/*
	 * The level and the queue are this processor's, but an interrupt
	 * arriving between reading one and writing the other would see a state
	 * that is neither. Interrupts off across the transition costs two
	 * instructions and removes the case entirely.
	 */
	interrupts_disable();
	p->ipl = level;

	if (level < old)
		spl_replay(p);

	if (enabled)
		interrupts_enable();

	return old;
}

uint64_t spl_deferred_count(void)
{
	return percpu()->deferred;
}

uint64_t spl_replayed_count(void)
{
	return percpu()->replayed;
}

/*
 * Interrupts off at the processor, and back on (#453).
 *
 * Distinct from the levels above, which are a software priority this
 * processor keeps in its own block: these two touch the interrupt flag, for
 * the places that must take nothing at all -- reloading the IDT, the last
 * steps of a halt.
 *
 * sploff() answers the level rather than the flag, matching its neighbours
 * so a caller can pair it with splx() for the level part; the flag is
 * splon()'s to restore.  Crossing the two -- sploff() then splx() -- leaves
 * interrupts disabled with the level lowered, which looks like a hang and is
 * one.
 */
spl_t sploff(void)
{
	spl_t	level = splx(SPLHI);

	__asm__ volatile("cli" : : : "memory");
	return level;
}

void splon(spl_t level)
{
	__asm__ volatile("sti" : : : "memory");
	(void) splx(level);
}
