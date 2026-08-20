/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The storage and the report for the copy-on-write phase breakdown (#482).
 * The hooks themselves are inline in <kern/fault_profile.h>, because a phase
 * of a few hundred cycles must not be measured through a call.
 */

#include <kern/fault_profile.h>
#include <kern/misc_protos.h>		/* printf */

#if	FAULT_PROFILE

struct fault_profile_cpu	fault_profile[NCPUS];

/*
 * What each slice is.
 *
 * ⚠️ The two marked `*' are the calls that end in a shootdown, and they are
 * the reason the issue exists.  Reading the table without knowing which two
 * they are is reading it without knowing what it was taken to answer.
 */
static const char *const	fp_name[FP_PHASES] = {
	"entry     ",	/* FP_ENTRY    */
	"map lock  ",	/* FP_MAPLOCK  */
	"lookup    ",	/* FP_LOOKUP   */
	"chain     ",	/* FP_CHAIN    */
	"obj lock  ",	/* FP_OBJLOCK  */
	"alloc     ",	/* FP_ALLOC    */
	"copy      ",	/* FP_COPY     */
	"protect  *",	/* FP_PROTECT  */
	"collapse  ",	/* FP_COLLAPSE */
	"enter    *",	/* FP_ENTER    */
	"queues    ",	/* FP_QUEUES   */
	"return    "	/* FP_RETURN   */
};

/*
 * What one pair of timestamps costs on the machine that is running, measured
 * there rather than assumed.
 *
 * 🔑 Thirteen marks bracket one fault, so this multiplied by thirteen is how
 * much wider the fault got by being watched.  Printing it is what separates a
 * measurement from a claim: the reader subtracts a number they were given,
 * instead of trusting that the instrument is small enough not to matter.
 *
 * ⚠️ The tsc pair only.  A mark is that plus a subtraction, two stores and an
 * indexed load, which are a handful of cycles more and are not separable from
 * the phase they sit in without a second instrument.  So this is a floor on
 * the instrument's cost and is named as one.
 */
static uint32_t	fp_pair_cost;

static void
fp_sort(uint32_t *v, int n)
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
fp_measure_self(void)
{
	uint32_t	d[9];
	int		i;

	for (i = 0; i < 9; i++) {
		uint64_t	a = fault_profile_tsc();
		uint64_t	b = fault_profile_tsc();

		d[i] = (uint32_t) (b - a);
	}
	fp_sort(d, 9);
	fp_pair_cost = d[4];		/* median of nine, never the mean */
}

/*
 * Percent of the whole, in 32 bits.
 *
 * 🔴 Deliberately not (uint64_t) arithmetic.  This file compiles for both
 * targets, and a 64-bit divide on the 32-bit one is a call to libgcc's
 * __udivdi3, which this kernel does not link and will not link.  A slice is a
 * page fault's worth of cycles, so it fits; the guard is for the sample that
 * does not, which the caller has already refused to record.
 */
static uint32_t
fp_percent(uint32_t part, uint32_t whole)
{
	if (whole == 0 || part > 42000000u)
		return 0;
	return (part * 100u) / whole;
}

void
fault_profile_dump(void)
{
	struct fault_profile_cpu	*fp = &fault_profile[cpu_number()];
	uint32_t			col[FP_SAMPLES];
	uint32_t			whole;
	int				n = (int) fp->nsamples;
	int				median = 0;
	int				i, p;

	if (n == 0) {
		printf("fault_profile[%d]: armed, no copy-on-write fault seen\n",
		       cpu_number());
		return;
	}

	if (fp_pair_cost == 0)
		fp_measure_self();

	/*
	 * The representative fault is the one whose TOTAL is the median, and
	 * its own slices are what gets printed.
	 *
	 * 🔴 Not the per-phase medians, which is the tempting table and the
	 * wrong one: twelve independent medians come from twelve different
	 * faults and sum to nothing that ever happened.  One real fault, chosen
	 * for being the middle one, has slices that add up because they were
	 * cut out of the same interval.  [technique: measurement discipline]
	 */
	for (i = 0; i < n; i++)
		col[i] = fp->total[i];
	fp_sort(col, n);
	whole = col[(n - 1) / 2];
	for (i = 0; i < n; i++) {
		if (fp->total[i] == whole) {
			median = i;
			break;
		}
	}

	printf("fault_profile[%d] #%u: %d faults, %u interrupted, %u via "
	       "vm_fault_page; 13 marks x %u cyc = %u\n",
	       cpu_number(), fp->ndumps + 1, n, fp->ndropped, fp->nslow,
	       fp_pair_cost, 13u * fp_pair_cost);
	printf("fault_profile[%d]   median fault %u cyc (spread %u..%u)\n",
	       cpu_number(), whole, col[0], col[n - 1]);

	for (p = 0; p < FP_PHASES; p++) {
		for (i = 0; i < n; i++)
			col[i] = fp->sample[i][p];
		fp_sort(col, n);

		printf("fault_profile[%d]   %s %6u  %2u%%  [%u..%u]\n",
		       cpu_number(), fp_name[p], fp->sample[median][p],
		       fp_percent(fp->sample[median][p], whole),
		       col[0], col[n - 1]);
	}

	fp->ndumps++;
	if (fp->ndumps >= FP_MAX_DUMPS)
		printf("fault_profile[%d]: %u breakdowns printed, no more from "
		       "this processor\n", cpu_number(), fp->ndumps);
}

/*
 * A copy-on-write fault that took the slow path.
 *
 * 🔑 Counted rather than profiled, and the count is the answer to a question
 * that was never asked out loud: the breakdown describes the FAST path, and a
 * table that describes the fast path while most faults take the slow one
 * describes nothing.  Zero here is what makes the rest of the report about the
 * copy-on-write fault rather than about a corner of it.
 * [feedback: proofs by absence need a presence]
 */
void
fault_profile_slow(void)
{
	fault_profile[cpu_number()].nslow++;
}

void
fault_profile_commit(const void *token)
{
	struct fault_profile_cpu	*fp = &fault_profile[cpu_number()];
	uint32_t			sum = 0;
	uint32_t			whole;
	int				i;

	if (!fp->cow)
		return;			/* not a fast copy-on-write fault */
	fp->cow = 0;

	if (fp->ndumps >= FP_MAX_DUMPS)
		return;

	/*
	 * Somebody else's cursor.  A trap arrived inside this one, armed a
	 * sample of its own, and every mark since then has been charged
	 * against its clock.
	 */
	if (fp->owner != token) {
		fp->ndropped++;
		return;
	}

	for (i = 0; i < FP_PHASES; i++)
		sum += fp->slice[i];
	whole = (uint32_t) (fp->cursor - fp->first);

	/*
	 * The one way the slices can fail to be the interval: a phase longer
	 * than a uint32 of cycles, about a second, which means this fault was
	 * not the thing the breakdown is about.  Dropped and said, rather than
	 * averaged in.
	 */
	if (sum != whole) {
		fp->ndropped++;
		return;
	}

	for (i = 0; i < FP_PHASES; i++)
		fp->sample[fp->nsamples][i] = fp->slice[i];
	fp->total[fp->nsamples] = whole;
	fp->nsamples++;

	if (fp->nsamples >= FP_SAMPLES) {
		/*
		 * ⚠️ Printed here, after the last slice is closed and the
		 * sample is stored, and never between two marks.  The console
		 * takes a lock and writes a serial port; a breakdown that
		 * included the cost of reporting the previous one would be
		 * measuring itself.
		 */
		fault_profile_dump();
		fp->nsamples = 0;
		fp->ndropped = 0;
		fp->nslow = 0;
	}
}

#else	/* !FAULT_PROFILE */

/*
 * 🔑 The instrument that was not built says so.
 *
 * A kernel compiled without this must not be able to look like one compiled
 * with it that happened to see nothing -- that is the same shape of mistake as
 * a failed build that looks like a successful run, and it has cost this port
 * whole afternoons.  [feedback: the instrument lies]
 */
void
fault_profile_dump(void)
{
	printf("fault_profile: not built — configure with "
	       "-DUROS_FAULT_PROFILE=ON\n");
}

#endif	/* FAULT_PROFILE */
