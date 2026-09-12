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

/*
 * 🔴 THE PRESENCE CONTROL FOR AN ABSENCE THIS INSTRUMENT CAN PRINT.
 *
 * The first run of the split printed twelve columns of zeroes and "0 of 16 took
 * the hand-off".  That reading is compatible with two entirely different
 * worlds: the hot path is not being taken, or the marks are not being reached.
 * A table of zeroes cannot tell them apart, and a table of zeroes is exactly
 * the shape a broken instrument has.
 *
 * These two counters are the kernel's own, unconditional, and older than this
 * file: `candidates' counts every trap that entered the hot path at all,
 * `hand-offs' counts every one that reached the commit point.  Printed beside
 * the zeroes they turn "nothing happened" into a statement about which thing
 * did not happen.  [feedback: an absence must be confirmed by a presence]
 */
extern unsigned int	c_mmot_combined_S_R;
extern unsigned int	c_mach_msg_trap_switch_fast;

/*
 * 🔥 AND THE SAME QUESTION ASKED OF THE INSTRUMENT ITSELF.
 *
 * With the counters above, twelve columns of zeroes alongside eighty thousand
 * hand-offs says the path is walked and the marks are not firing -- which is
 * two possibilities again, one line further down: the mark SITES are not being
 * reached, or they are reached with no sample open on that thread.
 *
 * These count both, per phase, kernel-wide.  A site with hits and no slices is
 * a sample that was not open; a site with no hits at all is a route through
 * mach_msg that does not pass where the mark was put.  Three lines of counters
 * to keep an absence from being explained instead of divided.
 */
static unsigned int	sp_site[SP_PHASES];
static unsigned int	sp_site_shut[SP_PHASES];

/*
 * ⚠️ Named for the WORK and not for the function, and the two copies are named
 * as copies.  A reader deciding #391 has to be able to find them without
 * knowing that ipc_kmsg_get() is where a message is copied in.
 */
