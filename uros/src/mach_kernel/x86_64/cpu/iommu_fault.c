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
#include <kern/misc_protos.h>	/* printf, bzero */

/*
 * ── Stage 3d: the log of refusals ────────────────────────────────────
 *
 * A ring of the last IOMMU_FAULT_LOG, and a count that does not wrap with it.
 * Both are needed and they answer different questions -- see the note on
 * IOMMU_FAULT_LOG in <cpu/iommu.h>.
 *
 * ⚠️ No lock.  The only writer is iommu_fault_poll(), and the callers of that
 * are the boot self-test and, later, the fault interrupt -- so the day a
 * second processor can poll is the day this needs one, and it is called out
 * here rather than discovered then.  A ring whose entries are 24 bytes cannot
 * be made safe by making the index atomic.
 */
static struct iommu_fault	fault_log[IOMMU_FAULT_LOG];
static unsigned			fault_total;
static int			fault_overflow;

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

void iommu_record_fault(const struct iommu_fault *f)
{
	fault_log[fault_total % IOMMU_FAULT_LOG] = *f;
	fault_total++;
	fault_table_note(&fault_devices, f->source, f->address);
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

unsigned iommu_fault_count(void)
{
	return fault_total;
}

unsigned iommu_fault_logged(void)
{
	return fault_total < IOMMU_FAULT_LOG ? fault_total : IOMMU_FAULT_LOG;
}

/*
 * Oldest first, which for a wrapped ring is not element zero.
 *
 * 🔑 The caller counts 0..iommu_fault_logged()-1 and gets them in the order
 * they happened, whether or not the ring has wrapped -- which is the property
 * that lets a reporter be written once.  A reader handed the raw array would
 * have to know about the wrap, and every reader would have to know separately.
 */
const struct iommu_fault *iommu_fault(unsigned index)
{
	unsigned logged = iommu_fault_logged();
	unsigned first;

	if (index >= logged)
		return 0;

	first = fault_total < IOMMU_FAULT_LOG
		? 0 : fault_total % IOMMU_FAULT_LOG;

	return &fault_log[(first + index) % IOMMU_FAULT_LOG];
}

int iommu_fault_overflowed(void)
{
	return fault_overflow;
}

unsigned iommu_fault_poll(void)
{
	unsigned found = 0;

	/*
	 * ⚠️ Nothing to read before the engines are running.  A unit's fault
	 * registers are readable whether or not translation is on, and they
	 * are meaningless then -- an engine that is not translating refuses
	 * nothing.  Reading them anyway would report whatever the firmware
	 * left behind as this kernel's own faults.
	 */
	if (!iommu_translating())
		return 0;

	for (unsigned i = 0; i < iommu_unit_count(); i++)
		if (iommu_vendor() == IOMMU_INTEL)
			found += iommu_vtd_fault_drain(i, &fault_overflow);
		else if (iommu_vendor() == IOMMU_AMD)
			found += iommu_amd_fault_drain(i, &fault_overflow);

	return found;
}

/*
 * ── Saying it out loud ───────────────────────────────────────────────
 *
 * 🔑 `reported' AND `fault_total' are two counters and not one.  The ring can
 * wrap between two polls, and then the number of faults that happened is
 * larger than the number of records that survived -- so the reporter says how
 * many it could not show rather than showing the last sixteen and implying
 * that was all of them.
 */
static unsigned reported;

/*
 * 🔥 AND THERE IS NO RATE LIMIT IN HERE, WHICH IS WHERE ONE WAS PUT AND WAS
 * WRONG.  The idle loop calls this thousands of times a second and does want
 * one; a driver asking whether the IOMMU refused its transfer calls the SAME
 * function and must never be told no because the divider had not come round.
 * It was, for one run: the engine refused the DMA, QEMU said so on its own
 * console, and this kernel reported that nothing had been refused.
 *
 * 🔑 The limit belongs to the CALLER WITH THE FREQUENCY PROBLEM, which is the
 * idle loop, and it is there.  A function used by two callers with opposite
 * requirements cannot hold either one's policy.
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

unsigned iommu_fault_report(void)
{
	static uint64_t reported_unplaced;
	unsigned before = fault_total;
	unsigned printed = 0;
	unsigned lost;

	/*
	 * #599: iommu_fault_poll() answers nothing until an engine translates.
	 * This also asked iommu_domain_count() first, outside the lock that
	 * guards it; the poll's own gate is the one that means something.
	 */
	iommu_fault_poll();
	if (fault_total == before)
		return 0;

	/*
	 * How many the ring could not keep.  Two ways to lose one -- the
	 * engine dropped it, which iommu_fault_overflowed() says, and this
	 * ring wrapped, which only arithmetic says.
	 */
	lost = (fault_total - before) > IOMMU_FAULT_LOG
	       ? (fault_total - before) - IOMMU_FAULT_LOG : 0;

	/*
	 * 🔑 The ring's element i is the (fault_total - logged + i)th fault of
	 * the boot, and that number is what says whether it has been printed.
	 * Comparing positions inside the ring could not: the ring's element
	 * zero is a different fault after every wrap.
	 */
	{
		unsigned logged = iommu_fault_logged();
		unsigned first = fault_total - logged;

		for (unsigned i = 0; i < logged; i++) {
			const struct iommu_fault *f = iommu_fault(i);

			if (f == 0 || first + i < reported)
				continue;

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
	}

	reported = fault_total;

	if (lost != 0)
		printf("iommu: and %u more that this log had no room for\n",
		       lost);
	if (fault_devices.unplaced != reported_unplaced) {
		printf("iommu: %llu refusal(s) from devices the per-device count "
		       "has no room to name, %llu since boot (#599)\n",
		       (unsigned long long)(fault_devices.unplaced -
					    reported_unplaced),
		       (unsigned long long)fault_devices.unplaced);
		reported_unplaced = fault_devices.unplaced;
	}
	if (iommu_fault_overflowed())
		printf("iommu: an engine ran out of fault records before"
		       " anyone read them — the count above is a floor\n");

	return printed;
}

unsigned iommu_faults_for(uint16_t bdf, uint64_t *last_address)
{
	/*
	 * #599: the count and the last address, both from the per-device
	 * table, which only grows.  The address came from the ring, and a
	 * noisier device could push it out while the count stayed.
	 */
	const struct fault_device *d = fault_table_find(&fault_devices, bdf);

	if (d == 0)
		return 0;
	if (last_address)
		*last_address = d->last_address;
	return (unsigned)d->recorded;
}
