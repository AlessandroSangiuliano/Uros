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
#include <cpu/pci_cfg.h>
#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/pmap.h>

#include <device/pci.h>		/* PCI_VENDOR_ID */

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
 * ── The root and context entries, which are stage 2's first structures ─
 *
 * Rev 5.20, Figures 9-1 and 9-3.  A root entry per bus points at a 4KB context
 * table; a context entry per device/function says how that device's requests
 * are translated.  Both are 128 bits, low word first.
 *
 * 🔴 AND THE ZERO ENTRY MEANS THE OPPOSITE OF AMD'S.  Here P=0 is NOT PRESENT
 * and every request through it is blocked and faulted; there V=0 is "forwarded
 * without translation".  An empty table is a closed door on this vendor and an
 * open one on the other.  Two structures that look alike, initialised the same
 * way, with opposite consequences -- and the dangerous direction is the one
 * that looks configured and enforces nothing.
 */
#define	VTD_ROOT_PRESENT	(1ULL << 0)
#define	VTD_CTX_PRESENT		(1ULL << 0)
#define	VTD_CTX_TT_SHIFT	2		/* 10b = pass-through */
#define	VTD_CTX_TT_PASSTHROUGH	2ULL
#define	VTD_CTX_TT_SECOND_STAGE	0ULL		/* 00b = walk SSPTPTR  */
#define	VTD_CTX_SSPTPTR_MASK	0xFFFFFFFFFFFFF000ULL
/* AW is bits 66:64 and DID bits 87:72: the low word of the second half. */
#define	VTD_CTX_AW_SHIFT	0
#define	VTD_CTX_DID_SHIFT	8

void iommu_vtd_root_entry(uint64_t context_table_pa, uint64_t out[2])
{
	out[0] = (context_table_pa & ~0xFFFULL) | VTD_ROOT_PRESENT;
	out[1] = 0;
}

/*
 * Pass-through for one device.
 *
 * ⚠️ `levels' is the DEEPEST the engine supports, not a choice.  The
 * specification requires AW to name the largest AGAW the hardware reports when
 * the type is pass-through, and the encoding is AGAW value = levels - 2, which
 * happens to be the bit position that width occupies in SAGAW.
 *
 * 🔴 The caller must have checked ECAP[PT] first: pass-through is RESERVED on
 * hardware that does not report it, which means an engine without it will fault
 * on an entry that looks perfectly formed.  AMD has no such condition -- its
 * translation-disabled mode is always available -- so this is one more place
 * the two vendors do not mirror each other.
 */
void iommu_vtd_context_passthrough(uint16_t domain, unsigned levels,
				   uint64_t out[2])
{
	out[0] = VTD_CTX_PRESENT
	       | (VTD_CTX_TT_PASSTHROUGH << VTD_CTX_TT_SHIFT);
	out[1] = ((uint64_t)(levels - 2) & 7ULL) << VTD_CTX_AW_SHIFT
	       | ((uint64_t)domain << VTD_CTX_DID_SHIFT);
}

/*
 * ── Stage 3c: one device, translating through a table of its own ─────
 *
 * Translation type 00b, which Rev 5.20 Table 21 defines as: "Untranslated
 * requests are translated using second-stage paging structures referenced
 * through the SSPTPTR field.  Translated requests and Translation Requests are
 * blocked."
 *
 * 🔑 THE SECOND SENTENCE IS HALF THE POINT.  A device with its own Device-TLB
 * can present an address it says is already translated, and under 01b the
 * engine would take its word for it -- so a driver that can talk to such a
 * device could hand the hardware a physical address again and this whole issue
 * would be back where it started.  00b refuses those, and it is chosen for
 * that and not merely because 01b needs Device-TLB support.
 *
 * ⚠️ `levels' is this domain's depth and not the engine's deepest.  Under
 * pass-through AW must name the largest the hardware reports; here it must name
 * the table actually being pointed at, and an address above what it covers is
 * blocked as a translation fault.  Same field, two different rules, and the
 * pass-through encoder above is not a template for this one.
 */
void iommu_vtd_context_domain(uint16_t domain, unsigned levels,
			      uint64_t root_pa, uint64_t out[2])
{
	out[0] = VTD_CTX_PRESENT
	       | (VTD_CTX_TT_SECOND_STAGE << VTD_CTX_TT_SHIFT)
	       | (root_pa & VTD_CTX_SSPTPTR_MASK);
	out[1] = ((uint64_t)(levels - 2) & 7ULL) << VTD_CTX_AW_SHIFT
	       | ((uint64_t)domain << VTD_CTX_DID_SHIFT);
}