static const char *const sp_name[SP_PHASES] = {
	"entry   ",	/* SP_ENTRY   */
	"get buf ",	/* SP_GET     */
	"COPYIN  ",	/* SP_COPYIN  */
	"resolve ",	/* SP_RESOLVE */
	"queue   ",	/* SP_QUEUE   */
	"pick rcv",	/* SP_PICK    */
	"claim   ",	/* SP_CLAIM   */
	"park snd",	/* SP_PARK    */
	"deliver ",	/* SP_DELIVER */
	"wait    ",	/* SP_WAIT    */
	"SWITCH  ",	/* SP_SWITCH  */
	"splx    ",	/* SP_SPL     */
	"resume  ",	/* SP_RESUME  */
	"copyout ",	/* SP_COPYOUT */
	"PUT     ",	/* SP_PUT     */
	"residue ",	/* SP_BODY    */
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
	int		n = 0;
	int		nhot = 0;
	int		median = -1;
	int		i, ph;
	uint64_t	ret_cyc, ret_n;
	uint32_t	work;
	uint32_t	copies;

	if (sp_pair_cost == 0)
		sp_measure_self();

	for (i = 0; i < SP_SAMPLES; i++)
		if (p->hot[i])
			nhot++;

	/*
	 * The representative trap is the one whose TOTAL is the median, and its
	 * own slices are what gets printed.
	 *
	 * 🔴 Not the per-phase medians, which is the tempting table and the
	 * wrong one: a median per phase comes from a different trap in every
	 * column, and the columns sum to something that never happened.  One
	 * real trap, chosen for being the middle one, has slices that add up
	 * because they were cut out of one interval.
	 *
	 * 🔥 #392: and chosen from among the traps that took the HAND-OFF, when
	 * there are any.  A window holding both hand-offs and traps that fell
	 * off the hot path at the first test has a median describing neither --
	 * the two shapes differ by eight of the twelve columns.  When the window
	 * holds none, everything is printed and the header says the breakdown is
	 * of something else.
	 */
	for (i = 0; i < SP_SAMPLES; i++)
		if (nhot == 0 || p->hot[i])
			col[n++] = p->total[i];
	sp_sort(col, n);
	whole = col[(n - 1) / 2];
	for (i = 0; i < SP_SAMPLES; i++) {
		if (p->total[i] == whole && (nhot == 0 || p->hot[i])) {
			median = i;
			break;
		}
	}
	if (median < 0)
		return;

	/*
	 * ⚠️ The window and the trap numbers, and not just a dump counter.
	 * The windows are spaced geometrically, so "#3" is traps 49-64 in one
	 * thread and 1,985-2,000 in the next -- and which suite of the
	 * benchmark those fell in is the difference between a startup number
	 * and a steady-state one.
	 */
	printf("syscall_profile trap %d thread %p dump %u, window %u = traps "
	       "%u..%u of %u this thread made (%u slept, %u dropped), %d of %d "
	       "took the hand-off; %u marks x %u cyc = %u of instrument\n",
	       syscall_profile_trap, p->self, p->ndumps, p->nwindows,
	       p->nseen - SP_SAMPLES + 1, p->nseen, p->nseen,
	       p->nblocked, p->ndropped, nhot, (int) SP_SAMPLES,
	       (unsigned int) SP_MARKS, sp_pair_cost,
	       (unsigned int) SP_MARKS * sp_pair_cost);
	printf("syscall_profile   median %s trap %u cyc (spread %u..%u); "
	       "kernel-wide: %u hot-path candidates, %u hand-offs\n",
	       nhot == 0 ? "NON-HANDOFF" : "hand-off", whole, col[0],
	       col[n - 1], c_mmot_combined_S_R, c_mach_msg_trap_switch_fast);

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
		int	k = 0;

		for (i = 0; i < SP_SAMPLES; i++)
			if (nhot == 0 || p->hot[i])
				col[k++] = p->sample[i][ph];
		sp_sort(col, k);

		printf("syscall_profile   %s %10u ", sp_name[ph],
		       p->sample[median][ph]);
		sp_print_pct(sp_percent(p->sample[median][ph],
				        ph == SP_WAIT ? whole : work));
		printf(" %s  [%u..%u]\n",
		       ph == SP_WAIT ? "of wall " : "of work",
		       col[0], col[k - 1]);
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
		 * to be worked out from the table.
		 *
		 * #411 printed a BOUND here -- floor against the whole body --
		 * because the body was one bucket and a bound was all it could
		 * support.  With the body split the answer itself is printable:
		 * the ceiling is the two copies and nothing else, because those
		 * are the only two phases carrying the payload in registers
		 * would remove.  Everything else -- the entry, the resolve, the
		 * claim, the switch, the return -- is paid whatever the message
		 * travels in.
		 *
		 * ⚠️ The share is of WORK, not of the trap.  A trap that waited
		 * for a message spent most of its life off the processor, and a
		 * percentage taken against that total would say the copies are
		 * negligible -- which is true of the wall clock and false of
		 * everything #392 is deciding.
		 */
		work += ret_mean;
		copies = p->sample[median][SP_COPYIN] + p->sample[median][SP_PUT];

		printf("syscall_profile   #391 ceiling: the two copies %u cyc"
		       " (copyin %u + put %u), share",
		       copies, p->sample[median][SP_COPYIN],
		       p->sample[median][SP_PUT]);
		sp_print_pct(sp_percent(copies, work));
		printf(" of the %u cyc this trap spent ON a processor\n", work);

		/*
		 * And the switch beside it, because that is the phase the
		 * x86-64-against-i386 comparison pointed at and the one #391
		 * does not touch at all.  Two numbers on two lines, so that
		 * whichever is the larger is the one the reader sees first.
		 */
		printf("syscall_profile   floor: entry %u + return %u + switch"
		       " %u = %u cyc, share",
		       p->sample[median][SP_ENTRY], ret_mean,
		       p->sample[median][SP_SWITCH],
		       p->sample[median][SP_ENTRY] + ret_mean +
		       p->sample[median][SP_SWITCH]);
		sp_print_pct(sp_percent(p->sample[median][SP_ENTRY] + ret_mean +
					p->sample[median][SP_SWITCH], work));
		printf(" of the same %u\n", work);

		/*
		 * 🔑 And the counterweight on one line, because it is what the
		 * first divided run turned out to be about: the hand-off
		 * machinery against the two copies.  Six columns that exist
		 * because one thread stops running and another starts.
		 */
		printf("syscall_profile   hand-off machinery: pick %u + claim "
		       "%u + park %u + deliver %u + switch %u + splx %u = %u, "
		       "share",
		       p->sample[median][SP_PICK], p->sample[median][SP_CLAIM],
		       p->sample[median][SP_PARK],
		       p->sample[median][SP_DELIVER],
		       p->sample[median][SP_SWITCH], p->sample[median][SP_SPL],
		       p->sample[median][SP_PICK] + p->sample[median][SP_CLAIM] +
		       p->sample[median][SP_PARK] +
		       p->sample[median][SP_DELIVER] +
		       p->sample[median][SP_SWITCH] + p->sample[median][SP_SPL]);
		sp_print_pct(sp_percent(p->sample[median][SP_PICK] +
					p->sample[median][SP_CLAIM] +
					p->sample[median][SP_PARK] +
					p->sample[median][SP_DELIVER] +
					p->sample[median][SP_SWITCH] +
					p->sample[median][SP_SPL], work));
		printf(" of the same %u\n", work);
	}

	/*
	 * ⚠️ Printed only while some site is dark, so that a working instrument
	 * does not spend a line a dump saying it is working.
	 */
	/*
	 * ⚠️ Printed only when THIS breakdown has nothing in it, which is the
	 * case where the reader needs to know whether the instrument or the
	 * path is the reason.  A dump with slices does not spend a line saying
	 * the instrument works.
	 */
	if (p->sample[median][SP_GET] == 0 &&
	    p->sample[median][SP_RESOLVE] == 0 &&
	    p->sample[median][SP_QUEUE] == 0) {
		printf("syscall_profile   this thread marked nothing; sites "
		       "kernel-wide (reached/of those with no sample open):");
		for (ph = 0; ph < SP_PHASES; ph++)
			if (sp_site[ph] != 0)
				printf(" %s=%u/%u", sp_name[ph], sp_site[ph],
				       sp_site_shut[ph]);
		printf("\n");
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

	t->syscall_profile.self = (const void *) t;
	syscall_profile_begin(&t->syscall_profile, syscall_profile_entry_tsc());
	/*
	 * The entry phase closes HERE, at the first instruction that could
	 * close it: everything between the stub's clock reading and this line
	 * is what a Mach trap costs before it has begun doing what it was
	 * asked.  That is the number #392 weighs the copies against.
	 */
	syscall_profile_mark(&t->syscall_profile, SP_ENTRY);
}

