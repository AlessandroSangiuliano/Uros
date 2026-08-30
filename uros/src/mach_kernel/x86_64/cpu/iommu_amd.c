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
#include <pmap/bootmem.h>
#include <pmap/layout.h>
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
 * ── The engine's own registers ────────────────────────────────────────
 *
 * Field positions from AMD I/O Virtualization Technology (IOMMU)
 * Specification, 48882-PUB Rev 3.11, Apr 2026 -- MMIO Offset 0030h and MMIO
 * Offset 0018h.
 *
 * 🔴 THE REVISION IS PART OF THE CITATION, AND NOT AS BOOKKEEPING.  Rev 2.62
 * (Feb 2015) calls bits 2 and 5 of this register Reserved.  Rev 3.11 calls bit
 * 2 XTSup and bit 5 GAPPISup, and the real AMD machine this was written on has
 * bit 2 SET -- so a decode written from the older document would have reported
 * "no x2APIC support" on hardware that has it, silently and forever.  A
 * specification is a moving object and a register is not.
 *
 * ⚠️ The document itself is not in this tree: it is AMD's, distributed under
 * its own agreement.  What is here is the citation and the field positions,
 * which are facts about the silicon.
 */
#define	AMD_REG_CONTROL		0x18
#define	AMD_REG_EXT_FEATURE	0x30

/* MMIO Offset 0030h, IOMMU Extended Feature Register, bits 15:0. */
#define	AMD_EFR_PREFSUP(e)	(((e) >> 0) & 1)	/* PREFETCH command   */
#define	AMD_EFR_PPRSUP(e)	(((e) >> 1) & 1)	/* peripheral page rq */
#define	AMD_EFR_XTSUP(e)	(((e) >> 2) & 1)	/* x2APIC in the IRT  */
#define	AMD_EFR_NXSUP(e)	(((e) >> 3) & 1)
#define	AMD_EFR_GTSUP(e)	(((e) >> 4) & 1)	/* guest translation  */
#define	AMD_EFR_IASUP(e)	(((e) >> 6) & 1)	/* INVALIDATE_ALL     */
#define	AMD_EFR_GASUP(e)	(((e) >> 7) & 1)	/* guest virtual APIC */
#define	AMD_EFR_HESUP(e)	(((e) >> 8) & 1)	/* hardware error regs*/
#define	AMD_EFR_PCSUP(e)	(((e) >> 9) & 1)	/* perf counters      */

/*
 * HATS, bits 11:10 -- the deepest host page table this engine will walk.
 *
 *	00b = 4 levels   01b = 5 levels   10b = 6 levels   11b = Reserved
 *
 * 🔑 It is a MAXIMUM and not a fixed depth: DTE[Mode] selects 1 through 6 per
 * device, and 000b means translation disabled altogether.  Which is why stage
 * 2's identity domain needs no page table on this vendor at all -- the
 * passthrough is a mode, not an identity mapping somebody has to build.
 *
 * ⚠️ The reserved encoding is REFUSED rather than clamped.  A newer engine
 * that reports 11b means something this file has not read about, and answering
 * "4 levels" to that would be inventing the safest-looking number.
 */
#define	AMD_EFR_HATS(e)		(((e) >> 10) & 3)
#define	AMD_EFR_GATS(e)		(((e) >> 12) & 3)	/* same encoding, guest */

/*
 * MMIO Offset 0018h[Coherent], bit 10.
 *
 * ⚠️ NOT the same kind of thing as Intel's ECAP[C], and the description holds
 * both in one field, so the difference is stated here rather than lost.  Intel
 * reports a CAPABILITY -- whether the engine's page walks snoop the caches.
 * AMD's is a CONTROL, read/write, reset to 1, and it covers the engine's reads
 * of the DEVICE TABLE.  So a zero here does not mean the hardware cannot
 * snoop; it means somebody turned it off, and the tables this kernel writes
 * would have to be flushed before the engine is told about them.
 */
#define	AMD_CONTROL_COHERENT(c)	(((c) >> 10) & 1)

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

/*
 * The feature register, turned into the four things the description holds.
 *
 * Pure, and separate from the reading, so it can be checked without an AMD
 * machine -- see <cpu/iommu_backend.h>.
 */
