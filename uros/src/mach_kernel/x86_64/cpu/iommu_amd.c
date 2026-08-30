/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * AMD-Vi, read out of the IVRS (#432, stage 1).
 *
 * The same job as cpu/iommu_vtd.c and almost none of the same encoding.  What
 * the two share is the answer -- some engines, each covering some devices,
 * plus memory certain devices must go on reaching -- and that is exactly what
 * <cpu/iommu.h> was shaped to hold.  Writing this reader is what tested that
 * shape; the description did not change to admit it, which is the property
 * #432 asked for when it said the second vendor should be a backend.
 *
 * The three places it pushed back, and where the differences went:
 *
 *   - AMD names devices in RANGES.  A scope carries a last bus/device/function
 *     for this reason and Intel simply sets it equal to the first.
 *
 *   - A device entry's LENGTH is encoded in the top two bits of its type byte,
 *     so entries this reader does not understand are still stepped over
 *     exactly.  Intel puts the length in a field of its own.
 *
 *   - There is no version register at a known offset.  What there is instead
 *     is better, and is used below.
 */

#include <stdint.h>

#include <cpu/acpi.h>
#include <cpu/iommu_backend.h>
#include <cpu/pci_cfg.h>
#include <pmap/pmap.h>

/* ------------------------------------------------------------------ */
/*  The table                                                           */
/* ------------------------------------------------------------------ */

struct ivrs_table {
	struct acpi_header	header;
	uint32_t		iv_info;
	uint64_t		reserved;
	/* IVHD and IVMD blocks follow */
} __attribute__((packed));

_Static_assert(sizeof(struct ivrs_table) == 48,
	       "the IVRS header is forty-eight bytes before its blocks");

/*
 * How wide a physical address is on this machine, out of IVinfo.
 *
 * ⚠️ This one field is checked by what it ANSWERS rather than by the
 * specification alone: a width read at the wrong offset comes out as zero or
 * as something absurd, and a plausible width is a strong statement that the
 * field is where this thinks it is.  It is the only field in this file read
 * from IVinfo, precisely because the rest of that word is a set of bits whose
 * positions moved between revisions of the document -- see the note on the
 * feature registers below.
 */
#define	IVINFO_PA_SIZE(w)	((unsigned)(((w) >> 8) & 0x7F))

struct ivrs_block {
	uint8_t		type;
	uint8_t		flags;
	uint16_t	length;
} __attribute__((packed));

/*
 * An engine.  Three type numbers for the same block, differing in what sits
 * between the fixed part and the device entries: type 0x10 has nothing there,
 * 0x11 and 0x40 carry a copy of the engine's feature register.
 *
 * 🔑 Which is why the device entries do not start at a constant offset, and
 * why getting that wrong would not be caught by a length check -- the block's
 * own length would still add up while every device in it was read out of the
 * feature words.
 */
#define	IVRS_IVHD_10		0x10
#define	IVRS_IVHD_11		0x11
#define	IVRS_IVHD_40		0x40

#define	IVHD_ENTRIES_10		24	/* no feature image  */
#define	IVHD_ENTRIES_11		40	/* two feature words */

struct ivhd_header {
	struct ivrs_block	block;
	uint16_t		device_id;	/* the engine's OWN pci bdf */
	uint16_t		cap_offset;	/* ... and its capability   */
	uint64_t		base;		/* its registers            */
	uint16_t		segment;
	uint16_t		info;
	uint32_t		attributes;
} __attribute__((packed));

_Static_assert(sizeof(struct ivhd_header) == IVHD_ENTRIES_10,
	       "an IVHD's fixed part is where its type-0x10 entries begin");

/*
 * Memory a device must go on reaching -- AMD's answer to Intel's RMRR.  Three
 * type numbers: for all devices, for one, and for a range.
 */
#define	IVRS_IVMD_ALL		0x20
#define	IVRS_IVMD_ONE		0x21
#define	IVRS_IVMD_RANGE		0x22

struct ivmd_header {
	struct ivrs_block	block;
	uint16_t		device_id;
	uint16_t		aux;
	uint64_t		reserved;
	uint64_t		start;
	uint64_t		length;		/* bytes, not a limit */
} __attribute__((packed));

_Static_assert(sizeof(struct ivmd_header) == 32, "an IVMD is thirty-two bytes");

/*
 * Device entries.  The top two bits of the type byte give the length: 4, 8,
 * 16 or 32 bytes -- so an entry this reader has never heard of is still
 * stepped over by exactly the right amount, which is the property that lets
 * the cursor check mean anything.
 */
#define	IVHD_ENTRY_LENGTH(t)	(4u << ((t) >> 6))

#define	IVHD_DEV_PAD		0x00
#define	IVHD_DEV_ALL		0x01
#define	IVHD_DEV_SELECT		0x02
#define	IVHD_DEV_RANGE_START	0x03
#define	IVHD_DEV_RANGE_END	0x04
#define	IVHD_DEV_ALIAS_SELECT	0x42
#define	IVHD_DEV_ALIAS_RANGE	0x43
#define	IVHD_DEV_EXT_SELECT	0x46
#define	IVHD_DEV_EXT_RANGE	0x47
#define	IVHD_DEV_SPECIAL	0x48

#define	IVHD_SPECIAL_IOAPIC	1
#define	IVHD_SPECIAL_HPET	2

/* ------------------------------------------------------------------ */
/*  The silicon                                                         */
/* ------------------------------------------------------------------ */

/*
 * There is no version register here, and what stands in for one is better.
 *
 * The engine is itself a PCI function, and the IVHD names both its bus,
 * device and function AND the offset of its capability in that function's
 * configuration space.  That capability holds the engine's register base --
 * so the table and the silicon each state the same address independently, and
 * comparing them is a stronger check than any single register read: a
 * misparsed IVHD gets a wrong device, a wrong capability offset and a wrong
 * base, and all three would have to be wrong in the same direction to agree.
 *
 * ⚠️ And it fails SAFE.  If the capability layout below is not what this
 * thinks, the comparison fails and the engine is reported as not having
 * answered -- which is the honest outcome, and is what a guess must produce
 * when it is wrong rather than a number that looks read.
 */
#define	PCI_CAP_ID_AMD_IOMMU	0x0F
#define	AMD_CAP_BASE_LOW	0x04	/* bit 0 enables; base is bits 31:14 */
#define	AMD_CAP_BASE_HIGH	0x08

/*
 * The engine's feature register.
 *
 * 🔴 READ AND KEPT RAW, AND DELIBERATELY NOT DECODED.  The bits that say how
 * wide the engine translates, how deep a page table it will walk and whether
 * its walks are coherent are all in here, and their positions differ between
 * revisions of AMD's specification in ways I could not settle from the
 * document I had.  A decode written from memory would produce numbers of
 * exactly the shape this stage exists to distrust: real bits from a real
 * register, read one field over, agreeing with nothing but themselves.
 *
 * So the word is recorded whole, the neutral fields it would have filled are
 * left at zero, and iommu_selftest() says "not decoded" rather than printing a
 * width nobody established.  ⚠️ Stage 2 decodes it against the specification,
 * and until then iommu_address_bits_ok() answers no on this vendor -- which is
 * a refusal to promise, and is the correct answer to a question nobody has
 * looked up.
 */
#define	AMD_REG_CONTROL		0x18
#define	AMD_REG_EXT_FEATURE	0x30

static int is_ivhd(uint8_t type)
{
	return type == IVRS_IVHD_10 || type == IVRS_IVHD_11
	    || type == IVRS_IVHD_40;
}

/*
 * The highest IVHD type in the table that describes the engine at `base'.
 *
 * 🔴 AN IVRS DESCRIBES ONE ENGINE SEVERAL TIMES.  The types are not different
 * engines and not variants of one: they are the same physical unit written out
 * again for a reader of a different vintage, and an operating system is meant
 * to take the highest it understands and ignore the rest.
 *
 * Reading them as separate units is not a cosmetic error.  QEMU's amd-iommu
 * emits two, and this file reported two engines at 0xfed80000 with identical
 * scopes and identical feature words -- which is arithmetically impossible,
 * two engines cannot share an address, and is exactly how the mistake
 * announced itself.  Downstream it would have been worse than a wrong count:
 * stage 3 programs each unit, and programming one engine twice through two
 * descriptions of it is a machine whose second write undoes the first.
 *
 * ⚠️ Highest rather than first, and that is not a preference.  Type 0x10
 * carries no copy of the engine's feature register and 0x11 and 0x40 do, so
 * "the first one" would be the description with the least in it -- and the
 * loss would only surface at stage 2, in a field that was simply absent.
 * Choosing here costs a second pass over bytes already in the direct map.
 */
static uint8_t highest_type_for(const uint8_t *from, const uint8_t *end,
				uint64_t base)
{
	uint8_t best = 0;

	while (from + sizeof(struct ivrs_block) <= end) {
		const struct ivrs_block *b = (const struct ivrs_block *)from;
		const struct ivhd_header *h = (const struct ivhd_header *)from;

		if (b->length < sizeof(*b) || from + b->length > end)
			break;

		if (is_ivhd(b->type) && b->length >= sizeof(*h)
		    && h->base == base && b->type > best)
			best = b->type;

		from += b->length;
	}

	return best;
}

/* A scope whose range is a single device: AMD's ordinary case. */
static void one_device(uint16_t segment, uint16_t bdf, uint8_t kind,
		       uint8_t enumeration_id)
{
	struct iommu_scope s;

	s.kind = kind;
	s.enumeration_id = enumeration_id;
	s.segment = segment;
	s.bus = (uint8_t)(bdf >> 8);
	s.dev = (uint8_t)((bdf >> 3) & 0x1F);
	s.func = (uint8_t)(bdf & 0x7);
	s.last_bus = s.bus;
	s.last_dev = s.dev;
	s.last_func = s.func;
	s.depth = 1;

	iommu_record_scope(&s);
}

static void device_range(uint16_t segment, uint16_t first, uint16_t last)
{
	struct iommu_scope s;

	s.kind = IOMMU_SCOPE_ENDPOINT;
	s.enumeration_id = 0;
	s.segment = segment;
	s.bus = (uint8_t)(first >> 8);
	s.dev = (uint8_t)((first >> 3) & 0x1F);
	s.func = (uint8_t)(first & 0x7);
	s.last_bus = (uint8_t)(last >> 8);
	s.last_dev = (uint8_t)((last >> 3) & 0x1F);
	s.last_func = (uint8_t)(last & 0x7);
	s.depth = 1;

	iommu_record_scope(&s);
}

/*
 * Whether this block's device entries say "everything".
 *
 * A pass of its own, before the block is recorded, because covers_rest is
 * settled when the unit is created and the entry that says so can be anywhere
 * among the entries.  ⚠️ Two passes over the same bytes with the same stepping
 * rule -- if they ever disagree about where an entry ends, the second pass's
 * cursor check is what notices.
 */
static int entries_say_all(const uint8_t *p, const uint8_t *end)
{
	while (p + 4 <= end) {
		unsigned len = IVHD_ENTRY_LENGTH(p[0]);

		if (p + len > end)
			return 0;
		if (p[0] == IVHD_DEV_ALL)
			return 1;
		p += len;
	}

	return 0;
}

/*
 * Record what one block's device entries name.  Answers whether the entries
 * ended exactly on the block's end.
 */
static int read_entries(const uint8_t *p, const uint8_t *end, uint16_t segment)
{
	uint16_t range_first = 0;
	int in_range = 0;

	while (p + 4 <= end) {
		uint8_t type = p[0];
		unsigned len = IVHD_ENTRY_LENGTH(type);
		uint16_t bdf;

		if (p + len > end)
			return 0;

		bdf = (uint16_t)(p[1] | ((uint16_t)p[2] << 8));

		if (type == IVHD_DEV_SELECT
		    || type == IVHD_DEV_ALIAS_SELECT
		    || type == IVHD_DEV_EXT_SELECT) {
			one_device(segment, bdf, IOMMU_SCOPE_ENDPOINT, 0);
		} else if (type == IVHD_DEV_RANGE_START
			   || type == IVHD_DEV_ALIAS_RANGE
			   || type == IVHD_DEV_EXT_RANGE) {
			range_first = bdf;
			in_range = 1;
		} else if (type == IVHD_DEV_RANGE_END) {
			/*
			 * An end with no start names nothing.  Dropped rather
			 * than recorded from a zero, because a scope covering
			 * 00:00.0 through wherever is a claim about the host
			 * bridge that the table never made.
			 */
			if (in_range)
				device_range(segment, range_first, bdf);
			in_range = 0;
		} else if (type == IVHD_DEV_SPECIAL) {
			/*
			 * The I/O APIC or the HPET.  Its own bdf is in the
			 * entry's first field and is zero; the device it
			 * SOURCES interrupts as is at offset 5, and that is
			 * the one an engine has to know about.
			 */
			uint16_t source = (uint16_t)(p[5] | ((uint16_t)p[6] << 8));

			one_device(segment, source,
				   p[7] == IVHD_SPECIAL_HPET
				   ? IOMMU_SCOPE_HPET : IOMMU_SCOPE_IOAPIC,
				   p[4]);
		}

		/*
		 * Everything else -- padding, "all devices", the ACPI-named
		 * variants -- is stepped over by its declared length.  "All"
		 * was settled by entries_say_all() before the unit existed.
		 */

		p += len;
	}

	return p == end;
}

/*
 * Ask the engine's own PCI capability where it thinks it is, and believe the
 * table only if the two agree.
 */
static void confirm_engine(unsigned index, const struct ivhd_header *h)
{
	uint8_t		bus = (uint8_t)(h->device_id >> 8);
	uint8_t		dev = (uint8_t)((h->device_id >> 3) & 0x1F);
	uint8_t		func = (uint8_t)(h->device_id & 0x7);
	uint32_t	low, high;
	uint64_t	from_silicon;
	uint8_t		cap_id;
	volatile uint8_t *regs;
	uint64_t	control, features;

	if (h->cap_offset < 0x40)
		return;

	cap_id = pci_cfg_read8(h->segment, bus, dev, func, h->cap_offset);
	if (cap_id != PCI_CAP_ID_AMD_IOMMU)
		return;

	low = pci_cfg_read(h->segment, bus, dev, func,
			   (uint16_t)(h->cap_offset + AMD_CAP_BASE_LOW));
	high = pci_cfg_read(h->segment, bus, dev, func,
			    (uint16_t)(h->cap_offset + AMD_CAP_BASE_HIGH));

	from_silicon = ((uint64_t)high << 32) | (low & 0xFFFFC000u);
	if (from_silicon != h->base)
		return;

	/*
	 * Only now are the registers worth mapping: the address has been
	 * stated twice, by two things that did not consult each other.
	 */
	regs = (volatile uint8_t *)(uintptr_t)pmap_map_device(h->base, 0x1000);
	if (regs == 0)
		return;

	control = *(volatile uint64_t *)(regs + AMD_REG_CONTROL);
	features = *(volatile uint64_t *)(regs + AMD_REG_EXT_FEATURE);

	/*
	 * Zero width, zero levels, no claim about remapping or coherency: see
	 * the note on AMD_REG_EXT_FEATURE.  What is being recorded here is
	 * that the engine EXISTS and is where the table said, which is the
	 * whole of what stage 1 set out to establish.
	 */
	iommu_record_hardware(index, 0, 0, 0, 0, 0, features, control);
}

int iommu_amd_read(void)
{
	const struct ivrs_table *ivrs;
	const uint8_t *p, *end, *first_block;
	unsigned first_unit;
	int exact;

	ivrs = (const struct ivrs_table *)acpi_find_table("IVRS");
	if (ivrs == 0)
		return 0;

	if (ivrs->header.length < sizeof(*ivrs))
		return 0;

	/*
	 * ⚠️ No interrupt-remapping claim and no x2apic objection, because the
	 * IVRS makes neither.  AMD's table states which wide-id modes are
	 * SUPPORTED rather than asking that they be avoided, so answering no
	 * here means "nobody objected" -- which is what <cpu/iommu.h> says it
	 * means, and is why that had to be written down.
	 */
	iommu_record_platform(IVINFO_PA_SIZE(ivrs->iv_info), 0, 0);

	first_unit = iommu_unit_count();
	exact = 1;

	first_block = (const uint8_t *)ivrs + sizeof(*ivrs);
	p = first_block;
	end = (const uint8_t *)ivrs + ivrs->header.length;

	while (p + sizeof(struct ivrs_block) <= end) {
		const struct ivrs_block *b = (const struct ivrs_block *)p;

		if (b->length < sizeof(*b) || p + b->length > end) {
			exact = 0;
			break;
		}

		if (is_ivhd(b->type)
		    && b->length >= sizeof(struct ivhd_header)) {
			const struct ivhd_header *h = (const void *)p;
			unsigned at = b->type == IVRS_IVHD_10
				    ? IVHD_ENTRIES_10 : IVHD_ENTRIES_11;
			int index;

			if (b->length < at) {
				exact = 0;
			} else if (b->type != highest_type_for(first_block, end,
							       h->base)) {
				/*
				 * The same engine again, for an older reader.
				 * Stepped over, not recorded -- and it is
				 * still walked past by its own length, so the
				 * cursor check keeps meaning what it means.
				 */
			/*
			 * ⚠️ Window size zero, meaning THE TABLE DID NOT SAY.
			 * An IVHD gives a base and no extent, where a DRHD
			 * gives both -- so writing 4096 here would be putting
			 * this reader's guess in a field whose other filler
			 * puts the firmware's statement, and nothing
			 * downstream could tell the two apart.
			 */
			} else if ((index = iommu_record_unit(h->segment,
						h->base, 0,
						entries_say_all(p + at,
								p + b->length)))
				   >= 0) {
				if (!read_entries(p + at, p + b->length,
						  h->segment))
					exact = 0;

				/*
				 * Confirmed here rather than in a second walk
				 * over the same blocks.  The index is in hand;
				 * a separate pass would have to rediscover it
				 * by repeating this filter, which is a second
				 * copy of a rule that has already been wrong
				 * once.
				 */
				confirm_engine((unsigned)index, h);
			}
		} else if ((b->type == IVRS_IVMD_ALL || b->type == IVRS_IVMD_ONE
			    || b->type == IVRS_IVMD_RANGE)
			   && b->length >= sizeof(struct ivmd_header)) {
			const struct ivmd_header *m = (const void *)p;

			/*
			 * ⚠️ AMD gives a LENGTH where Intel gives a limit, and
			 * <cpu/iommu.h> holds a limit.  Converted here rather
			 * than by making the description hold both, because a
			 * field that means two things depending on who filled
			 * it is a field every reader gets right once.
			 *
			 * A zero-length region would make the limit one below
			 * the base; recorded as base..base instead, so that a
			 * region nobody can map is still a region somebody can
			 * see.
			 */
			uint64_t limit = m->length
				       ? m->start + m->length - 1 : m->start;

			if (iommu_record_reserved(0, m->start, limit) >= 0
			    && b->type != IVRS_IVMD_ALL) {
				if (b->type == IVRS_IVMD_ONE)
					one_device(0, m->device_id,
						   IOMMU_SCOPE_ENDPOINT, 0);
				else
					device_range(0, m->device_id, m->aux);
			}
		}

		p += b->length;
	}

	if (p != end)
		exact = 0;

	if (iommu_unit_count() == first_unit) {
		iommu_record_reset();
		return 0;
	}

	iommu_record_walk(exact);

	return 1;
}