/*
 * Blocked: not present, which on this vendor is all it takes.
 *
 * ⚠️ NOT "present with an empty page table".  A second-stage pointer of zero
 * is a pointer to physical page zero, and the engine would walk it as a page
 * table -- reading whatever is there as translations.  Blocking by leaving a
 * pointer null is how a table that blocks nothing gets written.
 */
void iommu_vtd_context_blocked(uint64_t out[2])
{
	out[0] = 0;
	out[1] = 0;
}

/*
 * ── Stage 2a: the root and context tables, built and read back ───────
 *
 * A root table of 256 entries, one per bus, each pointing at a context table
 * of 256 entries, one per device/function.  Both are 4KB and both are indexed
 * by a byte, so neither needs a size field.
 *
 * ⚠️ A context table is allocated only for a bus that has a device on it.  The
 * alternative is 256 of them, a megabyte, nearly all describing buses that do
 * not exist -- and an absent bus needs no table because its root entry stays
 * NOT PRESENT, which on this vendor already means blocked.  🔑 That is the
 * asymmetry paying off in the direction that costs memory rather than safety:
 * the same shortcut on AMD would leave devices forwarding untranslated.
 */
#define	VTD_BUSES		256u
#define	VTD_DEVFNS		256u
#define	VTD_ENTRY_WORDS		2u		/* 128 bits */
#define	VTD_ECAP_PT(e)		((((e) >> 6) & 0x1) != 0)

/* The deepest page table the engines all support, as a level count. */
static unsigned deepest_level(void)
{
	uint32_t common = 0xFFFFFFFFu;
	unsigned deepest = 0;

	for (unsigned i = 0; i < iommu_unit_count(); i++)
		common &= iommu_unit(i)->page_levels;

	for (unsigned l = 0; l < 32; l++)
		if (common & (1u << l))
			deepest = l;

	return deepest;
}

int iommu_vtd_build(void)
{
	uint64_t root_pa;
	volatile uint64_t *root;
	unsigned levels = deepest_level();
	unsigned contexts = 0, devices = 0, frames = 1;

	if (levels == 0)
		return 0;

	/*
	 * 🔴 Pass-through is RESERVED on hardware that does not report it, and
	 * an entry using it there faults on everything while looking perfectly
	 * formed.  Asked of every engine, not the first: a machine whose
	 * engines disagree has no single answer, and building for the ones
	 * that can would leave the others faulting.
	 */
	for (unsigned i = 0; i < iommu_unit_count(); i++)
		if (!iommu_unit(i)->answered
		    || !VTD_ECAP_PT(iommu_unit(i)->vendor_caps[1]))
			return 0;

	root_pa = boot_frame_alloc();
	if (root_pa == 0)
		return 0;

	root = (volatile uint64_t *)(uintptr_t)phys_to_direct(root_pa);

	for (unsigned bus = 0; bus < VTD_BUSES; bus++) {
		uint64_t ctx_pa;
		volatile uint64_t *ctx;
		uint64_t entry[VTD_ENTRY_WORDS];
		int present = 0;

		for (unsigned d = 0; d < 32 && !present; d++)
			for (unsigned f = 0; f < 8; f++)
				if (pci_cfg_read(0, (uint8_t)bus, (uint8_t)d,
						 (uint8_t)f, PCI_VENDOR_ID)
				    != 0xFFFFFFFFu) {
					present = 1;
					break;
				}

		if (!present) {
			/*
			 * Left not present, which blocks.  Written rather than
			 * assumed: see the note above about zeroed frames.
			 */
			root[bus * VTD_ENTRY_WORDS + 0] = 0;
			root[bus * VTD_ENTRY_WORDS + 1] = 0;
			continue;
		}

		ctx_pa = boot_frame_alloc();
		if (ctx_pa == 0)
			return 0;
		frames++;

		ctx = (volatile uint64_t *)(uintptr_t)phys_to_direct(ctx_pa);
		iommu_vtd_context_passthrough(IOMMU_DOMAIN_PASSTHROUGH, levels,
					      entry);

		for (unsigned i = 0; i < VTD_DEVFNS; i++) {
			ctx[i * VTD_ENTRY_WORDS + 0] = entry[0];
			ctx[i * VTD_ENTRY_WORDS + 1] = entry[1];
			devices++;
		}

		for (unsigned i = 0; i < VTD_DEVFNS; i++)
			if (ctx[i * VTD_ENTRY_WORDS + 0] != entry[0]
			    || ctx[i * VTD_ENTRY_WORDS + 1] != entry[1])
				return 0;

		iommu_vtd_root_entry(ctx_pa, entry);
		root[bus * VTD_ENTRY_WORDS + 0] = entry[0];
		root[bus * VTD_ENTRY_WORDS + 1] = entry[1];
		contexts++;
	}

	iommu_record_tables(root_pa, 4096, 0, 0, devices, contexts, frames);
	return 1;
}