/*
 * ⚠️ `next' is stamped even when `old' is not being profiled, and that is the
 * whole point of passing it: whether a thread's switch can be measured must not
 * depend on who happened to be running before it.
 */
void
syscall_profile_blocked(thread_t old, thread_t next, int phase)
{
	struct syscall_profile_thread	*np;

	np = (next == THREAD_NULL) ? (struct syscall_profile_thread *) 0
				   : &next->syscall_profile;

	if (old == THREAD_NULL) {
		if (np != (struct syscall_profile_thread *) 0)
			np->switch_in = syscall_profile_tsc();
		return;
	}

	syscall_profile_switch_out(&old->syscall_profile, np, phase);
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
syscall_profile_switched_in(void)
{
	thread_t	t = current_thread();

	if (t == THREAD_NULL)
		return;

	syscall_profile_switched(&t->syscall_profile);
}

void
syscall_profile_took_handoff(void)
{
	thread_t	t = current_thread();

	if (t == THREAD_NULL)
		return;

	syscall_profile_handoff(&t->syscall_profile);
}

void
syscall_profile_phase(int phase)
{
	thread_t	t = current_thread();

	sp_site[phase]++;
	if (t == THREAD_NULL || !t->syscall_profile.open) {
		sp_site_shut[phase]++;
		return;
	}

	syscall_profile_mark(&t->syscall_profile, phase);
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
