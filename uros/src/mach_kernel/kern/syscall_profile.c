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
 * Which trap is being profiled -- a slot of the table in kern/syscall_sw.c.
 * 33 is urmach_msg, the path #392 exists to weigh, and it is the default
 * because that is what this instrument was built for.  42 is urmach_futex,
 * #554's block-and-wake round trip.
 *
 * Set at configure time (UROS_SYSCALL_PROFILE_TRAP) so that the subject of a
 * run is visible in the run's own configuration rather than in whoever
 * remembers which arm it was.
 *
 * Defined whether or not the profile is compiled in, so that anything which
 * wants to point it elsewhere links either way.
 */
#ifndef	SYSCALL_PROFILE_TRAP
#define	SYSCALL_PROFILE_TRAP	SP_TRAP_MSG
#endif

int	syscall_profile_trap = SYSCALL_PROFILE_TRAP;

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
 * #559: and the three routes, which are the denominator the breakdown is a
 * fraction of.  See the block above them in ipc/mach_msg.c.
 */
extern unsigned int	c_route_msg_send;
extern unsigned int	c_route_msg_receive;
extern unsigned int	c_route_msg_continue;

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
/*
 * 🔑 TWO VOCABULARIES FOR ONE SET OF SLOTS (#554).  The futex borrows the
 * phases whose work is the same shape and needs its own words for them: see
 * the SP_FX_* block in the header for why the slots are shared rather than
 * grown.  A slot the profiled trap cannot reach prints as "-" instead of a
 * name that would invite a reader to look for it.
 */
static const char *const sp_name_msg[SP_PHASES] = {
	"entry   ",	/* SP_ENTRY   */
	"get buf ",	/* SP_GET     */
	"COPYIN  ",	/* SP_COPYIN  */
	"kmsg_get",	/* SP_KMSGGET */
	"resolve ",	/* SP_RESOLVE */
	"queue   ",	/* SP_QUEUE   */
	"pick rcv",	/* SP_PICK    */
	"claim   ",	/* SP_CLAIM   */
	"park snd",	/* SP_PARK    */
	"deliver ",	/* SP_DELIVER */
	"mq_send ",	/* SP_MQSEND  */
	"wait    ",	/* SP_WAIT    */
	"RUNQ    ",	/* SP_RUNQ    */
	"SWITCH  ",	/* SP_SWITCH  */
	"splx    ",	/* SP_SPL     */
	"mq_recv ",	/* SP_MQRECV  */
	"resume  ",	/* SP_RESUME  */
	"copyout ",	/* SP_COPYOUT */
	"PUT     ",	/* SP_PUT     */
	"residue ",	/* SP_BODY    */
};

static const char *const sp_name_futex[SP_PHASES] = {
	"entry   ",	/* SP_ENTRY               */
	"-       ",	/* SP_GET     unreached   */
	"COPYIN  ",	/* SP_FX_COPYIN: the word */
	"-       ",	/* SP_KMSGGET unreached   */
	"-       ",	/* SP_RESOLVE unreached   */
	"-       ",	/* SP_QUEUE   unreached   */
	"key hash",	/* SP_FX_KEY              */
	"assert_w",	/* SP_FX_ASSERT           */
	"-       ",	/* SP_PARK    unreached   */
	"HANDOFF ",	/* SP_FX_HANDOFF          */
	"-       ",	/* SP_MQSEND  unreached   */
	"wait    ",	/* SP_WAIT                */
	"RUNQ    ",	/* SP_RUNQ                */
	"SWITCH  ",	/* SP_SWITCH              */
	"splx    ",	/* SP_SPL                 */
	"-       ",	/* SP_MQRECV  unreached   */
	"resume  ",	/* SP_FX_RESUME           */
	"-       ",	/* SP_COPYOUT unreached   */
	"-       ",	/* SP_PUT     unreached   */
	"residue ",	/* SP_BODY                */
};

/*
 * Which words this dump speaks.  Read from the trap being profiled rather than
 * passed down, because every call site that prints a phase name already knows
 * nothing about traps and should go on knowing nothing.
 */
static const char *const *
sp_names(void)
{
	return (syscall_profile_trap == SP_TRAP_FUTEX) ? sp_name_futex
						       : sp_name_msg;
}

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

/*
 * 🔥 #559: THE SHAPES IN A WINDOW, and one table for each of them.
 *
 * A window can hold three kinds of trap, and they differ by most of the table:
 * one that took the hand-off, one that blocked without it, and one that never
 * left the processor at all -- the last has no wait, no run-queue and no
 * switch, because none of that happened to it.
 *
 * 🔴 A SLOW-PATH ROUND TRIP IS TWO TRAPS AND THEY ARE DIFFERENT SHAPES.  The
 * send does not block; the receive does.  Printing one median for the window
 * describes neither, and printing only the blocked one -- which is what this
 * did first -- leaves the send half counted in the header and never broken
 * down.  #392's whole finding came from dividing instead of averaging; the same
 * applies to the window itself.
 */