/*
 * ── Stage 2b: set the root pointer, invalidate, enable ───────────────
 *
 * Rev 5.20.  GCMD's bits are WRITE-ONLY and the register takes one command at
 * a time, so each write must carry the state of everything already on --
 * which is read out of GSTS, whose bits sit at the same positions.  🔑 A
 * read-modify-write of GCMD itself would read zeros and turn off whatever was
 * running.
 */
#define	VTD_GCMD	0x18
#define	VTD_GSTS	0x1C
#define	VTD_RTADDR	0x20
#define	VTD_CCMD	0x28

#define	VTD_GCMD_TE	(1ULL << 31)	/* translation enable            */
#define	VTD_GCMD_SRTP	(1ULL << 30)	/* set root table pointer        */
#define	VTD_GSTS_TES	(1ULL << 31)
#define	VTD_GSTS_RTPS	(1ULL << 30)

/* The bits of GSTS that describe state worth carrying into the next GCMD. */
#define	VTD_GSTS_KEEP	0x96FFFFFFu

#define	VTD_CCMD_ICC	(1ULL << 63)	/* invalidate context cache      */
#define	VTD_CCMD_GLOBAL	(1ULL << 61)	/* ... globally                  */
#define	VTD_IOTLB_IVT	(1ULL << 63)
#define	VTD_IOTLB_GLOBAL (1ULL << 60)
#define	VTD_ECAP_IRO(e)	((unsigned)((((e) >> 8) & 0x3FF) * 16))

/*
 * Spin until a bit settles, bounded.
 *
 * ⚠️ Bounded because this runs before any timer and an engine that never
 * answers would hang the boot with nothing on the screen -- which is the one
 * outcome worse than a failure, since it cannot be reported.
 */
static int wait_bit(volatile uint8_t *regs, unsigned off, uint64_t bit,
		    int want, int wide)
{
	for (unsigned spin = 0; spin < 1000000u; spin++) {
		uint64_t v = wide ? *(volatile uint64_t *)(regs + off)
				  : *(volatile uint32_t *)(regs + off);

		if (((v & bit) != 0) == (want != 0))
			return 1;
	}

	return 0;
}

int iommu_vtd_enable(void)
{
	const struct iommu_tables *t = iommu_tables();
	unsigned enabled = 0;

	for (unsigned i = 0; i < iommu_unit_count(); i++) {
		const struct iommu_unit *u = iommu_unit(i);
		volatile uint8_t *regs;
		uint32_t keep;
		unsigned iro;

		if (!u->answered || u->register_va == 0)
			return 0;

		regs = (volatile uint8_t *)(uintptr_t)u->register_va;

		/*
		 * The root table, in legacy mode: TTM is bits 11:10 and zero
		 * is what says so.  Written before SRTP, which is what makes
		 * the engine read it.
		 */
		*(volatile uint64_t *)(regs + VTD_RTADDR) = t->root;

		keep = *(volatile uint32_t *)(regs + VTD_GSTS) & VTD_GSTS_KEEP;
		*(volatile uint32_t *)(regs + VTD_GCMD) =
			keep | (uint32_t)VTD_GCMD_SRTP;
		if (!wait_bit(regs, VTD_GSTS, VTD_GSTS_RTPS, 1, 0))
			return 0;

		/*
		 * 🔴 INVALIDATE BEFORE ENABLING, both caches, globally.  The
		 * engine may hold entries from whoever ran it before us --
		 * firmware, or a previous boot that left it on -- and a
		 * translation cached against a table we have replaced is a
		 * device reaching memory by an old description.  Costs two
		 * writes and two spins, once.
		 */
		*(volatile uint64_t *)(regs + VTD_CCMD) =
			VTD_CCMD_ICC | VTD_CCMD_GLOBAL;
		if (!wait_bit(regs, VTD_CCMD, VTD_CCMD_ICC, 0, 1))
			return 0;

		iro = VTD_ECAP_IRO(u->vendor_caps[1]);
		*(volatile uint64_t *)(regs + iro + 8) =
			VTD_IOTLB_IVT | VTD_IOTLB_GLOBAL;
		if (!wait_bit(regs, iro + 8, VTD_IOTLB_IVT, 0, 1))
			return 0;

		keep = *(volatile uint32_t *)(regs + VTD_GSTS) & VTD_GSTS_KEEP;
		*(volatile uint32_t *)(regs + VTD_GCMD) =
			keep | (uint32_t)VTD_GCMD_TE;
		if (!wait_bit(regs, VTD_GSTS, VTD_GSTS_TES, 1, 0))
			return 0;

		enabled++;
	}

	return enabled > 0;
}

