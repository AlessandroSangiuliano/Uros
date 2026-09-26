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
#include <cpu/quiet_census.h>	/* #599: the reporter is not work */

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

/*
 * #599: everything the log keeps, in one place, so that the boot check can
 * drain fabricated engines into a scratch copy through the same sink.
 *
 * `episodes' counts drains in which an engine may have discarded a refusal:
 * it said so, its log was full or filled while read, or it logged an entry it
 * never wrote (`why' keeps which, since boot).  It only goes up, and with the
 * unplaced count it is what iommu_fault_lost() answers.  It was a flag that
 * stuck at the first overflow, and the report repeated its "floor" line after
 * it for the rest of the boot.
 */
struct fault_ledger {
	struct iommu_fault	ring[IOMMU_FAULT_LOG];
	unsigned		total;
	struct fault_table	table;
	uint64_t		episodes;
	unsigned		why;		/* IOMMU_LOST_*, since boot */
	uint32_t		stopped;	/* a bit per unit: its log was
						   found stopped last drain */
	uint32_t		blind;		/* ... and the drain before */
};

static struct fault_ledger	ledger;

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
	struct fault_ledger	*l;
	int			 live;
	unsigned		 found;
};

void iommu_fault_sink_record(struct iommu_fault_sink *s,
			     const struct iommu_fault *f)
{
	struct fault_ledger *l = s->l;

	assert(!s->live || hw_lock_held(&iommu_fault_lock));
	l->ring[l->total % IOMMU_FAULT_LOG] = *f;
	l->total++;
	fault_table_note(&l->table, f->source, f->address);
	s->found++;
}

void iommu_fault_sink_stopped(struct iommu_fault_sink *s, unsigned unit,
			      int stopped)
{
	struct fault_ledger *l = s->l;
	uint32_t bit = unit < 32 ? 1u << unit : 0;

	assert(!s->live || hw_lock_held(&iommu_fault_lock));
	if (!stopped) {
		l->stopped &= ~bit;
		l->blind &= ~bit;
		return;
	}
	if (l->stopped & bit)
		l->blind |= bit;	/* a restart did not bring it back */
	l->stopped |= bit;
}