void iommu_amd_decode(uint64_t efr, uint64_t control,
		      unsigned *address_bits, uint32_t *page_levels,
		      int *interrupt_remapping, int *coherent)
{
	unsigned hats = AMD_EFR_HATS(efr);
	unsigned deepest;

	/*
	 * 11b is reserved.  Refused, which the caller reads as "the register
	 * said something this could not read" -- a width of zero is not a
	 * width.
	 */
	if (hats == 3) {
		*address_bits = 0;
		*page_levels = 0;
		*interrupt_remapping = 0;
		*coherent = 0;
		return;
	}

	deepest = 4 + hats;			/* 00b->4, 01b->5, 10b->6 */

	/*
	 * Every depth up to the maximum, because DTE[Mode] chooses one per
	 * device and any value from 1 to the limit is legal.  Intel's SAGAW
	 * names a set instead of a ceiling, which is why the description holds
	 * a bitmask and not a number: the two vendors answer different
	 * questions and the mask is what both can say.
	 */
	*page_levels = 0;
	for (unsigned l = 1; l <= deepest; l++)
		*page_levels |= 1u << l;

	/*
	 * The device virtual address space each depth reaches, from the
	 * DTE[Mode] table: four levels is 48 bits, five is 57, six is 64.
	 */
	*address_bits = deepest == 6 ? 64u : (deepest == 5 ? 57u : 48u);

	/*
	 * 🔑 Always, on this vendor, and it is not an assumption.  An AMD-Vi
	 * engine has an interrupt remapping table architecturally -- every DTE
	 * carries an interrupt table root pointer -- which is why the register
	 * has no bit for "is there one" and has one for whether that table can
	 * hold x2APIC destinations.  XTSup is the refinement, not the switch.
	 */
	*interrupt_remapping = 1;

	*coherent = AMD_CONTROL_COHERENT(control);
}

/*
 * ── The device table entry, which is stage 2's first structure ────────
 *
 * 32 bytes per device id, from Table 7 of Rev 3.11.  Only the fields a
 * pass-through or a blocked entry needs are named; the rest stay zero and are
 * reserved or belong to features this kernel does not use yet.
 *
 * 🔴 A ZEROED DEVICE TABLE IS NOT A CLOSED DOOR.  V=0 means "all addresses are
 * forwarded without translation; individual control fields are ignored" -- so
 * an entry nobody filled in is a device with unrestricted access to memory,
 * which is precisely today's behaviour and precisely what stage 3 exists to
 * end.  Blocking takes MORE bits than allowing: V=1, TV=1, and both permission
 * bits CLEAR.  Getting that backwards produces a table that looks configured
 * and enforces nothing.
 */
#define	AMD_DTE_V		(1ULL << 0)	/* the entry is valid       */
#define	AMD_DTE_TV		(1ULL << 1)	/* ... and so is its mode   */
#define	AMD_DTE_MODE_SHIFT	9		/* 000b = translation off   */
#define	AMD_DTE_IR		(1ULL << 61)	/* reads permitted          */
#define	AMD_DTE_IW		(1ULL << 62)	/* writes permitted         */
/* DomainID is bits 79:64, which is the low sixteen bits of the second word. */

/*
 * Pass-through: the device reaches memory exactly as it does today, but
 * through an entry the kernel wrote and can change.
 *
 * 🔑 Mode 000b with V and TV SET, rather than the simpler V=0.  Both forward
 * the address untranslated; only this one goes through the permission bits and
 * carries a domain id, so it is the entry stage 3 narrows rather than a
 * different entry stage 3 would have to replace.  Behaviourally identical to
 * today and structurally one step from the answer, which is what #432's stage
 * 2 asks for.
 */
void iommu_amd_dte_passthrough(uint16_t domain, uint64_t out[4])
{
	out[0] = AMD_DTE_V | AMD_DTE_TV | (0ULL << AMD_DTE_MODE_SHIFT)
	       | AMD_DTE_IR | AMD_DTE_IW;
	out[1] = domain;
	out[2] = 0;
	out[3] = 0;
}

/*
 * Blocked: the device is described, and allowed nothing.
 *
 * ⚠️ Not the same as an empty entry, and that is the whole point of having
 * this beside the one above.
 */