/*
 * ── Stage 3: the second-stage paging entries ─────────────────────────
 *
 * Rev 5.20, section 9.8.  Read in bit 0, write in bit 1, the address in
 * 51:12, and nothing that says which level this is -- here the level is the
 * depth the walker reached the entry at, which is the ordinary arrangement and
 * the opposite of AMD's.
 *
 * 🔑 The two formats agree on almost every position and disagree about what a
 * table IS, which is the shape of difference that gets flattened by whoever
 * writes the second one from the first.  <cpu/iommu_backend.h> keeps them as
 * two encoders for that reason and not for symmetry.
 *
 * ⚠️ Permission is ANDed down the walk here too, so a directory needs read and
 * write set for anything below it to be reachable.
 */
#define	VTD_SS_R		(1ULL << 0)
#define	VTD_SS_W		(1ULL << 1)
#define	VTD_SS_ADDR_MASK	0x000FFFFFFFFFF000ULL

uint64_t iommu_vtd_ss_pte(uint64_t pa, int read, int write)
{
	return (pa & VTD_SS_ADDR_MASK)
	     | (read ? VTD_SS_R : 0)
	     | (write ? VTD_SS_W : 0);
}

/*
 * A directory entry.  ⚠️ Bit 7 (page size) stays CLEAR: set, it would make
 * this a large-page translation and the address a page frame rather than a
 * table -- the same bits meaning something else entirely, with no complaint
 * from anything until a device read the wrong memory.
 */
uint64_t iommu_vtd_ss_pde(uint64_t next_table_pa)
{
	return (next_table_pa & VTD_SS_ADDR_MASK) | VTD_SS_R | VTD_SS_W;
}

/*
 * ── Stage 3b: the same entry, read the way the engine reads it ───────
 *
 * 🔴 THERE IS NO PRESENT BIT.  Rev 5.20 §3.7: "If a second-stage
 * paging-structure entry's read and write permissions are both 0 or if the
 * entry sets any reserved field, the entry is used neither to reference another
 * paging-structure entry nor to map a page."
 *
 * So permission and presence are ONE question here and two on AMD, and the
 * state "mapped, and every access refused" -- which an AMD entry says with PR=1
 * and no IR or IW -- cannot be written down in this format at all.  A decoder
 * carried across from the other vendor would read such an entry as a mapping
 * with no permissions and hand back a physical address for an input the
 * hardware faults on.
 */
#define	VTD_SS_PS		(1ULL << 7)

int iommu_vtd_pt_decode(uint64_t entry, unsigned level,
			struct iommu_pt_step *step)
{
	int read = (entry & VTD_SS_R) != 0;
	int write = (entry & VTD_SS_W) != 0;

	if (!read && !write)
		return 0;

	step->next = entry & VTD_SS_ADDR_MASK;
	step->read = read;
	step->write = write;

	/*
	 * At the bottom there is nowhere further to go, and Table 47 makes bit
	 * 7 Ignored there -- so it is not consulted rather than required clear.
	 */
	if (level == 1) {
		step->level = 0;
		return 1;
	}

