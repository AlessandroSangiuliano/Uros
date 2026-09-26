/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The log of DMA refusals (#432 stage 3d), and each device's count of them
 * (#599): drained out of the engines by the vendor code, kept here, printed
 * from here.  It reaches the unit table only through iommu_vendor(),
 * iommu_unit_count() and iommu_translating().
 */

#include <stdint.h>

#include <cpu/iommu_backend.h>
#include <kern/assert.h>
#include <kern/misc_protos.h>	/* printf, bzero */
#include <kern/sched_prim.h>	/* the reporter's sleep, #599 */
#include <kern/task.h>		/* kernel_task */
#include <kern/thread.h>
#include <kern/sched.h>		/* sched_tick: one a second */
#include <kern/time_out.h>	/* hz */
#include <sync/lock.h>		/* hw_lock, #599 */

/*
 * ── Stage 3d: the log of refusals ────────────────────────────────────
 *
 * A ring of the last IOMMU_FAULT_LOG, and a count that does not wrap with it.
 * Both are needed and they answer different questions -- see the note on
 * IOMMU_FAULT_LOG in <cpu/iommu.h>.
 *
 * 🔴 #599: iommu_fault_lock guards every drain, the ring, the counts and the
 * per-device table.  This said "No lock -- the day a second processor can poll
 * is the day this needs one", and that day had come: the idle loop of every
 * processor and a driver's RPC drained the same engines, and two drains could
 * each move an engine's head, count one record twice, or write the head
 * backwards.
 *
 * The rule, which is why it is a raw hw_lock:
 *  - a LEAF.  While holding it, code touches only engine registers, the
 *    engines' log memory and what this file keeps.  No printf, no mutex, no
 *    allocation, no thread_wakeup -- the report copies what it will print
 *    and prints after letting go;
 *  - no interrupt handler takes it;
 *  - never held with iommu_domain_lock, and mechanically so: this file
 *    cannot see that mutex (static in iommu.c).
 * hw_lock is real in every configuration -- simple_lock and mutex compile to
 * nothing on an NCPUS==1 build -- and it masks interrupts and disables
 * preemption for the hold.  Zero is free: no init, no init order.
 */
static hw_lock_data_t		iommu_fault_lock;

/* #599: what the reporter sleeps on besides its timeout. */
static int			iommu_fault_wakeup;

static struct iommu_fault	fault_log[IOMMU_FAULT_LOG];
static unsigned			fault_total;

/*
 * #599: drains in which an engine may have discarded a refusal -- today, the
 * ones where it said so (AMD EventOverflow, VT-d PFO).  It only goes up, and
 * with the unplaced count it is what iommu_fault_lost() answers.  This was a
 * flag that stuck at the first overflow, and the report repeated its "floor"
 * line after it for the rest of the boot.
 */
static uint64_t			fault_episodes;

/*
 * Counted per device as well as kept in the ring, and the two are not the same
 * fact.
 *
 * 🔴 A COUNT THAT WRAPS IS NOT A COUNT.  The ring holds the last sixteen
 * refusals and says which; a driver comparing "how many before" with "how many
 * after" needs a number that only goes up, or its own two refusals could be
 * pushed out by a noisier device between the two calls and the comparison
 * would read as "not refused".  That is the one answer this must never give.
 *
 * #599: AND IT LIVES HERE, NOT IN THE DEVICE'S DOMAIN SLOT.  It was a field of
 * device_domains[]: bumped without the domain lock while a release compacted
 * that array, reset when a domain was opened, and not kept at all for a device
 * in no domain -- a count that could move backwards between a driver's two
 * questions.  This table only grows: a device, once named, keeps its slot for
 * the boot.  A refusal from a device it has no room for is counted in
 * `unplaced', which the report prints.
 */
#define	IOMMU_FAULT_DEVICES	32	/* > IOMMU_MAX_DEVICE_DOMAINS */

struct fault_device {
	uint16_t	bdf;
	uint16_t	used;
	uint64_t	recorded;
	uint64_t	last_address;
};

struct fault_table {
	struct fault_device	dev[IOMMU_FAULT_DEVICES];
	uint64_t		unplaced;
};

static struct fault_table	fault_devices;

/* Count one refusal of `bdf' at `address'.  Slots fill in order, no holes. */
static void fault_table_note(struct fault_table *t, uint16_t bdf,
			     uint64_t address)
{
	for (unsigned i = 0; i < IOMMU_FAULT_DEVICES; i++) {
		struct fault_device *d = &t->dev[i];

		if (!d->used) {
			d->used = 1;
			d->bdf = bdf;
		} else if (d->bdf != bdf) {
			continue;
		}
		d->recorded++;
		d->last_address = address;
		return;
	}
	t->unplaced++;
}

static const struct fault_device *
fault_table_find(const struct fault_table *t, uint16_t bdf)
{
	for (unsigned i = 0; i < IOMMU_FAULT_DEVICES && t->dev[i].used; i++)
		if (t->dev[i].bdf == bdf)
			return &t->dev[i];

	return 0;
}

/*
 * #599: the vendor drains reach the log only through a sink, and a live sink
 * exists only inside drain_all_locked(), after the lock is taken.  The type
 * is complete only in this file.
 */
struct iommu_fault_sink {
	int		live;
	unsigned	found;
};

void iommu_fault_sink_record(struct iommu_fault_sink *s,
			     const struct iommu_fault *f)
{
	assert(!s->live || hw_lock_held(&iommu_fault_lock));
	fault_log[fault_total % IOMMU_FAULT_LOG] = *f;
	fault_total++;
	fault_table_note(&fault_devices, f->source, f->address);
	s->found++;
}

void iommu_fault_sink_lost(struct iommu_fault_sink *s, unsigned unit,
			   unsigned why)
{
	(void)unit;
	(void)why;
	assert(!s->live || hw_lock_held(&iommu_fault_lock));
	fault_episodes++;
}

/*
 * #599: the table asked about itself, at every boot, on a scratch copy: 33
 * devices refused once each -- 32 named, the 33rd unplaced and not found --
 * and one refused twice, which counts 2 and keeps the second address.
 */
int iommu_fault_ledger_check(unsigned *ran, unsigned *wrong)
{
	static struct fault_table t;	/* scratch; too big for the stack */
	const struct fault_device *d;
	unsigned i, named = 0;

	*ran = 0;
	*wrong = 0;
	bzero((char *)&t, sizeof(t));
	for (i = 0; i <= IOMMU_FAULT_DEVICES; i++)
		fault_table_note(&t, (uint16_t)(0x100 + i), 0x1000u * i);
	for (i = 0; i < IOMMU_FAULT_DEVICES; i++) {
		d = fault_table_find(&t, (uint16_t)(0x100 + i));
		if (d != 0 && d->recorded == 1 && d->last_address == 0x1000u * i)
			named++;
	}
	(*ran)++;
	if (named != IOMMU_FAULT_DEVICES)
		(*wrong)++;
	(*ran)++;
	if (t.unplaced != 1)
		(*wrong)++;
	(*ran)++;
	if (fault_table_find(&t, (uint16_t)(0x100 + IOMMU_FAULT_DEVICES)) != 0)
		(*wrong)++;

	bzero((char *)&t, sizeof(t));
	fault_table_note(&t, 0x18, 0xA000);
	fault_table_note(&t, 0x20, 0xB000);
	fault_table_note(&t, 0x18, 0xC000);
	d = fault_table_find(&t, 0x18);
	(*ran)++;
	if (d == 0 || d->recorded != 2)
		(*wrong)++;
	(*ran)++;
	if (d == 0 || d->last_address != 0xC000)
		(*wrong)++;

	return *wrong == 0;
}

/*
 * Every engine drained, with iommu_fault_lock held.
 *
 * ⚠️ Nothing to read before the engines are running.  A unit's fault
 * registers are readable whether or not translation is on, and they are
 * meaningless then -- an engine that is not translating refuses nothing.
 * Reading them anyway would report whatever the firmware left behind as this
 * kernel's own faults.
 */
static unsigned drain_all_locked(void)
{
	struct iommu_fault_sink s = { 1, 0 };

	if (!iommu_translating())
		return 0;

	for (unsigned i = 0; i < iommu_unit_count(); i++)
		if (iommu_vendor() == IOMMU_INTEL)
			(void) iommu_vtd_fault_drain(i, &s);
		else if (iommu_vendor() == IOMMU_AMD)
			(void) iommu_amd_fault_drain(i, &s);

	return s.found;
}

unsigned iommu_fault_poll(void)
{
	unsigned found;

	hw_lock_lock(&iommu_fault_lock);
	found = drain_all_locked();
	hw_lock_unlock(&iommu_fault_lock);
	return found;
}

uint64_t iommu_fault_lost(void)
{
	uint64_t lost;

	hw_lock_lock(&iommu_fault_lock);
	lost = fault_episodes + fault_devices.unplaced;
	hw_lock_unlock(&iommu_fault_lock);
	return lost;
}

/*
 * ── Saying it out loud ───────────────────────────────────────────────
 *
 * 🔑 `reported' AND `fault_total' are two counters and not one.  The ring can
 * wrap between two drains, and then the number of faults that happened is
 * larger than the number of records that survived -- so the reporter says how
 * many it could not show rather than showing the last sixteen and implying
 * that was all of them.
 *
 * #599: printed by one thread only, the reporter below, and the cursors are
 * its own: nothing else can print these lines or move them.  The idle loop
 * used to print them, once every 4096 passes, and a driver's question too --
 * a processor spinning in a driver never idles, so a refusal nobody asked
 * about waited for the spin to end, and a uniprocessor boot could wait for
 * ever.  Draining and printing are apart now: a driver's question drains and
 * never prints.
 */

static const char *fault_kind_name(uint8_t kind)
{
	switch (kind) {
	case IOMMU_FAULT_PAGE:		return "no mapping, or no permission";
	case IOMMU_FAULT_ENTRY:		return "the device's own entry";
	case IOMMU_FAULT_HARDWARE:	return "the engine could not read a table";
	default:			return "a reason this kernel does not read";
	}
}

static unsigned fault_report_print(void)
{
	static unsigned reported;
	static uint64_t reported_unplaced, reported_episodes;
	struct iommu_fault copy[IOMMU_FAULT_LOG];
	uint64_t unplaced, episodes;
	unsigned n = 0, from, oldest, lost, printed = 0;

	/*
	 * #599: drain, claim [reported, fault_total), copy, let go -- then
	 * print.  Claiming under the lock is what keeps two callers from
	 * printing the same records; printing outside it is the lock's rule.
	 *
	 * 🔑 The ring's element i is the (fault_total - logged + i)th fault of
	 * the boot, and that number is what says whether it has been printed.
	 * Comparing positions inside the ring could not: the ring's element
	 * zero is a different fault after every wrap.
	 */
	hw_lock_lock(&iommu_fault_lock);
	(void) drain_all_locked();
	oldest = fault_total > IOMMU_FAULT_LOG ? fault_total - IOMMU_FAULT_LOG
					       : 0;
	from = reported > oldest ? reported : oldest;
	lost = from - reported;		/* wrapped out before anyone printed */
	for (unsigned k = from; k != fault_total; k++)
		copy[n++] = fault_log[k % IOMMU_FAULT_LOG];
	reported = fault_total;
	unplaced = fault_devices.unplaced - reported_unplaced;
	reported_unplaced = fault_devices.unplaced;
	episodes = fault_episodes - reported_episodes;
	reported_episodes = fault_episodes;
	hw_lock_unlock(&iommu_fault_lock);

	for (unsigned i = 0; i < n; i++) {
		const struct iommu_fault *f = &copy[i];

		printf("iommu: %02x:%02x.%u was REFUSED a %s at "
		       "0x%lx — %s (reason 0x%02x)\n",
		       (unsigned)(f->source >> 8),
		       (unsigned)((f->source >> 3) & 0x1F),
		       (unsigned)(f->source & 7),
		       f->write ? "write" : "transfer",
		       (unsigned long)f->address,
		       fault_kind_name(f->kind), (unsigned)f->reason);
		printed++;
	}
	if (lost != 0)
		printf("iommu: and %u more that this log had no room for\n",
		       lost);
	if (unplaced != 0)
		printf("iommu: %llu refusal(s) from devices the per-device count "
		       "has no room to name, %llu since boot (#599)\n",
		       (unsigned long long)unplaced,
		       (unsigned long long)reported_unplaced);
	if (episodes != 0)
		printf("iommu: an engine ran out of fault records before"
		       " anyone read them, %llu time(s) since the last report"
		       " — the count above is a floor\n",
		       (unsigned long long)episodes);

	/* Anything said: the reporter's once-a-second limit counts it all. */
	return printed + (lost != 0) + (unplaced != 0) + (episodes != 0);
}

void iommu_fault_ask(uint16_t bdf, struct iommu_fault_answer *a)
{
	/*
	 * #599: the count and the last address, both from the per-device
	 * table, which only grows (the address came from the ring, and a
	 * noisier device could push it out while the count stayed); and how
	 * many of them this drain found, which is what a driver's refusal
	 * looks like when nothing else read the engines in time.
	 */
	const struct fault_device *d;
	uint64_t before;

	hw_lock_lock(&iommu_fault_lock);
	d = fault_table_find(&fault_devices, bdf);
	before = d != 0 ? d->recorded : 0;
	(void) drain_all_locked();
	d = fault_table_find(&fault_devices, bdf);
	a->recorded = d != 0 ? d->recorded : 0;
	a->last_address = d != 0 ? d->last_address : 0;
	a->undrained = a->recorded - before;
	a->lost = fault_episodes + fault_devices.unplaced;
	hw_lock_unlock(&iommu_fault_lock);

	if (a->undrained != 0)
		thread_wakeup((event_t)&iommu_fault_wakeup);	/* to print */
}

/*
 * ── The reporter (#599) ──────────────────────────────────────────────
 *
 * A kernel thread that drains every engine every 100 ms, whether or not any
 * processor idles and whether or not any driver asks, and prints what is new
 * at most once a second.  At kernel_thread's default priority, below the
 * clock's softclock, which delivers the timed wakeup it sleeps on.
 *
 * ⚠️ A wakeup that arrives between the drain and the sleep is not lost for
 * good: it costs at most one period, 100 ms, when the timeout ends the sleep.
 * A thread spinning at or above this priority, or with preemption off,
 * starves it -- which no interrupt would cure, since the thread must run.
 */
unsigned long iommu_fault_reporter_passes;	/* nm/gdb: rises ~10 a second */

static void iommu_fault_reporter(void)
{
	unsigned last_print = sched_tick;
	int period = hz / 10 ? hz / 10 : 1;

	printf("iommu: refusals are read out every %d ms by a thread of their "
	       "own, and printed at most once a second (#599)\n",
	       period * 1000 / hz);
	for (;;) {
		iommu_fault_reporter_passes++;
		if (sched_tick != last_print) {	/* a second has turned */
			if (fault_report_print() != 0)
				last_print = sched_tick;
		} else {
			(void) iommu_fault_poll();
		}
		assert_wait((event_t)&iommu_fault_wakeup, FALSE);
		thread_set_timeout(period);
		thread_block((void (*)(void)) 0);
		reset_timeout_check(&current_thread()->timer);
	}
}

void iommu_fault_reporter_start(void)
{
	if (!iommu_translating()) {
		printf("iommu: the fault reporter: NOT ASKED — no engine is "
		       "translating, so nothing can be refused (#599)\n");
		return;
	}
	(void) kernel_thread(kernel_task, iommu_fault_reporter, (char *) 0);
}