void iommu_amd_dte_blocked(uint16_t domain, uint64_t out[4])
{
	out[0] = AMD_DTE_V | AMD_DTE_TV | (0ULL << AMD_DTE_MODE_SHIFT);
	out[1] = domain;
	out[2] = 0;
	out[3] = 0;
}

/*
 * ── Stage 2a: the device table, built and read back ──────────────────
 *
 * One entry per 16-bit device id: 65536 entries of 32 bytes, two megabytes,
 * which is the whole of what the Device Table Base register can describe and
 * the whole of what this machine's own IVRS needs -- its device entries name
 * ranges reaching ff:1f.7.
 *
 * 🔴 EVERY ENTRY IS WRITTEN, and the allocator's zeroing is not allowed to
 * mean anything.  An all-zero entry has V=0, which forwards without
 * translation; a table that was merely allocated is a table that permits
 * everything, and it would look identical to one that had been configured.
 */
#define	AMD_DEVICE_IDS		65536u
#define	AMD_DTE_WORDS		4u		/* 32 bytes */
#define	AMD_DEVICE_TABLE_FRAMES	((AMD_DEVICE_IDS * AMD_DTE_WORDS * 8u) / 4096u)

_Static_assert(AMD_DEVICE_TABLE_FRAMES == 512,
	       "the device table is two megabytes, which is its architectural maximum");

int iommu_amd_build(void)
{
	uint64_t table, command, event;
	volatile uint64_t *dt;
	uint64_t want[AMD_DTE_WORDS];
	unsigned written = 0;

	table = boot_frames_alloc(AMD_DEVICE_TABLE_FRAMES);
	if (table == 0)
		return 0;

	/*
	 * The command buffer and the event log, one frame each, which is the
	 * smallest either may be.  Allocated here rather than in stage 2b
	 * because an engine cannot be enabled without them and finding that
	 * out with translation half on is not a discovery anyone wants.
	 */
	command = boot_frame_alloc();
	event = boot_frame_alloc();
	if (command == 0 || event == 0)
		return 0;

	dt = (volatile uint64_t *)(uintptr_t)phys_to_direct(table);
	iommu_amd_dte_passthrough(IOMMU_DOMAIN_PASSTHROUGH, want);

	for (unsigned i = 0; i < AMD_DEVICE_IDS; i++) {
		dt[i * AMD_DTE_WORDS + 0] = want[0];
		dt[i * AMD_DTE_WORDS + 1] = want[1];
		dt[i * AMD_DTE_WORDS + 2] = want[2];
		dt[i * AMD_DTE_WORDS + 3] = want[3];
		written++;
	}

	/*
	 * ⚠️ ALL of them read back, not a sample.  A sample is exactly the
	 * check that misses the one entry a loop got wrong, and these are
	 * written once and read only by hardware -- there is no later moment
	 * when a wrong one produces a wrong answer instead of an unpoliced
	 * device.
	 */
	for (unsigned i = 0; i < AMD_DEVICE_IDS; i++)
		if (dt[i * AMD_DTE_WORDS + 0] != want[0]
		    || dt[i * AMD_DTE_WORDS + 1] != want[1]
		    || dt[i * AMD_DTE_WORDS + 2] != want[2]
		    || dt[i * AMD_DTE_WORDS + 3] != want[3])
			return 0;

	iommu_record_tables(table, (uint64_t)AMD_DEVICE_TABLE_FRAMES * 4096u,
			    command, event, written, 0,
			    AMD_DEVICE_TABLE_FRAMES + 2u);
	return 1;
}

/*
 * ── Stage 2b: point the engine at the table and let it run ───────────
 *
 * Rev 3.11, MMIO Offsets 0000h, 0008h, 0010h and 0018h.  The bases carry the
 * physical address in bits 51:12 and their own length beside it: the device
 * table as (n+1) 4-Kbyte pages in bits 8:0, the two buffers as a power of two
 * in bits 59:56, where 1000b is the 256-entry minimum and anything smaller is
 * reserved.
 */
#define	AMD_REG_DEVTAB		0x00
#define	AMD_REG_CMDBUF		0x08
#define	AMD_REG_EVTLOG		0x10
#define	AMD_REG_CMDBUF_HEAD	0x2000
#define	AMD_REG_CMDBUF_TAIL	0x2008