	if ((entry & VTD_SS_PS) != 0) {
		/*
		 * ⚠️ A large page, but only where one can exist.  §3.7 reserves
		 * the page-size field in an SS-PML4E and an SS-PML5E, and a
		 * reserved bit set makes the entry unusable rather than
		 * generous -- so above level 3 this is a refusal and not a
		 * 512-Gbyte page.
		 */
		if (level > 3)
			return 0;

		/*
		 * ⚠️ And §3.7 reserves the address bits below the page it
		 * claims to be -- "if the R or W fields of an SS-PDE is 1, and
		 * the PS field in that SS-PDE is 1, bits 20:12 are reserved".
		 * So a large page at an address that is not that size aligned
		 * is refused, not rounded down.
		 */
		if ((step->next & (iommu_level_span(level) - 1ULL)) != 0)
			return 0;

		step->level = 0;
		return 1;
	}

	/* No field says where to go next: the level is the depth. */
	step->level = level - 1;
	return 1;
}

/*
 * The ways an Intel entry can be wrong.  See <cpu/iommu_backend.h>.
 */
int iommu_vtd_pt_ablate(unsigned kind, unsigned level, uint64_t *entry)
{
	switch (kind) {
	case IOMMU_ABLATE_ROAD_IS_DESTINATION:
		/*
		 * The page-size bit set on a directory: the table it points at
		 * becomes the page the device reaches.  Same outcome as AMD's
		 * cleared Next Level, arrived at from the opposite direction --
		 * there a field is forgotten, here one is added.
		 *
		 * ⚠️ Aligned down for the same reason as AMD's: a misaligned
		 * large page is refused as a reserved-field violation, and the
		 * mistake worth catching is the one that stays well-formed.
		 */
		*entry |= VTD_SS_PS;
		*entry &= ~(iommu_level_span(level) - 1ULL) | 0xFFFULL;
		return 1;

	case IOMMU_ABLATE_DENY_ON_THE_ROAD:
		*entry &= ~VTD_SS_W;
		return 1;
	}

	/*
	 * 🔑 IOMMU_ABLATE_SKIP_A_LEVEL lands here, and answering no is the
	 * point: this format has no field with which to skip a level, so it has
	 * no way to skip one wrongly.  Said here, where the format is, rather
	 * than by the check leaving one case out silently.
	 */
	return 0;
}

/*
 * 🔑 And no legal skip either, for the same reason: a second-stage entry
 * carries no field naming the level below it, so every walk here descends
 * exactly one level per step.  What AMD spends three bits on, this format gets
 * from the depth -- and what it gets for free it also cannot vary.
 */
int iommu_vtd_pt_skip(uint64_t next_table_pa, unsigned next_level,
		      uint64_t *entry)
{
	(void)next_table_pa;
	(void)next_level;
	(void)entry;
	return 0;
}

/*
 * ── Stage 3d: moving one device into a domain, live ──────────────────
 *
 * The root table stays where stage 2 put it and so do the context tables; what
 * changes is ONE context entry, from the pass-through pair to the pair that
 * names a second-stage root.  Then the engine is told to forget the device.
 *
 * 🔴 CONTEXT CACHE FIRST, IOTLB SECOND, AND NOT THE OTHER WAY.  The context
 * entry is what says which page table to walk; the IOTLB holds translations
 * produced by walking the OLD one.  Invalidating the IOTLB first leaves a
 * window in which the engine can re-walk through the stale context entry and
 * repopulate it -- small, real, and impossible to see afterwards, because what
 * it produces is a device that still reaches its old memory and a set of
 * tables that say it should not.
 *
 * ⚠️ Global, and not device-selective, which this engine also offers.  An
 * attach happens once per device, at its first grant, and a global
 * invalidation costs two register writes and two bounded spins -- against a
 * device-selective one whose SID and DID fields are two more chances to be
 * wrong in a way that produces a correct-looking machine.  The moment attach
 * is on a path that runs often, this is the thing to sharpen.
 */