void iommu_fault_sink_lost(struct iommu_fault_sink *s, unsigned unit,
			   unsigned why)
{
	(void)unit;
	assert(!s->live || hw_lock_held(&iommu_fault_lock));
	s->l->episodes++;
	s->l->why |= why;
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
 * ── The drains, asked about themselves (#599) ─────────────────────────
 *
 * At every boot, on fabricated engines: memory laid out as an engine's
 * registers and log, drained by the vendors' own cores into a scratch ledger
 * through a sink that is not live.  A write-one-to-clear lands in a word of
 * its own, so the check reads what was written while the "register" it reads
 * stays put.  What static memory cannot do is move a tail during a drain, so
 * a ring that fills while it is read is not asked here; it is written at the
 * core.
 */
#define	FAKE_AMD_BYTES		4096u
#define	FAKE_AMD_OVERFLOW	(1ULL << 0)	/* MMIO 2020h, EventOverflow */
#define	FAKE_AMD_RUN		(1ULL << 3)	/* MMIO 2020h, EventLogRun */
#define	FAKE_AMD_LOG_EN		(1ULL << 2)	/* MMIO 0018h, EventLogEn */
#define	FAKE_VTD_PFO		(1u << 0)	/* FSTS */
#define	FAKE_VTD_PPF		(1u << 1)
#define	FAKE_VTD_F		(1ULL << 63)	/* a fault record's F */

static uint64_t			fake_amd_log[FAKE_AMD_BYTES / 8];
static uint64_t			fake_vtd_rec[2 * 2];
static struct fault_ledger	fake_ledger;

struct fake_amd {
	uint64_t	head, tail, status, status_w1c, control;
};

/* An IO_PAGE_FAULT event from `bdf' at `address', in the slot at `off'. */
static void fake_amd_event(unsigned off, uint16_t bdf, uint64_t address)
{
	fake_amd_log[off / 8] = (2ULL << 60) | bdf;	/* EventCode 0010b */
	fake_amd_log[off / 8 + 1] = address;
}

/* Drain the fabricated AMD engine; how many found, and episodes added. */
static unsigned fake_amd_drain(struct fake_amd *r, uint64_t *lost)
{
	struct iommu_amd_evtlog v;
	struct iommu_fault_sink s = { &fake_ledger, 0, 0 };
	uint64_t before = fake_ledger.episodes;

	v.head = &r->head;
	v.tail = &r->tail;
	v.status = &r->status;
	v.status_w1c = &r->status_w1c;
	v.control = &r->control;
	v.log = (volatile uint8_t *)fake_amd_log;
	v.bytes = FAKE_AMD_BYTES;
	(void) iommu_amd_evtlog_drain(&v, 0, &s);
	*lost = fake_ledger.episodes - before;
	return s.found;
}

static unsigned fake_vtd_drain(uint32_t fsts, uint32_t *w1c, uint64_t *lost)
{
	struct iommu_vtd_records v;
	struct iommu_fault_sink s = { &fake_ledger, 0, 0 };
	uint64_t before = fake_ledger.episodes;
	uint32_t f = fsts;

	*w1c = 0;
	v.fsts = &f;
	v.fsts_w1c = w1c;
	v.records = (volatile uint8_t *)fake_vtd_rec;
	v.count = 2;
	(void) iommu_vtd_records_drain(&v, 0, &s);
	*lost = fake_ledger.episodes - before;
	return s.found;
}

/* Case number *ran failed: counted, and its bit set (A1 is bit 0). */
static void fake_failed(unsigned *ran, unsigned *wrong, unsigned *failed)
{
	(*wrong)++;
	*failed |= 1u << (*ran - 1);
}

static int fake_amd_empty(void)
{
	for (unsigned i = 0; i < FAKE_AMD_BYTES / 8; i++)
		if (fake_amd_log[i] != 0)
			return 0;
	return 1;
}

int iommu_fault_drain_check(unsigned *ran, unsigned *wrong, unsigned *failed)
{
	struct fake_amd r;
	uint64_t lost;
	uint32_t w1c;
	unsigned found, off;

	*ran = 0;
	*wrong = 0;
	*failed = 0;
	bzero((char *)&fake_ledger, sizeof(fake_ledger));

	/* A1: two entries, read, consumed, head moved, nothing lost. */
	bzero((char *)fake_amd_log, sizeof(fake_amd_log));
	bzero((char *)&r, sizeof(r));
	fake_amd_event(0, 0x20, 0xA000);
	fake_amd_event(16, 0x20, 0xB000);
	r.tail = 32;
	found = fake_amd_drain(&r, &lost);
	(*ran)++;
	if (found != 2 || lost != 0 || r.head != 32 || !fake_amd_empty())
		fake_failed(ran, wrong, failed);

	/* A2: a ring that wraps, 0xFE0 -> 0x020: four entries. */
	bzero((char *)&r, sizeof(r));
	for (off = 0xFE0; off != 0x020; off = (off + 16) % FAKE_AMD_BYTES)
		fake_amd_event(off, 0x20, 0xC000 + off);
	r.head = 0xFE0;
	r.tail = 0x020;
	found = fake_amd_drain(&r, &lost);
	(*ran)++;
	if (found != 4 || lost != 0 || r.head != 0x020 || !fake_amd_empty())
		fake_failed(ran, wrong, failed);

	/* A3: a full ring (head one past tail): 255 found, and a loss. */
	bzero((char *)&r, sizeof(r));
	for (off = 0x10; off != 0; off = (off + 16) % FAKE_AMD_BYTES)
		fake_amd_event(off, 0x20, 0xD000);
	r.head = 0x10;
	r.tail = 0;
	found = fake_amd_drain(&r, &lost);
	(*ran)++;
	if (found != 255 || lost != 1 || r.head != 0)
		fake_failed(ran, wrong, failed);

	/* A4: nothing logged, the engine's flag up: a loss, the flag cleared. */
	bzero((char *)&r, sizeof(r));
	r.head = r.tail = 0x40;
	r.status = FAKE_AMD_OVERFLOW;
	found = fake_amd_drain(&r, &lost);
	(*ran)++;
	if (found != 0 || lost != 1 || r.status_w1c != FAKE_AMD_OVERFLOW)
		fake_failed(ran, wrong, failed);

	/* A5: an entry the tail passed and nothing wrote: a loss, consumed. */
	bzero((char *)fake_amd_log, sizeof(fake_amd_log));
	bzero((char *)&r, sizeof(r));
	r.tail = 16;
	found = fake_amd_drain(&r, &lost);
	(*ran)++;
	if (found != 0 || lost != 1 || r.head != 16)
		fake_failed(ran, wrong, failed);

	/*
	 * A6: the log overflowed and stopped (Run clear, EventLogEn set): a
	 * loss, the flag cleared, the log restarted -- EventLogEn left set --
	 * and the unit marked stopped.  A second drain that still finds it
	 * stopped: another loss, and blind.  A8: a drain that finds it running
	 * clears both, and counts nothing.
	 */
	bzero((char *)&r, sizeof(r));
	r.status = FAKE_AMD_OVERFLOW;
	r.control = FAKE_AMD_LOG_EN;
	found = fake_amd_drain(&r, &lost);
	(*ran)++;
	if (found != 0 || lost != 1 || r.status_w1c != FAKE_AMD_OVERFLOW ||
	    r.control != FAKE_AMD_LOG_EN || !(fake_ledger.stopped & 1) ||
	    (fake_ledger.blind & 1))
		fake_failed(ran, wrong, failed);
	found = fake_amd_drain(&r, &lost);
	(*ran)++;
	if (lost != 1 || !(fake_ledger.blind & 1))
		fake_failed(ran, wrong, failed);

	/* A7: stopped with no overflow: a loss, a restart, no flag written. */
	fake_ledger.stopped = fake_ledger.blind = 0;
	bzero((char *)&r, sizeof(r));
	r.control = FAKE_AMD_LOG_EN;
	found = fake_amd_drain(&r, &lost);
	(*ran)++;
	if (found != 0 || lost != 1 || r.status_w1c != 0 ||
	    !(fake_ledger.stopped & 1))
		fake_failed(ran, wrong, failed);

	/* A8: running again: stopped and blind cleared, nothing lost. */
	fake_ledger.blind = 1;
	r.status = FAKE_AMD_RUN;
	found = fake_amd_drain(&r, &lost);
	(*ran)++;
	if (lost != 0 || fake_ledger.stopped != 0 || fake_ledger.blind != 0)
		fake_failed(ran, wrong, failed);

	/* V1: one of two records set: found, and only its F written. */
	fake_vtd_rec[0] = 0;
	fake_vtd_rec[1] = 0;
	fake_vtd_rec[2] = 0xE000;
	fake_vtd_rec[3] = FAKE_VTD_F | 0x20;
	found = fake_vtd_drain(FAKE_VTD_PPF, &w1c, &lost);
	(*ran)++;
	if (found != 1 || lost != 0 || fake_vtd_rec[1] != 0 ||
	    fake_vtd_rec[3] != FAKE_VTD_F || w1c != 0)
		fake_failed(ran, wrong, failed);

	/* V2: both records set and records dropped: two, a loss, PFO cleared. */
	fake_vtd_rec[1] = FAKE_VTD_F | 0x20;
	fake_vtd_rec[3] = FAKE_VTD_F | 0x20;
	found = fake_vtd_drain(FAKE_VTD_PPF | FAKE_VTD_PFO, &w1c, &lost);
	(*ran)++;
	if (found != 2 || lost != 1 || w1c != FAKE_VTD_PFO)
		fake_failed(ran, wrong, failed);

	/* V3: records dropped and none pending: a loss, PFO cleared. */
	fake_vtd_rec[1] = 0;
	fake_vtd_rec[3] = 0;
	found = fake_vtd_drain(FAKE_VTD_PFO, &w1c, &lost);
	(*ran)++;
	if (found != 0 || lost != 1 || w1c != FAKE_VTD_PFO)
		fake_failed(ran, wrong, failed);

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
	struct iommu_fault_sink s = { &ledger, 1, 0 };

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
	lost = ledger.episodes + ledger.table.unplaced;
	hw_lock_unlock(&iommu_fault_lock);
	return lost;
}

/*
 * ── Saying it out loud ───────────────────────────────────────────────
 *
 * 🔑 `reported' AND `ledger.total' are two counters and not one.  The ring can
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
	static uint32_t said_blind;
	uint32_t blind;
	struct iommu_fault copy[IOMMU_FAULT_LOG];
	uint64_t unplaced, episodes;
	unsigned n = 0, from, oldest, lost, printed = 0, why;

	/*
	 * #599: drain, claim [reported, ledger.total), copy, let go -- then
	 * print.  Claiming under the lock is what keeps two callers from
	 * printing the same records; printing outside it is the lock's rule.
	 *
	 * 🔑 The ring's element i is the (ledger.total - logged + i)th fault of
	 * the boot, and that number is what says whether it has been printed.
	 * Comparing positions inside the ring could not: the ring's element
	 * zero is a different fault after every wrap.
	 */
	hw_lock_lock(&iommu_fault_lock);
	(void) drain_all_locked();
	oldest = ledger.total > IOMMU_FAULT_LOG ? ledger.total - IOMMU_FAULT_LOG
					       : 0;
	from = reported > oldest ? reported : oldest;
	lost = from - reported;		/* wrapped out before anyone printed */
	for (unsigned k = from; k != ledger.total; k++)
		copy[n++] = ledger.ring[k % IOMMU_FAULT_LOG];
	reported = ledger.total;
	unplaced = ledger.table.unplaced - reported_unplaced;
	reported_unplaced = ledger.table.unplaced;
	episodes = ledger.episodes - reported_episodes;
	reported_episodes = ledger.episodes;
	why = ledger.why;
	blind = ledger.blind;
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
		printf("iommu: an engine may have discarded refusals, %llu "
		       "time(s) since the last report (seen since boot:%s%s%s%s) "
		       "— every count here is a floor\n",
		       (unsigned long long)episodes,
		       why & IOMMU_LOST_OVERFLOW ? " its own flag" : "",
		       why & IOMMU_LOST_FULL ? " a full log" : "",
		       why & IOMMU_LOST_EMPTY ? " an entry never written" : "",
		       why & IOMMU_LOST_STOPPED ? " a log that had stopped" : "");
	for (unsigned u = 0; u < 32; u++) {
		uint32_t bit = 1u << u;

		if ((blind & bit) && !(said_blind & bit))
			printf("iommu: unit %u's event log stays stopped after a "
			       "restart — its refusals go unrecorded, and every "
			       "drain counts a loss (#599)\n", u);
		else if (!(blind & bit) && (said_blind & bit))
			printf("iommu: unit %u's event log logs again\n", u);
	}
	if (blind != said_blind) {
		said_blind = blind;
		printed++;
	}

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
	d = fault_table_find(&ledger.table, bdf);
	before = d != 0 ? d->recorded : 0;
	(void) drain_all_locked();
	d = fault_table_find(&ledger.table, bdf);
	a->recorded = d != 0 ? d->recorded : 0;
	a->last_address = d != 0 ? d->last_address : 0;
	a->undrained = a->recorded - before;
	a->lost = ledger.episodes + ledger.table.unplaced;
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
	quiet_census_exempt_self();	/* wakes every period, work or not */
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