#define	AMD_BASE_MASK		0x000FFFFFFFFFF000ULL
#define	AMD_BUFLEN_256		(8ULL << 56)	/* 4 Kbytes, the minimum */

#define	AMD_CTL_IOMMU_EN	(1ULL << 0)
#define	AMD_CTL_EVENTLOG_EN	(1ULL << 2)
#define	AMD_CTL_CMDBUF_EN	(1ULL << 12)

int iommu_amd_enable(void)
{
	const struct iommu_tables *t = iommu_tables();
	unsigned enabled = 0;

	for (unsigned i = 0; i < iommu_unit_count(); i++) {
		const struct iommu_unit *u = iommu_unit(i);
		volatile uint8_t *regs;
		uint64_t control;

		if (!u->answered || u->register_va == 0)
			return 0;

		regs = (volatile uint8_t *)(uintptr_t)u->register_va;

		/*
		 * 🔴 OFF FIRST, and not as ceremony.  Firmware may have left
		 * this engine translating for its own devices, with its own
		 * device table and its own caches -- and pointing a running
		 * engine at a new table is asking it to use one description
		 * while remembering another.  On QEMU it is always off; on the
		 * machine this was written on the firmware runs it.
		 */
		control = *(volatile uint64_t *)(regs + AMD_REG_CONTROL);
		control &= ~AMD_CTL_IOMMU_EN;
		*(volatile uint64_t *)(regs + AMD_REG_CONTROL) = control;

		*(volatile uint64_t *)(regs + AMD_REG_DEVTAB) =
			(t->root & AMD_BASE_MASK)
			| ((t->root_bytes / 4096u) - 1u);

		*(volatile uint64_t *)(regs + AMD_REG_CMDBUF) =
			(t->command & AMD_BASE_MASK) | AMD_BUFLEN_256;
		*(volatile uint64_t *)(regs + AMD_REG_EVTLOG) =
			(t->event & AMD_BASE_MASK) | AMD_BUFLEN_256;

		/*
		 * The ring pointers, written rather than trusted.  They reset
		 * to zero and writing the base is documented to reset them,
		 * but "documented to reset" and "observed to be zero" are two
		 * different statements and only one of them costs two stores.
		 */
		*(volatile uint64_t *)(regs + AMD_REG_CMDBUF_HEAD) = 0;
		*(volatile uint64_t *)(regs + AMD_REG_CMDBUF_TAIL) = 0;

		/*
		 * ⚠️ The buffers BEFORE the engine.  An engine enabled without
		 * an event log has nowhere to report the first thing that goes
		 * wrong, which is precisely the moment one wants it -- and
		 * #432 asks for a blocked DMA to be diagnosable rather than
		 * silent.
		 */
		control |= AMD_CTL_CMDBUF_EN | AMD_CTL_EVENTLOG_EN;
		*(volatile uint64_t *)(regs + AMD_REG_CONTROL) = control;

		control |= AMD_CTL_IOMMU_EN;
		*(volatile uint64_t *)(regs + AMD_REG_CONTROL) = control;

		/*
		 * Read back, because a write that was accepted and ignored is
		 * the failure this cannot afford to call success.
		 */
		control = *(volatile uint64_t *)(regs + AMD_REG_CONTROL);
		if (!(control & AMD_CTL_IOMMU_EN))
			return 0;

		enabled++;
	}

	return enabled > 0;
}

/*
 * ── Stage 3: the page table itself ───────────────────────────────────
 *
 * Tables 17 and 18 of Rev 3.11.  Present in bit 0, the address in 51:12,
 * read and write permission in 61 and 62 -- and, in bits 11:9, the thing that
 * makes this NOT the processor's page table wearing different names:
 *
 *	Next Level.  Every entry says what LEVEL the table it points at is,
 *	so a directory may point two levels down and the walker skips the
 *	steps between.  000b and 111b mean the entry is a translation rather
 *	than a directory.
 *
 * 🔑 Intel's second-stage entries carry no such field: there the level is the
 * depth you reached it at.  So a builder that treated the two as one format
 * with different bit positions would produce an AMD table whose every
 * directory claimed to point at a translation.
 *
 * ⚠️ Permission is ANDed down the walk, and skipped levels count as ones.  A
 * directory with IR and IW clear therefore blocks its whole subtree, which is
 * how a range is denied without unmapping it -- and a directory built without
 * them set permits nothing, which is the mistake that looks like a table that
 * simply does not work.
 */