int iommu_vtd_attach(uint16_t bdf, const struct iommu_domain *d)
{
	const struct iommu_tables *t = iommu_tables();
	unsigned bus = (unsigned)(bdf >> 8);
	unsigned devfn = (unsigned)(bdf & 0xFF);
	volatile uint64_t *root, *ctx;
	uint64_t ctx_pa, entry[VTD_ENTRY_WORDS];
	unsigned attached = 0;

	if (t == 0 || t->root == 0 || d == 0 || d->root == 0)
		return 0;

	root = (volatile uint64_t *)(uintptr_t)phys_to_direct(t->root);

	/*
	 * ⚠️ A bus stage 2 found nothing on has no context table, and a device
	 * that turned up afterwards cannot be attached rather than being
	 * attached into a table that is not there.  Reported by answering no:
	 * the caller is granting memory, and a grant that silently did not
	 * take effect is the failure this whole issue exists to remove.
	 */
	if ((root[bus * VTD_ENTRY_WORDS + 0] & VTD_ROOT_PRESENT) == 0)
		return 0;

	ctx_pa = root[bus * VTD_ENTRY_WORDS + 0] & VTD_CTX_SSPTPTR_MASK;
	ctx = (volatile uint64_t *)(uintptr_t)phys_to_direct(ctx_pa);

	iommu_vtd_context_domain(d->id, d->levels, d->root, entry);

	/*
	 * 🔴 THE HIGH WORD BEFORE THE LOW ONE, because the low one carries
	 * Present.  Written the other way round, an engine that read the entry
	 * between the two stores would find it present, pointing at this
	 * domain's root, with a domain id and address width that are still the
	 * pass-through entry's -- a valid-looking entry nobody wrote.
	 */
	ctx[devfn * VTD_ENTRY_WORDS + 1] = entry[1];
	ctx[devfn * VTD_ENTRY_WORDS + 0] = entry[0];

	if (ctx[devfn * VTD_ENTRY_WORDS + 0] != entry[0]
	    || ctx[devfn * VTD_ENTRY_WORDS + 1] != entry[1])
		return 0;

	for (unsigned i = 0; i < iommu_unit_count(); i++) {
		const struct iommu_unit *u = iommu_unit(i);
		volatile uint8_t *regs;
		unsigned iro;

		if (u == 0 || !u->answered || u->register_va == 0)
			return 0;

		regs = (volatile uint8_t *)(uintptr_t)u->register_va;

		*(volatile uint64_t *)(regs + VTD_CCMD) =
			VTD_CCMD_ICC | VTD_CCMD_GLOBAL;
		if (!wait_bit(regs, VTD_CCMD, VTD_CCMD_ICC, 0, 1))
			return 0;

		iro = VTD_ECAP_IRO(u->vendor_caps[1]);
		*(volatile uint64_t *)(regs + iro + 8) =
			VTD_IOTLB_IVT | VTD_IOTLB_GLOBAL;
		if (!wait_bit(regs, iro + 8, VTD_IOTLB_IVT, 0, 1))
			return 0;

		attached++;
	}

	return attached > 0;
}

/*
 * A mapping changed inside a domain that is already attached.
 *
 * 🔴 AND IT IS NEEDED EVEN WHEN THE CHANGE IS NOT-PRESENT TO PRESENT.  Rev
 * 5.20 §6.1: when CAP.CM is Set, hardware caches non-present entries too, and
 * software must invalidate after making one present.  QEMU's engine can report
 * CM either way and a physical one usually reports it clear -- so a kernel
 * that skipped this would work on the machine it was written on and fail on
 * the machine it was tested on, or the other way round.  One invalidation per
 * grant, on a path that runs once per buffer.
 *
 * ⚠️ Global, though the domain is named and this engine offers a
 * domain-selective form -- IOTLB_REG's IIRG field with the DID beside it.  The
 * argument is the one on attach above: a field that is not written cannot be
 * written wrongly, and until grants are frequent the difference is two spins.
 */
int iommu_vtd_flush(const struct iommu_domain *d)
{
	unsigned flushed = 0;

	if (d == 0)
		return 0;

	for (unsigned i = 0; i < iommu_unit_count(); i++) {
		const struct iommu_unit *u = iommu_unit(i);
		volatile uint8_t *regs;
		unsigned iro;

		if (u == 0 || !u->answered || u->register_va == 0)
			return 0;

		regs = (volatile uint8_t *)(uintptr_t)u->register_va;
		iro = VTD_ECAP_IRO(u->vendor_caps[1]);

		*(volatile uint64_t *)(regs + iro + 8) =
			VTD_IOTLB_IVT | VTD_IOTLB_GLOBAL;
		if (!wait_bit(regs, iro + 8, VTD_IOTLB_IVT, 0, 1))
			return 0;

		flushed++;
	}

	return flushed > 0;
}