#define	SP_SHAPE_HANDOFF	0
#define	SP_SHAPE_BLOCKED	1
#define	SP_SHAPE_AWAKE		2
#define	SP_SHAPES		3

static const char *const sp_shape_name[SP_SHAPES] = {
	"hand-off",
	"blocked, no hand-off",
	"never left the processor",
};

static int
sp_in_shape(struct syscall_profile_thread *p, int i, int shape)
{
	if (shape == SP_SHAPE_HANDOFF)
		return p->hot[i] != 0;
	if (shape == SP_SHAPE_BLOCKED)
		return p->slept[i] != 0 && p->hot[i] == 0;
	return p->slept[i] == 0;
}

/*
 * One shape's breakdown.
 *
 * ⚠️ Only the columns that are not zero, and the zero ones named on one line
 * after them.  With twenty phases and three shapes the full grid is sixty lines
 * a dump, most of them zeroes -- and a kernel printf costs milliseconds, so the
 * instrument would be perturbing the very thing it is watching.  The zero names
 * are still printed, because a phase that is silently missing from a table
 * cannot be told from one that was never declared.
 */
static void
sp_table(struct syscall_profile_thread *p, int shape, uint32_t ret_mean,
	 int ret_known)
{
	uint32_t	col[SP_SAMPLES];
	uint32_t	whole, work, copies;
	int		n = 0, median = -1, i, ph, primo;

	for (i = 0; i < SP_SAMPLES; i++)
		if (sp_in_shape(p, i, shape))
			col[n++] = p->total[i];
	/*
	 * 🔴 Two samples at least.  A "median" of one is that one sample, and
	 * printing it under the same heading as a median of sixteen invites it
	 * to be read as one.
	 */
	if (n < 2)
		return;

	sp_sort(col, n);
	whole = col[(n - 1) / 2];
	for (i = 0; i < SP_SAMPLES; i++)
		if (p->total[i] == whole && sp_in_shape(p, i, shape)) {
			median = i;
			break;
		}
	if (median < 0)
		return;

	work = 0;
	for (ph = 0; ph < SP_PHASES; ph++)
		if (ph != SP_WAIT)
			work += p->sample[median][ph];
	if (ret_known)
		work += ret_mean;

	printf("syscall_profile  [%s] median of %d: %u cyc "
	       "(spread %u..%u), %u on a processor\n",
	       sp_shape_name[shape], n, whole, col[0], col[n - 1], work);

	for (ph = 0; ph < SP_PHASES; ph++) {
		int	k = 0;

		/*
		 * 🔴 The entry is UNAVAILABLE on a target whose stub takes no
		 * timestamp, which is not the same as free (#554).  Printing it
		 * as a dash keeps "nobody measured this" out of the column a
		 * reader adds up.
		 */
		if (ph == SP_ENTRY && p->entry_unknown) {
			printf("syscall_profile    %s         -  (no entry "
			       "timestamp on this target)\n", sp_names()[ph]);
			continue;
		}

		if (p->sample[median][ph] == 0)
			continue;

		for (i = 0; i < SP_SAMPLES; i++)
			if (sp_in_shape(p, i, shape))
				col[k++] = p->sample[i][ph];
		sp_sort(col, k);

		printf("syscall_profile    %s %9u ", sp_names()[ph],
		       p->sample[median][ph]);
		sp_print_pct(sp_percent(p->sample[median][ph],
					ph == SP_WAIT ? whole : work));
		printf(" %s  [%u..%u]\n",
		       ph == SP_WAIT ? "of wall " : "of work",
		       col[0], col[k - 1]);
	}

	primo = 1;
	for (ph = 0; ph < SP_PHASES; ph++) {
		if (p->sample[median][ph] != 0)
			continue;
		if (primo) {
			printf("syscall_profile    zero here:");
			primo = 0;
		}
		printf(" %s", sp_names()[ph]);
	}
	if (!primo)
		printf("\n");

	if (!ret_known)
		return;

	/*
	 * 🔑 What the issues come here for, on two lines so that whichever is
	 * the larger is the one the reader sees first: what register-IPC (#391)
	 * could reclaim, against what it does not touch.
	 */
	copies = p->sample[median][SP_COPYIN] + p->sample[median][SP_PUT];
	printf("syscall_profile    #391 ceiling: the copies %u cyc "
	       "(copyin %u + put %u), share", copies,
	       p->sample[median][SP_COPYIN], p->sample[median][SP_PUT]);
	sp_print_pct(sp_percent(copies, work));
	printf(" of the %u on a processor\n", work);

	printf("syscall_profile    getting the processor: RUNQ %u + SWITCH %u "
	       "= %u, share",
	       p->sample[median][SP_RUNQ], p->sample[median][SP_SWITCH],
	       p->sample[median][SP_RUNQ] + p->sample[median][SP_SWITCH]);
	sp_print_pct(sp_percent(p->sample[median][SP_RUNQ] +
				p->sample[median][SP_SWITCH], work));
	printf(" of the same %u\n", work);
}