#define	AMD_PT_PR		(1ULL << 0)
#define	AMD_PT_NEXT_SHIFT	9
#define	AMD_PT_ADDR_MASK	0x000FFFFFFFFFF000ULL
#define	AMD_PT_IR		(1ULL << 61)
#define	AMD_PT_IW		(1ULL << 62)

#define	AMD_PT_NEXT_TRANSLATION	0ULL	/* 000b: this entry IS the mapping */

/* One 4-Kbyte page, with the permissions the caller asks for. */
uint64_t iommu_amd_pte(uint64_t pa, int read, int write)
{
	return AMD_PT_PR
	     | (pa & AMD_PT_ADDR_MASK)
	     | (AMD_PT_NEXT_TRANSLATION << AMD_PT_NEXT_SHIFT)
	     | (read ? AMD_PT_IR : 0)
	     | (write ? AMD_PT_IW : 0);
}

/*
 * A directory pointing at a table of level `next_level'.
 *
 * ⚠️ Always permissive.  The permissions that matter are the leaf's and the
 * DTE's; a directory that withheld them would silently narrow everything below
 * it, and the place to deny a range is the range, not the road to it.
 */
uint64_t iommu_amd_pde(uint64_t next_table_pa, unsigned next_level)
{
	return AMD_PT_PR
	     | (next_table_pa & AMD_PT_ADDR_MASK)
	     | ((uint64_t)(next_level & 7u) << AMD_PT_NEXT_SHIFT)
	     | AMD_PT_IR | AMD_PT_IW;
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
	unsigned	bits = 0;
	uint32_t	levels = 0;
	int		ir = 0, coherent = 0;

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
	 *
	 * 🔴 512 KBYTES, NOT ONE PAGE.  This mapped 0x1000 and stage 2b then
	 * faulted on the command buffer head at offset 0x2000 -- the first
	 * write past the page, in a boot that had reported everything about
	 * this engine correctly.  An IVHD gives a base and no extent, and the
	 * extent is not in the table at all: the specification puts it at
	 * 16 Kbytes, or 512 Kbytes when the engine reports performance
	 * counters, which is a bit of the feature register that cannot be read
	 * until the registers are mapped.
	 *
	 * ⚠️ So the architectural maximum is mapped rather than the conditional
	 * size.  It costs 128 pages of kernel address space per engine and
	 * removes a bootstrap problem entirely -- and reading device space the
	 * engine does not decode returns all-ones, which is a wrong answer
	 * only to a question nothing here asks.
	 */
	regs = (volatile uint8_t *)(uintptr_t)pmap_map_device(h->base, 0x80000);
	if (regs == 0)
		return;

	control = *(volatile uint64_t *)(regs + AMD_REG_CONTROL);
	features = *(volatile uint64_t *)(regs + AMD_REG_EXT_FEATURE);

	iommu_amd_decode(features, control, &bits, &levels, &ir, &coherent);

	/*
	 * ⚠️ Version zero, because there is no version register to read.  The
	 * engine's identity was established by the capability comparison
	 * above, which is a stronger statement than a version number would
	 * have been -- and putting a made-up number here so the field looks
	 * filled is exactly what that comparison exists instead of.
	 */
	iommu_record_hardware(index, 0, bits, levels, ir, coherent,
			      features, control);
	iommu_record_registers(index, (uint64_t)(uintptr_t)regs);
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
	 * ⚠️ -1 for the interrupt-remapping claim, meaning THE TABLE DOES NOT
	 * SAY, and no x2apic objection because the IVRS makes none.  AMD's
	 * table states which wide-id modes are SUPPORTED rather than asking
	 * that they be avoided, so answering "no" to either would be this
	 * reader asserting something no firmware wrote -- and the first version
	 * did exactly that, which made the cross-check against the engines
	 * report a disagreement on every AMD boot.
	 */
	iommu_record_platform(IVINFO_PA_SIZE(ivrs->iv_info), -1, 0);

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