/*
 * ── Stage 3d: the fault recording registers ──────────────────────────
 *
 * Rev 5.20 §11.4.7.6, Figure 11-15.  A fault record is 128 bits, and there are
 * CAP.NFR+1 of them starting at CAP.FRO -- which is an offset in SIXTEEN-BYTE
 * units and not in bytes, the same shape as ECAP.IRO above and the same trap.
 *
 *	127	F	this record holds a fault
 *	126	T1	type, high bit
 *	103:96	FR	fault reason
 *	92	T2	type, low bit
 *	79:64	SID	the requester: bus:dev.func
 *	63:12	FI	the address that was refused, page aligned
 *
 * 🔴 THE HIGH HALF IS READ FIRST, AND THE SPECIFICATION SAYS SO.  §11.4.7.6
 * note 1: "Hardware updates to this register may be disassembled as multiple
 * doubleword writes", and software must see F set before believing the rest.
 * A reader that took the low half first can read an address the engine has not
 * finished writing -- and it would be right almost every time, which is the
 * worst frequency for a defect to have.
 *
 * ⚠️ {T1,T2} is TWO bits in this revision and was one in earlier ones: 00b
 * write, 01b page request, 10b read, 11b atomic.  A reader that kept the old
 * single-bit T would call every AtomicOp a read -- a wrong answer that looks
 * entirely reasonable in a log.
 */
#define	VTD_FSTS		0x34
#define	VTD_FSTS_PFO		(1u << 0)	/* records were dropped   */
#define	VTD_FSTS_PPF		(1u << 1)	/* at least one is set    */

#define	VTD_CAP_NFR(c)		((unsigned)((((c) >> 40) & 0xFF) + 1u))
#define	VTD_CAP_FRO(c)		((unsigned)((((c) >> 24) & 0x3FF) * 16u))

#define	VTD_FR_F		(1ULL << 63)
#define	VTD_FR_T1		(1ULL << 62)
#define	VTD_FR_T2		(1ULL << 28)
#define	VTD_FR_REASON(h)	((uint8_t)(((h) >> 32) & 0xFF))
#define	VTD_FR_SID(h)		((uint16_t)((h) & 0xFFFF))
#define	VTD_FR_ADDRESS(l)	((l) & ~0xFFFULL)

/*
 * Rev 5.20 Table 30, the conditions and the codes they are reported with.
 *
 * ⚠️ Only the ones this kernel can cause are read into a kind; everything else
 * stays IOMMU_FAULT_UNKNOWN with its raw code intact.  A table that mapped
 * every code to something would be inventing a reading for codes that arrive
 * from features we do not use, and the raw number is the thing a specification
 * can be looked up against.
 *
 * 🔴 Ah AND Ch ARE NOT THE SAME MISTAKE, and this file said they were until
 * the table was read: a non-zero reserved field in a ROOT entry is Ah (LRT.3)
 * and one in a second-stage PAGING entry is Ch (LSS.2).  Two different halves
 * of the machine got wrong, reported one bit apart.
 */
#define	VTD_FR_ROOT_NOT_PRESENT		0x01	/* LRT.2                    */
#define	VTD_FR_CONTEXT_NOT_PRESENT	0x02	/* LCT.2                    */
#define	VTD_FR_CONTEXT_INVALID		0x03	/* LCT.4: AW, TT, SSPTPTR   */
#define	VTD_FR_ADDRESS_TOO_WIDE		0x04	/* LGN.1                    */
#define	VTD_FR_WRITE_DENIED		0x05	/* LGN.2: walked, said no   */
#define	VTD_FR_READ_DENIED		0x06	/* LGN.3                    */
#define	VTD_FR_PAGE_TABLE_UNREADABLE	0x07	/* LSS.1: engine's own read */
#define	VTD_FR_ROOT_UNREADABLE		0x08	/* LRT.1                    */
#define	VTD_FR_CONTEXT_UNREADABLE	0x09	/* LCT.1                    */
#define	VTD_FR_ROOT_RESERVED		0x0A	/* LRT.3                    */
#define	VTD_FR_CONTEXT_RESERVED		0x0B	/* LCT.3                    */
#define	VTD_FR_PAGE_ENTRY_RESERVED	0x0C	/* LSS.2                    */
#define	VTD_FR_TRANSLATION_BLOCKED	0x0D	/* LCT.5                    */