void
syscall_profile_dump(struct syscall_profile_thread *p)
{
	uint64_t	ret_cyc, ret_n;
	uint32_t	ret_mean = 0;
	int		ret_known = 0;
	int		conta[SP_SHAPES];
	int		i, ph, shape;

	if (sp_pair_cost == 0)
		sp_measure_self();

	for (shape = 0; shape < SP_SHAPES; shape++) {
		conta[shape] = 0;
		for (i = 0; i < SP_SAMPLES; i++)
			if (sp_in_shape(p, i, shape))
				conta[shape]++;
	}

	/*
	 * ⚠️ A MEAN, and labelled one, because that is what a running sum can
	 * give and this file will not print a mean that looks like a median.
	 * It is defensible for the return path alone: it runs the same
	 * instructions whatever the message was.
	 */
	syscall_profile_return_cycles(&ret_cyc, &ret_n);
	if (ret_n != 0) {
		ret_mean = (uint32_t) (ret_cyc / ret_n);
		ret_known = 1;
	}

	printf("syscall_profile trap %d thread %p dump %u, window %u = traps "
	       "%u..%u of %u this thread made (%u dropped); shapes: %d "
	       "hand-off, %d blocked, %d awake; %u marks x %u cyc = %u of "
	       "instrument; return %u (mean of %u); kernel-wide %u candidates, "
	       "%u hand-offs\n",
	       syscall_profile_trap, p->self, p->ndumps, p->nwindows,
	       p->nseen - SP_SAMPLES + 1, p->nseen, p->nseen, p->ndropped,
	       conta[SP_SHAPE_HANDOFF], conta[SP_SHAPE_BLOCKED],
	       conta[SP_SHAPE_AWAKE],
	       (unsigned int) SP_MARKS, sp_pair_cost,
	       (unsigned int) SP_MARKS * sp_pair_cost,
	       ret_mean, (unsigned int) ret_n,
	       c_mmot_combined_S_R, c_mach_msg_trap_switch_fast);
	printf("syscall_profile   routes kernel-wide: %u combined (%u handed "
	       "off), %u send-only, %u receive-only (%u of them resumed in the "
	       "continuation)\n",
	       c_mmot_combined_S_R, c_mach_msg_trap_switch_fast,
	       c_route_msg_send, c_route_msg_receive, c_route_msg_continue);

	for (shape = 0; shape < SP_SHAPES; shape++)
		sp_table(p, shape, ret_mean, ret_known);

	/*
	 * ⚠️ Printed only when nothing in this window marked anything, which is
	 * the case where the reader needs to know whether the instrument or the
	 * path is the reason.
	 */
	/*
	 * ⚠️ THE PHASES ASKED ABOUT ARE THE PROFILED TRAP'S OWN (#554).  This
	 * tested three phases of mach_msg, which no futex trap can reach -- so
	 * extending the instrument to a second trap would have made this line
	 * fire on EVERY futex dump.  A guard that is always true is worse than
	 * no guard: it teaches its reader to skip the line that would matter.
	 */
	if (syscall_profile_trap == SP_TRAP_FUTEX
	    ? (sp_site[SP_FX_KEY] == 0 && sp_site[SP_FX_COPYIN] == 0 &&
	       sp_site[SP_FX_ASSERT] == 0)
	    : (sp_site[SP_KMSGGET] == 0 && sp_site[SP_GET] == 0 &&
	       sp_site[SP_RESOLVE] == 0)) {
		printf("syscall_profile   NO SITE REACHED kernel-wide "
		       "(reached/of those with no sample open):");
		for (ph = 0; ph < SP_PHASES; ph++)
			if (sp_site[ph] != 0)
				printf(" %s=%u/%u", sp_names()[ph], sp_site[ph],
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

/*
 * #559: a third thread has made `t' runnable.  Takes the thread rather than
 * reading current_thread(), which at this call site is the waker.
 */
void
syscall_profile_runnable(thread_t t)
{
	if (t == THREAD_NULL)
		return;

	syscall_profile_made_runnable(&t->syscall_profile);
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
