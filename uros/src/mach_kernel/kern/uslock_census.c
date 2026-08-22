/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The storage, the interrupt-time sample and the report for #486.  The
 * acquire and release hooks are inline in <kern/uslock_census.h>, because
 * they sit on every simple lock in the kernel.
 */

#include <kern/uslock_census.h>
#include <kern/misc_protos.h>		/* printf */

#if	USLOCK_CENSUS

struct uslock_census_cpu	uslock_census[NCPUS];

volatile int			uslock_census_ready = 0;

void
uslock_census_arm(void)
{
	uslock_census_ready = 1;
}

/*
 * Called at the very top of ipi_mp_handler(), before it disables anything, so
 * what it reads is the state of the code the interrupt cut into.
 *
 * Runs at IF=0 on the interrupt stack: it touches this processor's slot and
 * nothing else, takes no lock, and does not print.  Printing from here is what
 * #319 already established does not work -- it nests inside the console the
 * interrupted code may itself be holding.  The report runs from sched_thread.
 */
void
uslock_census_sample(void)
{
	struct uslock_census_cpu	*c = uslock_census_slot();
	unsigned long			pc;
	int				i;

	if (c == (struct uslock_census_cpu *) 0)
		return;

	c->entries++;

	/*
	 * The narrow counts first, because they are the ones the deadlock is
	 * about and they do not depend on the depth being trustworthy.
	 */
	if (c->in_tick != 0)
		c->tick_hits++;
	if (c->in_qadj != 0)
		c->qadj_hits++;

	if (c->depth <= 0)
		return;

	c->unsafe++;
	if ((unsigned int) c->depth > c->max_depth)
		c->max_depth = (unsigned int) c->depth;

	/*
	 * The most recently acquired lock still held.  Blaming the innermost
	 * one is the right choice: it is the one whose critical section we are
	 * standing in the middle of.
	 */
	i = c->depth - 1;
	if (i >= USLOCK_CENSUS_DEPTH)
		i = USLOCK_CENSUS_DEPTH - 1;
	pc = c->pc[i];

	for (i = 0; i < USLOCK_CENSUS_SITES; i++) {
		if (c->site_count[i] == 0) {
			c->site_pc[i] = pc;
			c->site_count[i] = 1;
			return;
		}
		if (c->site_pc[i] == pc) {
			c->site_count[i]++;
			return;
		}
	}
	c->site_lost++;
}

/*
 * Process context, one processor, reading every processor's counters while
 * they move.  That is a race and it does not matter: the counters only grow,
 * so a torn read understates and never invents.  What would matter is a
 * report that cannot say "nothing happened" -- so this prints for a processor
 * that took interrupts and held no lock too, because an absence is only worth
 * anything when the instrument could have shown a presence.
 */
void
uslock_census_report(void)
{
	int		cpu;
	int		i;

	if (!uslock_census_ready) {
		printf("uslock_census: not armed on this machine\n");
		return;
	}

	for (cpu = 0; cpu < NCPUS; cpu++) {
		struct uslock_census_cpu	*c = &uslock_census[cpu];

		if (c->entries == 0)
			continue;

		printf("uslock_census[%d]: %u mp-ipi entries, %u with a simple "
		       "lock held, deepest %u\n",
		       cpu, c->entries, c->unsafe, c->max_depth);
		printf("uslock_census[%d]   %u inside the quantum tick, "
		       "%u holding quantum_adj_lock\n",
		       cpu, c->tick_hits, c->qadj_hits);
		if (c->qadj_miss != 0 || c->qadj_averted != 0)
			printf("uslock_census[%d]   quantum_adj try failed "
			       "%u times, %u of them nested on this cpu "
			       "(self-deadlock averted)\n",
			       cpu, c->qadj_miss, c->qadj_averted);

		for (i = 0; i < USLOCK_CENSUS_SITES; i++) {
			if (c->site_count[i] == 0)
				break;
			printf("uslock_census[%d]   %6u  held from 0x%lx\n",
			       cpu, c->site_count[i], c->site_pc[i]);
		}
		if (c->site_lost != 0)
			printf("uslock_census[%d]   %u more from sites past "
			       "the table\n", cpu, c->site_lost);
		if (c->overflow != 0 || c->underflow != 0)
			printf("uslock_census[%d]   depth overflow %u, "
			       "underflow %u\n",
			       cpu, c->overflow, c->underflow);
		c->reports++;
	}
}

#endif	/* USLOCK_CENSUS */