static uint8_t vtd_fault_kind(uint8_t reason)
{
	switch (reason) {
	case VTD_FR_WRITE_DENIED:
	case VTD_FR_READ_DENIED:
	case VTD_FR_PAGE_ENTRY_RESERVED:
	case VTD_FR_ADDRESS_TOO_WIDE:
		return IOMMU_FAULT_PAGE;

	case VTD_FR_ROOT_NOT_PRESENT:
	case VTD_FR_CONTEXT_NOT_PRESENT:
	case VTD_FR_CONTEXT_INVALID:
	case VTD_FR_ROOT_RESERVED:
	case VTD_FR_CONTEXT_RESERVED:
	case VTD_FR_TRANSLATION_BLOCKED:
		return IOMMU_FAULT_ENTRY;

	case VTD_FR_PAGE_TABLE_UNREADABLE:
	case VTD_FR_ROOT_UNREADABLE:
	case VTD_FR_CONTEXT_UNREADABLE:
		return IOMMU_FAULT_HARDWARE;

	default:
		return IOMMU_FAULT_UNKNOWN;
	}
}

int iommu_vtd_fault_decode(uint64_t lo, uint64_t hi, struct iommu_fault *out)
{
	unsigned type;

	if (!(hi & VTD_FR_F))
		return 0;

	type = (unsigned)(((hi & VTD_FR_T1) ? 2u : 0u)
			  | ((hi & VTD_FR_T2) ? 1u : 0u));

	out->address = VTD_FR_ADDRESS(lo);
	out->source = VTD_FR_SID(hi);
	out->domain = IOMMU_FAULT_NO_DOMAIN;
	out->reason = VTD_FR_REASON(hi);
	out->kind = vtd_fault_kind(out->reason);
	out->write = type == 0;
	out->vendor = IOMMU_INTEL;
	return 1;
}

unsigned iommu_vtd_fault_drain(unsigned unit, int *overflowed)
{
	const struct iommu_unit *u = iommu_unit(unit);
	volatile uint8_t *regs;
	unsigned records, base, found = 0;
	uint32_t status;

	if (u == 0 || !u->answered || u->register_va == 0)
		return 0;

	regs = (volatile uint8_t *)(uintptr_t)u->register_va;
	records = VTD_CAP_NFR(u->vendor_caps[0]);
	base = VTD_CAP_FRO(u->vendor_caps[0]);

	status = *(volatile uint32_t *)(regs + VTD_FSTS);
	if (status & VTD_FSTS_PFO) {
		if (overflowed)
			*overflowed = 1;

		/*
		 * 🔴 CLEARED, or the engine records nothing further.  §11.4.7.1
		 * PFO: "When this field is Set, hardware does not record any
		 * new faults until software clears this field" -- so a reader
		 * that only reported the overflow would turn one lost fault
		 * into every subsequent one.
		 */
		*(volatile uint32_t *)(regs + VTD_FSTS) = VTD_FSTS_PFO;
	}

	/*
	 * PPF is the OR of every record's F, so a clear one means there is
	 * nothing to walk -- one uncached read instead of NFR+1 of them, on
	 * the path that runs on every poll and finds nothing almost always.
	 */
	if (!(status & VTD_FSTS_PPF))
		return 0;

	/*
	 * ⚠️ Every record, and not only the one FSTS.FRI names.  That field
	 * says where the FIRST pending fault was recorded, which is where to
	 * start if one wants them in order -- it is not a count, and a reader
	 * that treated it as one would drain a single record per poll and
	 * leave the rest to overflow.
	 */
	for (unsigned i = 0; i < records; i++) {
		volatile uint64_t *rec =
			(volatile uint64_t *)(regs + base + i * 16u);
		uint64_t hi = rec[1];
		struct iommu_fault f;

		if (!(hi & VTD_FR_F))
			continue;

		if (!iommu_vtd_fault_decode(rec[0], hi, &f))
			continue;

		iommu_record_fault(&f);
		found++;

		/*
		 * F is RW1CS, and this is the only write.  ⚠️ PPF is NOT
		 * written: §11.4.7.1 makes it read-only and computes it as the
		 * OR of the F fields, so it goes away when these do -- writing
		 * a one into a read-only bit is a store that reads as tidy and
		 * means nothing.
		 */
		rec[1] = VTD_FR_F;
	}

	return found;
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
	iommu_record_registers(index, (uint64_t)(uintptr_t)regs);
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
