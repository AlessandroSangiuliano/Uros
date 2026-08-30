/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Intel VT-d, read out of the DMAR (#432, stage 1).
 *
 * Finds the remapping engines this machine has, what they cover, and what the
 * firmware says must keep working across the handover -- and then asks each
 * engine's own registers whether the table was telling the truth.
 *
 * 🔑 That last step is what makes this more than a table dump.  A misparsed
 * table produces plausible numbers by construction: they are real bytes from a
 * real table, read one field over.  A base address read one field over names
 * nothing, and nothing reads back as all-ones -- so the version register
 * either says 1.0 or the walk was wrong, and there is no third answer.
 */

#include <stdint.h>

#include <cpu/acpi.h>
#include <cpu/iommu_backend.h>
#include <pmap/pmap.h>

/* ------------------------------------------------------------------ */
/*  The table                                                           */
/* ------------------------------------------------------------------ */

/*
 * The DMAR's own header.
 *
 * `host_address_width' is one less than the number of address bits the
 * PLATFORM can DMA to, which is a different number from the one any individual
 * engine can translate -- the engines answer that themselves, below.
 */
struct dmar_table {
	struct acpi_header	header;
	uint8_t			host_address_width;
	uint8_t			flags;
	uint8_t			reserved[10];
} __attribute__((packed));

/*
 * bit 0 says the platform can remap interrupts.  ⚠️ Not that any engine will:
 * the engine's own ECAP has to agree, and #457's MSI-X table is only policed
 * when both do.
 *
 * bit 1 is the firmware asking us NOT to use x2APIC mode.  Recorded because
 * ignoring it on a machine whose interrupt remapping cannot express wide APIC
 * ids means interrupts that are accepted and delivered nowhere.
 */
#define	DMAR_FLAG_INTR_REMAP	0x1
#define	DMAR_FLAG_X2APIC_OPT_OUT 0x2
#define	DMAR_FLAG_DMA_CTRL_OPT_IN 0x4

_Static_assert(sizeof(struct dmar_table) == 48,
	       "the DMAR header is forty-eight bytes before its structures");

struct dmar_entry {
	uint16_t	type;
	uint16_t	length;
} __attribute__((packed));

#define	DMAR_DRHD	0	/* an engine                                */
#define	DMAR_RMRR	1	/* memory a device must go on reaching      */
#define	DMAR_ATSR	2	/* root ports that support device-side TLBs */
#define	DMAR_RHSA	3	/* which NUMA node an engine is near        */
#define	DMAR_ANDD	4	/* a device named in AML rather than by BDF */

struct dmar_drhd {
	struct dmar_entry	entry;
	uint8_t			flags;
	uint8_t			size;
	uint16_t		segment;
	uint64_t		register_base;
	/* device scopes follow */
} __attribute__((packed));

#define	DRHD_INCLUDE_PCI_ALL	0x1

_Static_assert(sizeof(struct dmar_drhd) == 16, "a DRHD is sixteen bytes");

struct dmar_rmrr {
	struct dmar_entry	entry;
	uint16_t		reserved;
	uint16_t		segment;
	uint64_t		base;
	uint64_t		limit;		/* inclusive */
	/* device scopes follow */
} __attribute__((packed));

_Static_assert(sizeof(struct dmar_rmrr) == 24, "an RMRR is twenty-four bytes");

/*
 * A device scope, and then a path of (device, function) pairs.
 *
 * The path is why a scope is not simply a BDF.  A device behind bridges is
 * named by the bus the FIRST bridge is on and then one pair per hop, so
 * turning it into the address the device answers at means reading each
 * bridge's secondary bus number out of configuration space.  Stage 1 records
 * the first hop and the depth: see the comment on iommu_scope.depth.
 */
struct dmar_scope {
	uint8_t		type;
	uint8_t		length;
	uint16_t	reserved;
	uint8_t		enumeration_id;
	uint8_t		start_bus;
	/* path: two bytes per hop */
} __attribute__((packed));

_Static_assert(sizeof(struct dmar_scope) == 6, "a device scope header is six bytes");

#define	DMAR_SCOPE_ENDPOINT	1
#define	DMAR_SCOPE_BRIDGE	2
#define	DMAR_SCOPE_IOAPIC	3
#define	DMAR_SCOPE_HPET		4
#define	DMAR_SCOPE_NAMESPACE	5

/* ------------------------------------------------------------------ */
/*  The registers                                                       */
/* ------------------------------------------------------------------ */

#define	VTD_VER		0x000		/* 32 bits */
#define	VTD_CAP		0x008		/* 64 bits */
#define	VTD_ECAP	0x010		/* 64 bits */

/*
 * CAP, as far as stage 1 needs it.
 *
 * MGAW is one less than the widest address the engine will translate.  SAGAW
 * is a bitmask of the page-table depths it will walk, and it is the field that
 * decides what stage 3 has to build -- an engine offered a depth it does not
 * support does not translate wrongly, it refuses the root pointer, and there
 * is nothing to read afterwards.
 */
#define	VTD_CAP_SAGAW(c)	(((c) >> 8) & 0x1F)
#define	VTD_CAP_MGAW(c)		((unsigned)(((c) >> 16) & 0x3F))

/*
 * ⚠️ CAP also says where the fault-recording registers are and how many there
 * are, and stage 3 needs both -- "a blocked DMA must produce a diagnosable
 * event" starts at that offset.  They are NOT decoded here.  The whole
 * register is kept raw in the unit's vendor_caps, so nothing is lost, and a
 * field decoded three stages before anything reads it is a number that agrees
 * with itself and with nothing else.
 */

/*
 * ECAP bit 0 is page-walk coherency: whether the engine's own reads of the
 * page tables snoop the processor caches.  When it is clear, every table this
 * kernel writes has to be flushed to memory before the engine is told about
 * it, which is a cost and an ordering rule rather than an optional
 * optimisation -- so it is worth knowing three stages early.
 */
#define	VTD_ECAP_COHERENT(e)	(((e) & 0x1) != 0)
#define	VTD_ECAP_IR(e)		((((e) >> 3) & 0x1) != 0)

/*
 * The engine's version, split as the specification does.  A major version of
 * zero is not a version: it is what a read of nothing looks like once the
 * all-ones case has been taken out, and both are how a wrong base address
 * announces itself.
 */
#define	VTD_VER_MAJOR(v)	(((v) >> 4) & 0xF)

/*
 * Walk one structure's device scopes and record them.
 *
 * `from' is the first byte after the structure's own fixed part and `end' the
 * byte after the structure.  A scope whose length does not advance the cursor
 * stops the walk rather than spinning on it -- the same rule read_madt() uses,
 * and for the same reason.
 */
static int read_scopes(const uint8_t *from, const uint8_t *end,
		       uint16_t segment)
{
	while (from + sizeof(struct dmar_scope) <= end) {
		const struct dmar_scope *s = (const struct dmar_scope *)from;
		struct iommu_scope out;
		unsigned hops;

		if (s->length < sizeof(*s) || from + s->length > end)
			return 0;

		hops = (s->length - sizeof(*s)) / 2;

		out.kind = IOMMU_SCOPE_NAMESPACE;
		if (s->type == DMAR_SCOPE_ENDPOINT)
			out.kind = IOMMU_SCOPE_ENDPOINT;
		else if (s->type == DMAR_SCOPE_BRIDGE)
			out.kind = IOMMU_SCOPE_BRIDGE;
		else if (s->type == DMAR_SCOPE_IOAPIC)
			out.kind = IOMMU_SCOPE_IOAPIC;
		else if (s->type == DMAR_SCOPE_HPET)
			out.kind = IOMMU_SCOPE_HPET;

		out.enumeration_id = s->enumeration_id;
		out.segment = segment;
		out.bus = s->start_bus;
		out.depth = (uint8_t)hops;

		/*
		 * The first hop, which is the device on `start_bus'.  With one
		 * hop that IS the device; with more it is the first bridge,
		 * and depth says so.  A scope with no hops at all names the
		 * bus itself and is recorded as device zero function zero
		 * rather than being dropped, because dropping it would make
		 * the scope count disagree with the table.
		 */
		out.dev = hops ? from[sizeof(*s)] : 0;
		out.func = hops ? from[sizeof(*s) + 1] : 0;

		/* Intel names one device at a time; the range is a point. */
		out.last_bus = out.bus;
		out.last_dev = out.dev;
		out.last_func = out.func;

		iommu_record_scope(&out);

		from += s->length;
	}

	return from == end;
}

/*
 * The two capability words, turned into the four things the description holds.
 *
 * Pure, and separate from the reading, so that iommu_decode_check() can run it
 * against a captured value whose right answer is known -- see the note in
 * <cpu/iommu_backend.h>.
 */
void iommu_vtd_decode(uint64_t cap, uint64_t ecap,
		      unsigned *address_bits, uint32_t *page_levels,
		      int *interrupt_remapping, int *coherent)
{
	unsigned sagaw = VTD_CAP_SAGAW(cap);
	uint32_t levels = 0;

	/*
	 * SAGAW is a SET of supported widths, one bit each, and only three of
	 * its five bits mean anything (Rev 5.20, CAP_REG bits 12:8):
	 *
	 *	bit 0  Reserved
	 *	bit 1  39-bit AGAW, 3-level page table
	 *	bit 2  48-bit AGAW, 4-level page table
	 *	bit 3  57-bit AGAW, 5-level page table
	 *	bit 4  Reserved
	 *
	 * 🔴 THIS LOOP RAN OVER ALL FIVE, mapping bit i to 2 + i levels, which
	 * is right for the three that exist and invents a two-level table for
	 * bit 0 and a six-level one for bit 4.  It was written from memory and
	 * checked against the only value we can produce -- QEMU's 0x06, which
	 * has both reserved bits CLEAR -- so the wrong half was never once
	 * exercised.  A decode verified only where it happens to be right.
	 *
	 * ⚠️ The reserved bits are ignored rather than refused, and that is a
	 * different judgement from AMD's HATS.  HATS is one encoded value, so
	 * a reserved encoding makes the whole field unreadable; SAGAW is a set,
	 * and a bit we do not understand does not spoil the ones we do.  What
	 * it does mean is that this kernel cannot build the depth that bit is
	 * claiming, which is exactly what leaving it out of the mask says.
	 */
	for (unsigned i = 1; i <= 3; i++)
		if (sagaw & (1u << i))
			levels |= 1u << (2 + i);

	*address_bits = VTD_CAP_MGAW(cap) + 1u;
	*page_levels = levels;
	*interrupt_remapping = VTD_ECAP_IR(ecap);
	*coherent = VTD_ECAP_COHERENT(ecap);
}

/*
 * Ask one engine what it can do.
 *
 * ⚠️ The registers are mapped uncached and never unmapped.  A handful of pages
 * for the life of the machine, against a mapping that would have to be rebuilt
 * every time anything wanted to look at a fault -- and stage 3 wants exactly
 * that, continuously.
 */
static void read_hardware(unsigned index, uint64_t base, uint64_t size)
{
	volatile uint8_t *regs;
	uint32_t version;
	uint64_t cap, ecap;
	uint32_t levels = 0;
	unsigned bits = 0;
	int ir = 0, coherent = 0;

	regs = (volatile uint8_t *)(uintptr_t)pmap_map_device(base, size);
	if (regs == 0)
		return;

	version = *(volatile uint32_t *)(regs + VTD_VER);

	/*
	 * All-ones is a read of nothing at all, and a major version of zero is
	 * a read of something that is not this.  Either way the base address
	 * did not come from a correctly parsed table, and recording what the
	 * following registers say would be recording noise with a structure.
	 */
	if (version == 0xFFFFFFFFu || VTD_VER_MAJOR(version) == 0)
		return;

	cap = *(volatile uint64_t *)(regs + VTD_CAP);
	ecap = *(volatile uint64_t *)(regs + VTD_ECAP);

	iommu_vtd_decode(cap, ecap, &bits, &levels, &ir, &coherent);

	iommu_record_hardware(index, version, bits, levels, ir, coherent,
			      cap, ecap);
}

int iommu_vtd_read(void)
{
	const struct dmar_table *dmar;
	const uint8_t *p, *end;
	unsigned first_unit;
	int exact;

	dmar = (const struct dmar_table *)acpi_find_table("DMAR");
	if (dmar == 0)
		return 0;

	if (dmar->header.length < sizeof(*dmar))
		return 0;

	/*
	 * The width field is one less than the number of bits, and a zero in
	 * it would therefore mean a one-bit machine.  Read as "the table did
	 * not say" instead, which is what it means -- and reported as zero
	 * rather than as one, so that nothing downstream does arithmetic on a
	 * number nobody wrote.
	 */
	iommu_record_platform(dmar->host_address_width
			      ? dmar->host_address_width + 1u : 0u,
			      (dmar->flags & DMAR_FLAG_INTR_REMAP) != 0,
			      (dmar->flags & DMAR_FLAG_X2APIC_OPT_OUT) != 0);

	first_unit = iommu_unit_count();
	exact = 1;

	p = (const uint8_t *)dmar + sizeof(*dmar);
	end = (const uint8_t *)dmar + dmar->header.length;

	while (p + sizeof(struct dmar_entry) <= end) {
		const struct dmar_entry *e = (const struct dmar_entry *)p;

		/*
		 * A structure shorter than its own header would not advance
		 * the cursor.  Stop, and say the walk did not add up -- which
		 * is the difference between "this table has a kind of
		 * structure I do not know" and "this table is not one".
		 */
		if (e->length < sizeof(*e) || p + e->length > end) {
			exact = 0;
			break;
		}

		if (e->type == DMAR_DRHD
		    && e->length >= sizeof(struct dmar_drhd)) {
			const struct dmar_drhd *d = (const void *)p;
			uint64_t size = 4096ULL << (d->size & 0x1F);

			if (iommu_record_unit(d->segment, d->register_base,
					      size,
					      (d->flags & DRHD_INCLUDE_PCI_ALL) != 0)
			    >= 0) {
				if (!read_scopes(p + sizeof(*d), p + e->length,
						 d->segment))
					exact = 0;
			}
		} else if (e->type == DMAR_RMRR
			   && e->length >= sizeof(struct dmar_rmrr)) {
			const struct dmar_rmrr *r = (const void *)p;

			if (iommu_record_reserved(r->segment, r->base,
						  r->limit) >= 0) {
				if (!read_scopes(p + sizeof(*r), p + e->length,
						 r->segment))
					exact = 0;
			}
		}

		/*
		 * ATSR, RHSA and ANDD are stepped over rather than read.  Each
		 * matters to a later stage -- device-side TLBs to
		 * invalidation, affinity to where an engine's tables should be
		 * allocated, AML names to devices with no BDF -- and none of
		 * them changes what stage 1 can say about whether this machine
		 * could enforce isolation.  ⚠️ Stepping over them is only
		 * correct because the cursor check below notices if their
		 * lengths do not add up.
		 */

		p += e->length;
	}

	if (p != end)
		exact = 0;

	/*
	 * A DMAR with no engines in it is not a machine with an IOMMU.  Left
	 * as IOMMU_NONE rather than as a vendor with an empty list, because
	 * the two mean the same thing to everything downstream and one of them
	 * looks like a bug in this file.
	 */
	if (iommu_unit_count() == first_unit) {
		iommu_record_reset();
		return 0;
	}

	iommu_record_walk(exact);

	for (unsigned i = first_unit; i < iommu_unit_count(); i++) {
		const struct iommu_unit *u = iommu_unit(i);

		read_hardware(i, u->register_base, u->register_size);
	}

	return 1;
}
