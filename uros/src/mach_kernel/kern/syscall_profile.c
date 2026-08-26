/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Printing the per-phase breakdown of a Mach trap (#411, for #392).
 *
 * The contract, the phases and the reason the sample lives on the thread are
 * all in <kern/syscall_profile.h>.  This file is the report.
 */

#include <kern/syscall_profile.h>
#include <kern/misc_protos.h>		/* printf */
#if	SYSCALL_PROFILE
#include <kern/thread.h>
#endif

/*
 * Which trap is being profiled: urmach_msg, slot 33 of the table in
 * kern/syscall_sw.c, because that is the path #392 exists to weigh.
 *
 * Defined whether or not the profile is compiled in, so that anything which
 * wants to point it elsewhere links either way.
 */
int	syscall_profile_trap = 33;

#if	SYSCALL_PROFILE

/*
 * What a pair of timestamps costs on this machine, measured here rather than
 * assumed, so the reader can subtract the instrument from its own subject
 * instead of trusting that it is small.
 *
 * ⚠️ The tsc pair only.  A mark is that plus a subtraction, two stores and an
 * indexed load; those are not separable from the phase they sit in without a
 * second instrument.  So this is a FLOOR on the instrument's cost, and it is
 * named as one.  It matters more here than in fault_profile: a Mach trap is a
 * few hundred cycles, so four marks are a visible fraction of the subject
 * rather than a rounding error.
 */
static uint32_t	sp_pair_cost;

static const char *const sp_name[SP_PHASES] = {
	"entry ",	/* SP_ENTRY */
	"body  ",	/* SP_BODY  */
	"wait  ",	/* SP_WAIT  */
};

static void
sp_sort(uint32_t *v, int n)
{
	int	i, j;

	for (i = 1; i < n; i++) {
		uint32_t	x = v[i];

		for (j = i; j > 0 && v[j - 1] > x; j--)
			v[j] = v[j - 1];
		v[j] = x;
	}
}

static void
sp_measure_self(void)
{
	uint32_t	d[9];
	int		i;

	for (i = 0; i < 9; i++) {
		uint64_t	a = syscall_profile_tsc();
		uint64_t	b = syscall_profile_tsc();

		d[i] = (uint32_t) (b - a);
	}
	sp_sort(d, 9);
	sp_pair_cost = d[4];		/* median of nine, never the mean */
}

/*
 * Percent of the whole, in 32 bits.
 *
 * 🔴 Returns -1 rather than 0 when it cannot answer, and the caller prints
 * "??" for it.  fault_profile returns 0 here, which is defensible there
 * because a fault that overflows has already been refused -- and it is not
 * defensible here: the first real run of this profile produced slices of
 * three hundred million cycles, every percentage printed as `0%%', and a table
 * of zeroes reads as "these phases are free" rather than as "this arithmetic
 * gave up".  An instrument that cannot compute a number must say so.
 */
static int
sp_percent(uint32_t part, uint32_t whole)
{
	if (whole == 0)
		return -1;

	/*
	 * ⚠️ Multiply-then-divide only while it fits.  fault_profile refuses
	 * anything over forty-two million, which is right THERE -- a slice that
	 * big is a broken sample -- and wrong here: a trap that waited three
	 * hundred million cycles for a message is not broken, it is a server
	 * doing its job, and refusing to compute its share printed "??" for the
	 * one number in the table that was never in doubt.
	 *
	 * Above the limit, divide first.  It costs the last digit or two of
	 * precision on a figure that is being printed as a whole percent.
	 */
	if (part <= 42000000u)
		return (int) ((part * 100u) / whole);
	if (whole < 100u)
		return -1;
	return (int) (part / (whole / 100u));
}

static void
sp_print_pct(int pct)
{
	if (pct < 0)
		printf("  ??");
	else
		printf(" %2d%%", pct);
}

void
syscall_profile_dump(struct syscall_profile_thread *p)
{
	uint32_t	col[SP_SAMPLES];
	uint32_t	whole;
	int		n = SP_SAMPLES;
	int		median = 0;
	int		i, ph;
	uint64_t	ret_cyc, ret_n;
	uint32_t	work;

	if (sp_pair_cost == 0)
		sp_measure_self();

	/*
	 * The representative trap is the one whose TOTAL is the median, and its
	 * own slices are what gets printed.
	 *
	 * 🔴 Not the per-phase medians, which is the tempting table and the
	 * wrong one: a median per phase comes from a different trap in every
	 * column, and the columns sum to something that never happened.  One
	 * real trap, chosen for being the middle one, has slices that add up
	 * because they were cut out of one interval.
	 */
	for (i = 0; i < n; i++)
		col[i] = p->total[i];
	sp_sort(col, n);
	whole = col[(n - 1) / 2];
	for (i = 0; i < n; i++) {
		if (p->total[i] == whole) {
			median = i;
			break;
		}
	}

	printf("syscall_profile trap %d #%u: %d of %u traps this thread made "
	       "(%u of them slept, %u dropped); "
	       "%u marks x %u cyc = %u of instrument\n",
	       syscall_profile_trap, p->ndumps, n, p->nseen, p->nblocked,
	       p->ndropped,
	       (unsigned int) SP_MARKS, sp_pair_cost,
	       (unsigned int) SP_MARKS * sp_pair_cost);
	printf("syscall_profile   median trap %u cyc (spread %u..%u)\n",
	       whole, col[0], col[n - 1]);

	/*
	 * ⚠️ The on-processor phases are a share of WORK, and the wait is a
	 * share of the wall clock, and the header says which is which.
	 *
	 * 🔥 They were all taken against the wall clock, and every one of them
	 * printed `0%%' -- arithmetically correct and useless, because a trap
	 * that waited three hundred million cycles for a message makes every
	 * real phase a rounding error against its own total.  A table of zeroes
	 * reads as "these cost nothing"; what it meant was "the denominator is
	 * mostly sleep".
	 */
	work = 0;
	for (ph = 0; ph < SP_PHASES; ph++)
		if (ph != SP_WAIT)
			work += p->sample[median][ph];

	for (ph = 0; ph < SP_PHASES; ph++) {
		for (i = 0; i < n; i++)
			col[i] = p->sample[i][ph];
		sp_sort(col, n);

		printf("syscall_profile   %s %10u ", sp_name[ph],
		       p->sample[median][ph]);
		sp_print_pct(sp_percent(p->sample[median][ph],
				        ph == SP_WAIT ? whole : work));
		printf(" %s  [%u..%u]\n",
		       ph == SP_WAIT ? "of wall " : "of work",
		       col[0], col[n - 1]);
	}

	/*
	 * The return path, from the processor's own accounting.
	 *
	 * ⚠️ A MEAN, and labelled one, because that is what a running sum can
	 * give and this file will not print a mean that looks like a median.
	 * It is defensible here for the reason the column does not exist: the
	 * return path runs the same instructions every time, so its
	 * distribution has nothing to say that its centre does not.
	 */
	syscall_profile_return_cycles(&ret_cyc, &ret_n);
	if (ret_n != 0) {
		uint32_t	ret_mean = (uint32_t) (ret_cyc / ret_n);

		printf("syscall_profile   return %6u  (mean of %u, this "
		       "processor — not a column, see the header)\n",
		       ret_mean, (unsigned int) ret_n);

		/*
		 * 🔑 What #392 comes here for, said out loud rather than left
		 * to be worked out from the table: entry and return are the
		 * floor register-IPC (#391) cannot touch, and the body is the
		 * CEILING on what it could reclaim.  Not the answer -- that
		 * needs the body split, which is #392's own work -- but the
		 * bound, and a bound is what says whether the split is worth
		 * doing at all.
		 */
		/*
		 * ⚠️ The share is of WORK, not of the trap.  A trap that waited
		 * for a message spent most of its life off the processor, and a
		 * percentage taken against that total would say the copies are
		 * negligible -- which is true of the wall clock and false of
		 * everything #392 is deciding.
		 */
		work += ret_mean;
		printf("syscall_profile   floor entry+return %u cyc, ceiling "
		       "body %u cyc, body share",
		       p->sample[median][SP_ENTRY] + ret_mean,
		       p->sample[median][SP_BODY]);
		sp_print_pct(sp_percent(p->sample[median][SP_BODY], work));
		printf(" of the %u cyc this trap spent ON a processor\n", work);
	}

	if (p->ndumps >= SP_MAX_DUMPS)
		printf("syscall_profile: %u breakdowns printed, no more from "
		       "this thread\n", p->ndumps);
}

/*
 * The hooks, and the only two calls #392 has to add anywhere else.
 *
 * ⚠️ current_thread() and not the activation.  A sample follows the shuttle
 * through the block and the hand-off, which is precisely the thing the
 * activation does not do.
 */
void
syscall_profile_enter(int trap_number)
{
	thread_t	t = current_thread();

	if (trap_number != syscall_profile_trap || t == THREAD_NULL)
		return;

	syscall_profile_begin(&t->syscall_profile, syscall_profile_entry_tsc());
	/*
	 * The entry phase closes HERE, at the first instruction that could
	 * close it: everything between the stub's clock reading and this line
	 * is what a Mach trap costs before it has begun doing what it was
	 * asked.  That is the number #392 weighs the copies against.
	 */
	syscall_profile_mark(&t->syscall_profile, SP_ENTRY);
}

void
syscall_profile_blocked(thread_t t)
{
	if (t == THREAD_NULL)
		return;

	syscall_profile_waiting(&t->syscall_profile);
}

void
syscall_profile_back(void)
{
	thread_t	t = current_thread();

	if (t == THREAD_NULL)
		return;

	syscall_profile_resumed(&t->syscall_profile);
}

void
syscall_profile_leave(void)
{
	thread_t	t = current_thread();

	if (t == THREAD_NULL)
		return;

	syscall_profile_commit(&t->syscall_profile);
}

#endif	/* SYSCALL_PROFILE */
