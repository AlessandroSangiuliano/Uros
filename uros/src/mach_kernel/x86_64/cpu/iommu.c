/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What this machine has to police DMA with (#432, stage 1).
 *
 * The storage and the front door.  The readers that fill this in live beside
 * it, one per vendor; iommu_discover() tries each in turn, in an order that
 * does not matter, because a machine has one or the other and never both.
 */

#include <stdint.h>

#include <cpu/acpi.h>
#include <cpu/iommu_backend.h>
#include <kern/misc_protos.h>	/* printf, for the refusals (#432 stage 3d) */
#include <pmap/layout.h>
#include <pmap/pmap.h>		/* pmap_table_frame (#458) */

/*
 * Caps, and the flag that is what makes them safe.
 *
 * Static because this is read once at boot and never grows, and because a list
 * of engines is not the kind of thing that wants an allocator behind it.  The
 * numbers are generous for the hardware that exists -- a large two-socket
 * machine has one unit per socket plus one for the graphics -- and a machine
 * that exceeds them is REPORTED rather than either truncated or panicked over.
 *
 * 🔑 The choice not to panic is the substantive one.  This stage only looks at
 * the machine, and a kernel that stopped booting because it ran out of room to
 * DESCRIBE something would have turned a reporting feature into a regression.
 * The cost of not panicking is that something downstream must check
 * iommu_truncated(), which is why that function exists and says so.
 */
#define	IOMMU_MAX_UNITS		8
#define	IOMMU_MAX_RESERVED	16
#define	IOMMU_MAX_SCOPES	64

static enum iommu_vendor	found_vendor;
static int			discovered;
static int			truncated;
static int			walk_exact;
static int			both_tables;

static unsigned			platform_address_bits;
static int			platform_interrupt_remapping;
static int			x2apic_discouraged;

static struct iommu_unit	units[IOMMU_MAX_UNITS];
static unsigned			nunits;

static struct iommu_reserved	reserved[IOMMU_MAX_RESERVED];
static unsigned			nreserved;

static struct iommu_scope	scopes[IOMMU_MAX_SCOPES];
static unsigned			nscopes;

/*
 * Which of the two lists the last recorded thing was in, so that a scope knows
 * where to attach.  Nothing outside this file can see it, which is the point:
 * see the comment on iommu_record_scope() in the backend header.
 */
#define	LAST_NOTHING	0
#define	LAST_UNIT	1
#define	LAST_RESERVED	2

static int last_kind;
static unsigned last_index;

int iommu_record_unit(uint16_t segment, uint64_t base, uint64_t size,
		      int covers_rest)
{
	struct iommu_unit *u;

	if (nunits >= IOMMU_MAX_UNITS) {
		truncated = 1;
		last_kind = LAST_NOTHING;
		return -1;
	}

	u = &units[nunits];
	u->segment = segment;
	u->register_base = base;
	u->register_size = size;
	u->covers_rest = covers_rest;
	u->scope_first = nscopes;
	u->scope_count = 0;

	last_kind = LAST_UNIT;
	last_index = nunits;
	nunits++;
	return (int)last_index;
}

int iommu_record_reserved(uint16_t segment, uint64_t base, uint64_t limit)
{
	struct iommu_reserved *r;

	if (nreserved >= IOMMU_MAX_RESERVED) {
		truncated = 1;
		last_kind = LAST_NOTHING;
		return -1;
	}

	r = &reserved[nreserved];
	r->segment = segment;
	r->base = base;
	r->limit = limit;
	r->scope_first = nscopes;
	r->scope_count = 0;

	last_kind = LAST_RESERVED;
	last_index = nreserved;
	nreserved++;
	return (int)last_index;
}

void iommu_record_scope(const struct iommu_scope *scope)
{
	/*
	 * A scope with nothing to attach to is dropped and counted as
	 * truncation, which is the honest description: the table said
	 * something this description does not hold.  It happens when the unit
	 * that owned it did not fit.
	 */
	if (last_kind == LAST_NOTHING || nscopes >= IOMMU_MAX_SCOPES) {
		truncated = 1;
		return;
	}

	scopes[nscopes] = *scope;
	nscopes++;

	if (last_kind == LAST_UNIT)
		units[last_index].scope_count++;
	else
		reserved[last_index].scope_count++;
}

void iommu_record_hardware(unsigned index, uint32_t version,
			   unsigned address_bits, uint32_t page_levels,
			   int interrupt_remapping, int coherent_walk,
			   uint64_t caps0, uint64_t caps1)
{
	struct iommu_unit *u;

	if (index >= nunits)
		return;

	u = &units[index];
	u->answered = 1;
	u->version = version;
	u->address_bits = address_bits;
	u->page_levels = page_levels;
	u->interrupt_remapping = interrupt_remapping;
	u->coherent_walk = coherent_walk;
	u->vendor_caps[0] = caps0;
	u->vendor_caps[1] = caps1;
}

void iommu_record_vendor(enum iommu_vendor vendor)
{
	found_vendor = vendor;
}

void iommu_record_walk(int exact)
{
	walk_exact = exact;
}

void iommu_record_platform(unsigned address_bits, int interrupt_remapping,
			   int x2apic_off)
{
	platform_address_bits = address_bits;
	platform_interrupt_remapping = interrupt_remapping;
	x2apic_discouraged = x2apic_off;
}

void iommu_record_reset(void)
{
	nunits = 0;
	nreserved = 0;
	nscopes = 0;
	last_kind = LAST_NOTHING;
	truncated = 0;
	walk_exact = 0;
	platform_address_bits = 0;
	platform_interrupt_remapping = 0;
	x2apic_discouraged = 0;
	found_vendor = IOMMU_NONE;
}

enum iommu_vendor iommu_discover(void)
{
	/*
	 * Once.  The units' registers are mapped by the readers, and the
	 * device region is a bump allocator that never gives anything back --
	 * so a second walk would not be wrong, it would be a slow leak with a
	 * correct answer, which is the shape of thing that is never noticed.
	 */
	if (discovered)
		return found_vendor;

	discovered = 1;

	/*
	 * ⚠️ The first reader to claim the machine ends it, on the assumption
	 * that a machine has one vendor's tables or the other's and never
	 * both.  That assumption is sound on conforming firmware and it was
	 * SILENT: a machine describing both would have been read as Intel with
	 * nothing said, because the second reader is never asked.
	 *
	 * So the other table is looked for anyway, and found is a FINDING.
	 * The description does not try to hold two vendors -- that would be a
	 * shape invented for a machine nobody has -- it holds the one it read
	 * and says the other was there.
	 *
	 * 🔑 It costs one walk of the ACPI root table, once, in the branch
	 * where a vendor was found, and nothing at all afterwards: this
	 * function runs once at boot and the guard above is what makes that
	 * true.  An assumption converted into an observation for the price of
	 * twenty signature comparisons at boot.
	 *
	 * 🔴 AND THE TRUE BRANCH HAS NEVER RUN.  The lookup is exercised on
	 * every boot with an engine -- it answers no on the Intel machine and
	 * no on the AMD one, which is the arithmetic working -- but no machine
	 * reachable from here can make it answer yes.  QEMU declines:
	 *
	 *	-device amd-iommu: QEMU does not support multiple vIOMMUs
	 *	for x86 yet.
	 *
	 * ⚠️ So its silence is not evidence that no such machine exists.  It
	 * is a report that has been wired up and never fired, which is a
	 * different thing from a report that has fired and said no -- and
	 * saying which is the whole reason this paragraph is here.
	 */
	if (iommu_vtd_read()) {
		iommu_record_vendor(IOMMU_INTEL);
		both_tables = acpi_find_table("IVRS") != 0;
	} else if (iommu_amd_read()) {
		iommu_record_vendor(IOMMU_AMD);
		both_tables = acpi_find_table("DMAR") != 0;
	} else {
		iommu_record_vendor(IOMMU_NONE);
	}

	return found_vendor;
}

int iommu_both_tables(void)
{
	return both_tables;
}

enum iommu_vendor iommu_vendor(void)
{
	return found_vendor;
}

unsigned iommu_unit_count(void)
{
	return nunits;
}

const struct iommu_unit *iommu_unit(unsigned index)
{
	return index < nunits ? &units[index] : 0;
}

unsigned iommu_reserved_count(void)
{
	return nreserved;
}

const struct iommu_reserved *iommu_reserved(unsigned index)
{
	return index < nreserved ? &reserved[index] : 0;
}

const struct iommu_scope *iommu_scope(unsigned index)
{
	return index < nscopes ? &scopes[index] : 0;
}

int iommu_truncated(void)
{
	return truncated;
}

int iommu_walk_exact(void)
{
	return walk_exact;
}

/* ------------------------------------------------------------------ */
/*  Stage 2a: the tables, built and read back, hardware untouched       */
/* ------------------------------------------------------------------ */

static struct iommu_tables built;

void iommu_record_tables(uint64_t root, uint64_t root_bytes,
			 uint64_t command, uint64_t event,
			 unsigned devices, unsigned contexts, unsigned frames)
{
	built.root = root;
	built.root_bytes = root_bytes;
	built.command = command;
	built.event = event;
	built.devices = devices;
	built.contexts = contexts;
	built.frames = frames;
	built.verified = 1;
}

static int translating;

void iommu_record_registers(unsigned index, uint64_t va)
{
	if (index < nunits)
		units[index].register_va = va;
}

int iommu_translating(void)
{
	return translating;
}

/*
 * ⚠️ The tables must exist first, and this checks rather than assumes.  An
 * engine pointed at a table that was never built is pointed at whatever the
 * allocator would have returned, which is the one mistake in this whole issue
 * that cannot be reported afterwards -- the machine simply stops.
 */
int iommu_enable_passthrough(void)
{
	if (translating)
		return 1;
	if (!built.verified || built.root == 0)
		return 0;

	if (found_vendor == IOMMU_INTEL)
		translating = iommu_vtd_enable();
	else if (found_vendor == IOMMU_AMD)
		translating = iommu_amd_enable();

	return translating;
}

int iommu_build_passthrough(void)
{
	/*
	 * Once, and only where there is something to build for.  A machine
	 * with no engine gets no tables -- two megabytes for hardware that
	 * does not exist would be a cost paid by every machine to describe
	 * what none of them has.
	 */
	if (built.verified)
		return 1;
	if (found_vendor == IOMMU_NONE)
		return 0;

	if (found_vendor == IOMMU_INTEL)
		return iommu_vtd_build();

	return iommu_amd_build();
}

const struct iommu_tables *iommu_tables(void)
{
	return &built;
}

unsigned iommu_platform_address_bits(void)
{
	return platform_address_bits;
}

int iommu_platform_interrupt_remapping(void)
{
	return platform_interrupt_remapping;
}

int iommu_x2apic_discouraged(void)
{
	return x2apic_discouraged;
}

/*
 * The minimum over the units, and not the first one's.
 *
 * ⚠️ A unit whose registers did not answer is not skipped -- it makes the
 * answer no.  The question being asked is "may an address this wide be handed
 * to any device on this machine", and an engine we could not read is an engine
 * we cannot promise anything about.  Skipping it would turn a unit we failed
 * to reach into a unit that agrees with us.
 */
/*
 * ── The decode, against answers that were established elsewhere ───────
 *
 * 🔑 A DECODE THAT ONLY RUNS ON THE MACHINE IT WAS WRITTEN FOR IS A DECODE
 * NOBODY CAN CONTRADICT.  These are capability words captured from real
 * hardware and from QEMU, with the answers they must give -- so the arithmetic
 * is checked on every boot, on every board, including the boards that have no
 * remapping hardware at all and could otherwise say nothing about it.
 *
 * The first case is the one that makes this more than a self-consistency test.
 * It is the extended feature register of a physical AMD IOMMU, read out of
 * /sys/class/iommu/ivhd0/amd-iommu/features on the machine this was developed
 * on -- and the operating system that read it published its own decode beside
 * it, naming PPR X2APIC NX GT IA GA PC.  Two implementations, one register,
 * and the bits those names land on are the bits this file uses.
 *
 * ⚠️ Field positions come from the SPECIFICATION, not from that agreement --
 * 48882-PUB Rev 3.11.  The agreement is what caught the revision being wrong:
 * Rev 2.62 calls bit 2 Reserved where this hardware has XTSup set.
 *
 * ⚠️ The synthetic cases are marked as such.  Both real AMD parts report the
 * same HATS, so the encoding's other three values have nothing behind them but
 * the document, and a table that hid that behind two real-looking rows would
 * be claiming evidence it does not have.
 */
static int entries_agree(void);		/* defined below, beside the scopes */

struct decode_case {
	const char	*what;
	int		 amd;
	uint64_t	 a;		/* EFR, or Intel's CAP  */
	uint64_t	 b;		/* Control, or ECAP     */
	unsigned	 bits;
	uint32_t	 levels;
	int		 ir;
	int		 coherent;
};

/* Levels 1..N, which is what AMD's HATS means: a ceiling, not a set. */
#define	UPTO(n)		((uint32_t)(((1u << ((n) + 1)) - 1) & ~1u))

static const struct decode_case decode_cases[] = {
	/*
	 * A physical AMD IOMMU: Zen, ivhd0, EFR 0x206d73ef22254ade.
	 * HATS is 10b, so six levels and a 64-bit device address space.
	 * ⚠️ The control word is the architectural reset value and was not
	 * read from that machine -- only the feature register was.
	 */
	{ "amd, real silicon", 1, 0x206d73ef22254adeULL, 1ULL << 10,
	  64, UPTO(6), 1, 1 },

	/* QEMU's amd-iommu, which our own boot reads as 0x29d3. */
	{ "amd, qemu", 1, 0x29d3ULL, 1ULL << 10, 64, UPTO(6), 1, 1 },

	/* Synthetic: the three HATS encodings no part we can reach reports. */
	{ "amd, hats=4 levels (synthetic)", 1, 0x0ULL, 1ULL << 10,
	  48, UPTO(4), 1, 1 },
	{ "amd, hats=5 levels (synthetic)", 1, 1ULL << 10, 1ULL << 10,
	  57, UPTO(5), 1, 1 },
	/*
	 * Reserved, and the answer must be a refusal.  A decode that clamped
	 * this to the smallest plausible width would be inventing the
	 * safest-looking number for an engine it does not understand.
	 */
	{ "amd, hats reserved (synthetic)", 1, 3ULL << 10, 1ULL << 10,
	  0, 0, 0, 0 },

	/*
	 * QEMU's intel-iommu on q35, as our own boot reads it.  SAGAW 0x06 is
	 * levels three and four -- a SET and not a ceiling, which is the
	 * difference the description's bitmask exists to hold.
	 */
	{ "intel, qemu q35", 0, 0x80d2008c222f0606ULL, 0x0000000000f00f4aULL,
	  48, (1u << 3) | (1u << 4), 1, 0 },

	/*
	 * 🔴 THE CASE THAT CAUGHT A DEFECT, and the reason synthetic cases
	 * earn their place.  SAGAW with every bit set: only bits 1, 2 and 3
	 * mean anything, so the answer is three-, four- and five-level and
	 * nothing else.  The decode used to run over all five bits and would
	 * have claimed a two-level and a six-level table here -- while
	 * agreeing perfectly with the only real value we can produce, whose
	 * reserved bits are clear.
	 */
	{ "intel, sagaw all bits set (synthetic)", 0,
	  (0x1FULL << 8) | (47ULL << 16), 0x9ULL,
	  48, (1u << 3) | (1u << 4) | (1u << 5), 1, 1 },

	/* And 57-bit, five levels, which nothing we can run reports. */
	{ "intel, 5-level 57-bit (synthetic)", 0,
	  (0x8ULL << 8) | (56ULL << 16), 0x8ULL,
	  57, 1u << 5, 1, 0 },

	/* Coherency is a bit, and a test that never sees it clear is not one. */
	{ "amd, coherent turned off (synthetic)", 1, 0x29d3ULL, 0,
	  64, UPTO(6), 1, 0 },
};

int iommu_decode_check(unsigned *ran, unsigned *wrong)
{
	unsigned n = 0, bad = 0;

	for (unsigned i = 0;
	     i < sizeof(decode_cases) / sizeof(decode_cases[0]); i++) {
		const struct decode_case *c = &decode_cases[i];
		unsigned bits = 0;
		uint32_t levels = 0;
		int ir = 0, coherent = 0;

		if (c->amd)
			iommu_amd_decode(c->a, c->b, &bits, &levels,
					 &ir, &coherent);
		else
			iommu_vtd_decode(c->a, c->b, &bits, &levels,
					 &ir, &coherent);

		n++;
		if (bits != c->bits || levels != c->levels
		    || ir != c->ir || coherent != c->coherent)
			bad++;
	}

	/*
	 * The entry encoders travel with the decoders: both are the kernel's
	 * reading of the same documents, and a boot that checks one and not
	 * the other has verified the half that only reports.
	 */
	n++;
	if (!entries_agree())
		bad++;

	if (ran)
		*ran = n;
	if (wrong)
		*wrong = bad;

	return bad == 0;
}

/*
 * The entries stage 2 will write, against the bit patterns the specifications
 * define.  Same discipline as the decode check and for a sharper reason: an
 * entry is written once and read only by hardware, so a wrong bit does not
 * produce a wrong answer -- it produces a device that is not policed, or one
 * that faults on everything, and neither says which bit was wrong.
 *
 * 🔴 The first two cases are the asymmetry, asserted rather than remembered:
 * a zeroed Intel context entry BLOCKS and a zeroed AMD device table entry
 * FORWARDS.  If either of those ever stops being true, this says so before
 * anything is allocated.
 */
static int entries_agree(void)
{
	uint64_t d[4], c[2];
	int ok = 1;

	/* AMD: valid, translation mode zero, both permissions, domain 1. */
	iommu_amd_dte_passthrough(1, d);
	ok &= d[0] == 0x6000000000000003ULL && d[1] == 1 && d[2] == 0
	   && d[3] == 0;

	/* Blocked is the same entry with the permission bits gone. */
	iommu_amd_dte_blocked(1, d);
	ok &= d[0] == 0x0000000000000003ULL && d[1] == 1;

	/*
	 * 🔴 And an AMD entry of all zeros is NOT blocked: V=0 forwards.  The
	 * blocked encoder above must therefore differ from zero, or a table
	 * that was merely allocated would be a table that permits everything.
	 */
	ok &= d[0] != 0;

	/* Intel: present, pass-through, four levels means AW 010b, domain 1. */
	iommu_vtd_context_passthrough(1, 4, c);
	ok &= c[0] == 0x9ULL && c[1] == 0x102ULL;

	/* 🔴 And here zero IS blocked, which is the opposite of above. */
	iommu_vtd_context_blocked(c);
	ok &= c[0] == 0 && c[1] == 0;

	/* Five levels is AW 011b, and nothing else moves. */
	iommu_vtd_context_passthrough(1, 5, c);
	ok &= c[0] == 0x9ULL && c[1] == 0x103ULL;

	/* A root entry keeps the pointer's page and sets present. */
	iommu_vtd_root_entry(0x123456000ULL, c);
	ok &= c[0] == 0x123456001ULL && c[1] == 0;

	/*
	 * ── The page-table entries (stage 3) ─────────────────────────
	 *
	 * 🔴 The AMD cases assert the Next Level field, which is the field
	 * Intel does not have and the one a reader coming from Intel would
	 * leave at zero.  Zero is legal there and means "this entry is the
	 * translation" -- so a directory built without it would be read as a
	 * mapping of the table it was supposed to point at, and the device
	 * would reach the page table instead of the page.  Nothing would
	 * complain.
	 */
	ok &= iommu_amd_pte(0x1000, 1, 1) == 0x6000000000001001ULL;
	ok &= iommu_amd_pte(0x1000, 1, 0) == 0x2000000000001001ULL;
	ok &= iommu_amd_pte(0x1000, 0, 0) == 0x0000000000001001ULL;

	/* A directory at level two: 010b in bits 11:9, which is 0x400. */
	ok &= iommu_amd_pde(0x2000, 2) == 0x6000000000002401ULL;

	/* ⚠️ And a directory must NOT come out looking like a translation. */
	ok &= (iommu_amd_pde(0x2000, 2) & (7ULL << 9)) != 0;

	/* Intel's, where the level is the depth and no field says it. */
	ok &= iommu_vtd_ss_pte(0x1000, 1, 1) == 0x1003ULL;
	ok &= iommu_vtd_ss_pte(0x1000, 1, 0) == 0x1001ULL;
	ok &= iommu_vtd_ss_pte(0x1000, 0, 0) == 0x1000ULL;
	ok &= iommu_vtd_ss_pde(0x2000) == 0x2003ULL;

	/*
	 * ⚠️ Bit 7 clear in a directory.  Set, it is a large-page translation
	 * and the address becomes a page frame -- the same bits meaning
	 * something else, with nothing to complain until a device read the
	 * wrong memory.
	 */
	ok &= (iommu_vtd_ss_pde(0x2000) & (1ULL << 7)) == 0;

	/*
	 * ── Reading them back (stage 3b) ─────────────────────────────
	 *
	 * 🔴 THE DECODERS AGAINST LITERALS, NEVER AGAINST THE ENCODERS ABOVE.
	 * The builder and the walker share these decoders, so checking one
	 * against the other would let both halves hold the same wrong bit and
	 * call the result verified.  Every word below was copied out of a
	 * figure, and several are words no encoder here can produce.
	 */
	{
		struct iommu_pt_step s;

		/* AMD Figure 9: PR, IR, IW, Next Level 000b -- a 4-Kbyte page. */
		ok &= iommu_amd_pt_decode(0x6000000000001001ULL, 1, &s)
		   && s.next == 0x1000ULL && s.level == 0 && s.read && s.write;

		/* Figure 8: PR clear is not present, whatever else is set. */
		ok &= !iommu_amd_pt_decode(0xFFFFFFFFFFFFFFFEULL, 1, &s);

		/* Figure 10: Next Level 010b, found in a level-3 table. */
		ok &= iommu_amd_pt_decode(0x6000000000002401ULL, 3, &s)
		   && s.next == 0x2000ULL && s.level == 2;

		/*
		 * 🔴 The SAME word one level down is a fault, not a directory:
		 * Rev 3.11 §2.2.3 makes a Next Level at or above the table's
		 * own level an IO_PAGE_FAULT.  One word, two answers, and the
		 * level is the only thing that told them apart.
		 */
		ok &= !iommu_amd_pt_decode(0x6000000000002401ULL, 2, &s);

		/* 111b is the self-sized page: refused rather than guessed at. */
		ok &= !iommu_amd_pt_decode(0x6000000000001E01ULL, 3, &s);

		/*
		 * 🔴 AMD says "mapped, and refused" -- PR set, no permissions.
		 * Intel's identical-looking word says "not mapped".  The two
		 * lines below are the same idea in the two formats, and they
		 * disagree about whether there is a mapping at all.
		 */
		ok &= iommu_amd_pt_decode(0x0000000000001001ULL, 1, &s)
		   && s.level == 0 && !s.read && !s.write;
		ok &= !iommu_vtd_pt_decode(0x0000000000001000ULL, 1, &s);

		/* Rev 5.20 Table 47: read in bit 0, write in bit 1. */
		ok &= iommu_vtd_pt_decode(0x1003ULL, 1, &s)
		   && s.next == 0x1000ULL && s.level == 0 && s.read && s.write;
		ok &= iommu_vtd_pt_decode(0x1002ULL, 1, &s)
		   && !s.read && s.write;

		/* Table 46: bit 7 clear, so the level is the depth. */
		ok &= iommu_vtd_pt_decode(0x2003ULL, 3, &s) && s.level == 2;

		/* Table 45: bit 7 set at level 2 is a 2-Mbyte page. */
		ok &= iommu_vtd_pt_decode(0x200083ULL, 2, &s)
		   && s.next == 0x200000ULL && s.level == 0;

		/*
		 * ⚠️ And the same bit at level 4, where §3.7 reserves it, makes
		 * the entry unusable rather than enormous.
		 */
		ok &= !iommu_vtd_pt_decode(0x200083ULL, 4, &s);

		/*
		 * 🔴 A large page whose address is not that size aligned is
		 * REFUSED, on both vendors, and not quietly rounded down.  Both
		 * specifications make those low address bits reserved, and a
		 * reader that rounded would hand back an address the hardware
		 * would have faulted on.
		 */
		ok &= !iommu_vtd_pt_decode(0x2083ULL, 2, &s);
		ok &= !iommu_amd_pt_decode(0x6000000000002001ULL, 2, &s);
		ok &= iommu_amd_pt_decode(0x6000000000200001ULL, 2, &s)
		   && s.next == 0x200000ULL && s.level == 0;
	}

	/*
	 * ── The entries that point a device at a table (stage 3c) ────
	 *
	 * 🔴 The pass-through pair above and this pair differ in more than a
	 * pointer, which is the reason they are four encoders and not two with
	 * a parameter.  Intel's TYPE changes from 10b to 00b, and AMD's MODE
	 * from "translation off" to a depth -- and on AMD the root pointer is
	 * IGNORED in the entry above, so adding one to it would have produced
	 * an entry that still forwarded everything untranslated.
	 */
	{
		uint64_t c[2], d[4];

		/* Intel: present, type 00b, the root in 63:12, AW 010b, dom 2. */
		iommu_vtd_context_domain(2, 4, 0x123456000ULL, c);
		ok &= c[0] == 0x123456001ULL && c[1] == 0x202ULL;

		/* ⚠️ Type 00b means those two bits are CLEAR where 10b set one. */
		ok &= (c[0] & 0xCULL) == 0;

		/* AMD: valid, mode 100b for four levels, root, both permissions. */
		iommu_amd_dte_domain(2, 4, 0x123456000ULL, d);
		ok &= d[0] == 0x6000000123456803ULL && d[1] == 2
		   && d[2] == 0 && d[3] == 0;

		/* Three levels is mode 011b, and only that field moves. */
		iommu_amd_dte_domain(2, 3, 0x123456000ULL, d);
		ok &= d[0] == 0x6000000123456603ULL;

		/*
		 * 🔴 And neither may be mistaken for its pass-through twin: on
		 * AMD that one has mode 000b and NO pointer, so an entry built
		 * by adding a pointer to it would forward everything while
		 * looking configured.
		 */
		iommu_amd_dte_passthrough(2, d);
		ok &= (d[0] & (7ULL << 9)) == 0
		   && (d[0] & 0x000FFFFFFFFFF000ULL) == 0;
	}

	return ok;
}

/* Whether one scope's range contains this device. */
static int scope_covers(const struct iommu_scope *s, uint16_t segment,
			uint8_t bus, uint8_t dev, uint8_t func)
{
	uint32_t want, first, last;

	if (s->segment != segment)
		return 0;

	/*
	 * Compared as one number rather than three, because a range is
	 * ordered by the device id as a whole: 00:1f.7 to 01:00.0 is a legal
	 * range with a smaller function at its end, and three independent
	 * comparisons would exclude it.
	 */
	want  = ((uint32_t)bus << 8) | ((uint32_t)dev << 3) | func;
	first = ((uint32_t)s->bus << 8) | ((uint32_t)s->dev << 3) | s->func;
	last  = ((uint32_t)s->last_bus << 8) | ((uint32_t)s->last_dev << 3)
	      | s->last_func;

	return want >= first && want <= last;
}

int iommu_unit_for(uint16_t segment, uint8_t bus, uint8_t dev, uint8_t func)
{
	int catch_all = -1;

	for (unsigned i = 0; i < nunits; i++) {
		const struct iommu_unit *u = &units[i];

		for (unsigned s = 0; s < u->scope_count; s++)
			if (scope_covers(&scopes[u->scope_first + s], segment,
					 bus, dev, func))
				return (int)i;

		/*
		 * Remembered rather than returned, so that a unit which names
		 * this device explicitly wins over one that merely claims
		 * everything else -- which is what "everything else" means.
		 */
		if (u->covers_rest && u->segment == segment)
			catch_all = (int)i;
	}

	return catch_all;
}

int iommu_address_bits_ok(unsigned bits)
{
	if (nunits == 0)
		return 0;

	for (unsigned i = 0; i < nunits; i++)
		if (!units[i].answered || units[i].address_bits < bits)
			return 0;

	return 1;
}

/*
 * ── Stage 3b: a page table, and the walk that reads it back ──────────
 *
 * The two vendors index their tables identically -- nine bits per level, the
 * lowest twelve untranslated -- which is the one place they agree completely,
 * both having taken it from the processor's own paging.  So the indexing is
 * here, once, and everything the two disagree about is behind the decoders.
 *
 * ⚠️ Level 1 is the bottom.  Table 15 of AMD Rev 3.11 and §3.6 of Intel Rev
 * 5.20 number them the same way, so an entry fetched from a level-L table is
 * chosen by address bits (12 + 9L - 1):(12 + 9(L-1)).
 */
static unsigned level_index(uint64_t iova, unsigned level)
{
	return (unsigned)((iova >> (12u + 9u * (level - 1u))) & 511u);
}

static int pt_decode(enum iommu_vendor vendor, uint64_t entry, unsigned level,
		     struct iommu_pt_step *step)
{
	if (vendor == IOMMU_INTEL)
		return iommu_vtd_pt_decode(entry, level, step);

	return iommu_amd_pt_decode(entry, level, step);
}

static uint64_t pt_pde(enum iommu_vendor vendor, uint64_t next_table_pa,
		       unsigned next_level)
{
	if (vendor == IOMMU_INTEL)
		return iommu_vtd_ss_pde(next_table_pa);

	return iommu_amd_pde(next_table_pa, next_level);
}

static uint64_t pt_pte(enum iommu_vendor vendor, uint64_t pa, int read,
		       int write)
{
	if (vendor == IOMMU_INTEL)
		return iommu_vtd_ss_pte(pa, read, write);

	return iommu_amd_pte(pa, read, write);
}

static volatile uint64_t *table_at(uint64_t pa)
{
	return (volatile uint64_t *)(uintptr_t)phys_to_direct(pa);
}

/*
 * A frame for a page table, from whichever allocator owns physical memory
 * right now.
 *
 * 🔥 pmap_table_frame() AND NOT boot_frame_alloc(), and the difference is the
 * whole reason stage 3d could not have been written by copying stage 3b.  The
 * boot allocator is EMPTY after vm_page_bootstrap(): the VM takes every
 * remaining page through pmap_next_page(), which on this machine *is*
 * boot_frame_alloc().  A domain built during the boot self-test therefore
 * works, and the same code called from a device_dma_alloc RPC gets zero and
 * maps half a range.
 *
 * 🔑 THE CLASS WAS ALREADY NAMED, one file over.  pmap_create()'s comment
 * calls it "asks the boot allocator for a frame after the VM has taken it
 * over" and lists its three members -- pmap_create itself, the large-page
 * split in pmap/map.c, and pv_alloc() in pmap/pv.c.  This is the fourth, and
 * it was going to join by being called at a new time rather than by being
 * written wrongly: the line was correct for every caller it had.
 */
static uint64_t domain_frame(void)
{
	return pmap_table_frame();
}

int iommu_domain_create(struct iommu_domain *d, enum iommu_vendor vendor,
			uint16_t id, unsigned levels)
{
	if (vendor == IOMMU_NONE || levels < 2 || levels > 5)
		return 0;

	d->root = domain_frame();
	if (d->root == 0)
		return 0;

	d->vendor = vendor;
	d->levels = levels;
	d->id = id;
	d->frames = 1;
	d->pages = 0;
	return 1;
}

int iommu_domain_map(struct iommu_domain *d, uint64_t iova, uint64_t pa,
		     uint64_t size, int read, int write)
{
	if (d->root == 0 || size == 0)
		return 0;

	/*
	 * ⚠️ Refused rather than rounded.  A caller that asked to map half a
	 * page meant something, and mapping the whole one would grant a device
	 * reach over memory the caller never named.
	 */
	if (((iova | pa | size) & 0xFFFULL) != 0)
		return 0;

	/*
	 * 🔴 AND NOTHING MAY EVER TRANSLATE INTO THE INTERRUPT RANGE.  Both
	 * vendors say so in their own words -- see the citations in
	 * <cpu/iommu_backend.h> -- and the reason is worse than a rule: that
	 * range is the local APIC's, so a domain that mapped a device onto it
	 * would let the device raise interrupts by writing its own DMA buffer.
	 * Refused here, where every mapping passes, rather than trusted to
	 * every future caller.
	 */
	if (pa + size > IOMMU_INTERRUPT_RANGE_BASE
	    && pa <= IOMMU_INTERRUPT_RANGE_LIMIT)
		return 0;

	for (uint64_t off = 0; off < size; off += 4096) {
		uint64_t here = iova + off;
		uint64_t table = d->root;
		unsigned level = d->levels;

		/*
		 * Down to the bottom, building the road where there is none.
		 * The descent reads each entry through the same decoder the
		 * walk uses, so a directory this loop cannot understand is one
		 * the walk could not have understood either.
		 */
		while (level > 1) {
			volatile uint64_t *entries = table_at(table);
			unsigned index = level_index(here, level);
			struct iommu_pt_step step;
			uint64_t below;

			if (pt_decode(d->vendor, entries[index], level,
				      &step)) {
				/*
				 * ⚠️ A translation here is a large page over
				 * this address, and mapping inside it means
				 * splitting it.  Refused: nothing here builds
				 * one, so the split would be code no boot runs
				 * -- and a splitter that is wrong leaves a
				 * device reaching the memory either side.
				 */
				if (step.level == 0)
					return 0;

				table = step.next;
				level = step.level;
				continue;
			}

			below = domain_frame();
			if (below == 0)
				return 0;

			d->frames++;
			entries[index] = pt_pde(d->vendor, below, level - 1u);
			table = below;
			level--;
		}

		table_at(table)[level_index(here, 1)]
			= pt_pte(d->vendor, pa + off, read, write);
		d->pages++;
	}

	return 1;
}

int iommu_domain_walk(const struct iommu_domain *d, uint64_t iova,
		      uint64_t *pa, int *read, int *write)
{
	uint64_t table = d->root;
	unsigned level = d->levels;
	int r = 1, w = 1;

	if (d->root == 0)
		return 0;

	/*
	 * ⚠️ Bounded by the depth, and the bound cannot be reached: every step
	 * either ends the walk or moves to a strictly lower level, which the
	 * decoders enforce.  It is written as a bound anyway so that a decoder
	 * which ever stopped enforcing that stops this walk instead of the
	 * machine.
	 */
	for (unsigned step_count = 0; step_count < d->levels; step_count++) {
		struct iommu_pt_step step;
		uint64_t entry = table_at(table)[level_index(iova, level)];

		if (!pt_decode(d->vendor, entry, level, &step))
			return 0;

		/*
		 * Both vendors AND permission down the whole walk, so a
		 * directory that withholds one withholds it from everything
		 * below.  Accumulated here rather than in the decoders because
		 * it is the one rule the two formats state identically.
		 */
		r &= step.read;
		w &= step.write;

		if (step.level == 0) {
			*pa = step.next | (iova & (iommu_level_span(level) - 1ULL));
			*read = r;
			*write = w;
			return 1;
		}

		/*
		 * 🔴 Levels the entry skipped must have been chosen by address
		 * bits that are zero -- AMD Rev 3.11 §2.2.3, "if a translation
		 * skips levels and any of the skipped virtual address bits are
		 * non-zero, translation terminates with an IO_PAGE_FAULT".
		 *
		 * 🔑 Without this the walk would arrive at the table below
		 * having never consumed the bits that chose it, index it with
		 * the wrong ones, and land on a real entry.  The answer would
		 * be a physical address, wrong, and shaped exactly like a right
		 * one.  Intel cannot express a skip, so this is never true
		 * there -- which is why the check that exercises it reports one
		 * vendor and not two.
		 */
		if (step.level + 1u != level) {
			/*
			 * The levels passed over are step.level+1 up to
			 * level-1, and the bits they would have been indexed
			 * by are everything below this level's own down to
			 * where the next one starts.
			 */
			uint64_t skipped = iommu_level_span(level)
					 - iommu_level_span(step.level + 1u);

			if ((iova & skipped) != 0)
				return 0;
		}

		/*
		 * ⚠️ The next level comes from the ENTRY and not from level-1.
		 * On AMD the entry is where the answer lives, and a walk that
		 * counted down instead of reading would be right on every table
		 * this kernel builds and wrong on one built by anything else.
		 */
		table = step.next;
		level = step.level;
	}

	return 0;
}

/*
 * ── And the check that all of the above is worth anything ────────────
 *
 * Build a table for each vendor, map a range into it, walk every page back.
 *
 * 🔴 BOTH VENDORS ON EVERY MACHINE.  A page table is arithmetic and memory,
 * and nothing here needs an engine -- so the AMD format is built and walked on
 * Intel hardware and on machines with no remapping hardware at all.  The
 * alternative is that each format is only ever exercised where its silicon is,
 * which means the format that is wrong is the one this machine cannot run.
 *
 * The cost is twelve frames and about four thousand memory reads, once, at
 * boot, on every board.
 */
#define	PT_CHECK_LEVELS		4u
#define	PT_CHECK_PAGES		1025u
#define	PT_CHECK_PA_BASE	0x00000000AB000000ULL

/*
 * 🔴 A DIFFERENT INDEX AT EVERY LEVEL, AND NONE OF THEM ZERO.  A range starting
 * at zero has every index zero, so a walk with the shift wrong at one level
 * reads entry zero of the right table and answers correctly.  This one puts 5,
 * 4, 3 and 2 at levels 4 down to 1, so a wrong shift lands somewhere empty.
 */
static uint64_t check_iova(void)
{
	uint64_t v = 0;

	for (unsigned l = 1; l <= PT_CHECK_LEVELS; l++)
		v |= (uint64_t)(l + 1u) << (12u + 9u * (l - 1u));

	return v;
}

/*
 * 🔴 REVERSED, so that the mapping is not a function of the address.  An
 * identity map cannot tell a walk that read the tables from a walk that
 * returned its own argument, and an offset cannot tell it from one that added a
 * constant.  Running backwards, the only way to get the right answer is to have
 * read the entry.
 */
static uint64_t check_pa(unsigned page)
{
	return PT_CHECK_PA_BASE
	     + (uint64_t)(PT_CHECK_PAGES - 1u - page) * 4096u;
}

/* Every third page read-only, so the permission bits are carried, not assumed. */
static int check_write(unsigned page)
{
	return (page % 3u) != 0u;
}

static int pt_ablate(enum iommu_vendor vendor, unsigned kind, unsigned level,
		     uint64_t *entry)
{
	if (vendor == IOMMU_INTEL)
		return iommu_vtd_pt_ablate(kind, level, entry);

	return iommu_amd_pt_ablate(kind, level, entry);
}

/* Where one address's entry lives at a given level, or nothing. */
static volatile uint64_t *entry_at(const struct iommu_domain *d, uint64_t iova,
				   unsigned want_level)
{
	uint64_t table = d->root;
	unsigned level = d->levels;

	while (level > want_level) {
		struct iommu_pt_step step;

		if (!pt_decode(d->vendor,
			       table_at(table)[level_index(iova, level)],
			       level, &step))
			return 0;

		if (step.level == 0)
			return 0;

		table = step.next;
		level = step.level;
	}

	if (level != want_level)
		return 0;

	return &table_at(table)[level_index(iova, level)];
}

/*
 * The damage, and what it should do.  `answers' is whether a walk still
 * produces an address afterwards -- which is the whole difficulty with these
 * structures: two of the three mistakes below leave the walk succeeding, at a
 * different place, with nothing anywhere to say so.
 */
static const struct {
	unsigned	kind;
	unsigned	level;
	int		answers;
} pt_ablations[] = {
	{ IOMMU_ABLATE_ROAD_IS_DESTINATION,	2, 1 },
	{ IOMMU_ABLATE_DENY_ON_THE_ROAD,	2, 1 },
	{ IOMMU_ABLATE_SKIP_A_LEVEL,		3, 0 },
};

static int pt_skip(enum iommu_vendor vendor, uint64_t next_table_pa,
		   unsigned next_level, uint64_t *entry)
{
	if (vendor == IOMMU_INTEL)
		return iommu_vtd_pt_skip(next_table_pa, next_level, entry);

	return iommu_amd_pt_skip(next_table_pa, next_level, entry);
}

/*
 * ── Skipping a level, the legal way and the illegal one ──────────────
 *
 * 🔴 ONE SKIP THAT MUST BE FOLLOWED, AND ONE THAT MUST BE REFUSED, and neither
 * means anything without the other.
 *
 *	Without the accepted one, a walk that takes the next level from
 *	`level - 1' instead of from the entry passes everything: no table this
 *	kernel builds skips, and the ablation that skips wrongly is expected to
 *	be refused anyway.  A walker that refused all skipping would score
 *	perfectly.
 *
 *	Without the refused one, the rule that skipped address bits must be
 *	zero -- AMD Rev 3.11 §2.2.3 -- is a guard that never fires.  It was
 *	exactly that when this was first written: removing it changed no
 *	answer, because the walk it should have stopped ran into an entry that
 *	refused it for an unrelated reason.
 *
 * So the illegal skip is aimed at a page that IS there.  Then the only thing
 * standing between the walk and a plausible wrong physical address is the rule
 * being tested.
 *
 * ⚠️ Neither address is 2-Mbyte aligned, and neither has a zero index at the
 * level below the skip.  Both are ways a walk that lost a level lands on
 * something that happens to be right.
 */
#define	PT_CHECK_SKIP_PA	0x00000000AC001000ULL
#define	PT_CHECK_DECOY_PA	0x00000000AD002000ULL

static unsigned check_skipping(struct iommu_domain *d, uint64_t probe,
			       unsigned *walked)
{
	uint64_t under = check_iova() & ~(iommu_level_span(3) - 1ULL);
	uint64_t over_zeros = under | (7ULL << 12);

	/*
	 * The decoy sits at the index the illegal walk would use: `probe' has
	 * bits 20:12 of its own, and this is the page they would find in the
	 * table the skip arrives at.  Without it the illegal walk stops on an
	 * empty entry and the rule under test never gets a turn.
	 */
	uint64_t decoy = under | (probe & (iommu_level_span(2) - 1ULL));

	volatile uint64_t *directory;
	struct iommu_pt_step step;
	uint64_t below, saved, skipping, pa = 0;
	int read = 0, write = 0;
	unsigned bad = 0;

	if (!iommu_domain_map(d, over_zeros, PT_CHECK_SKIP_PA, 4096u, 1, 1)
	    || !iommu_domain_map(d, decoy, PT_CHECK_DECOY_PA, 4096u, 1, 1))
		return 1;

	/* The bottom table, found before the road to it is rewritten. */
	{
		volatile uint64_t *middle = entry_at(d, over_zeros, 2);

		if (middle == 0 || !pt_decode(d->vendor, *middle, 2, &step)
		    || step.level != 1)
			return 1;

		below = step.next;
	}

	directory = entry_at(d, over_zeros, 3);
	if (directory == 0)
		return 1;

	saved = *directory;

	if (!pt_skip(d->vendor, below, 1u, &skipping))
		return 0;

	*directory = skipping;

	/* The legal one: the skipped bits are zero, so the page is found. */
	(*walked)++;
	if (!iommu_domain_walk(d, over_zeros, &pa, &read, &write)
	    || pa != PT_CHECK_SKIP_PA || !read || !write)
		bad++;

	/*
	 * 🔴 And the illegal one through the SAME entry: `probe' has a nonzero
	 * index at the level being skipped, so the walk must refuse -- with a
	 * mapped page waiting at the far end for it to find if it does not.
	 */
	(*walked)++;
	if (iommu_domain_walk(d, probe, &pa, &read, &write))
		bad++;

	*directory = saved;
	(*walked)++;
	if (!iommu_domain_walk(d, over_zeros, &pa, &read, &write)
	    || pa != PT_CHECK_SKIP_PA)
		bad++;

	return bad;
}

static unsigned check_one_vendor(enum iommu_vendor vendor, unsigned *walked)
{
	struct iommu_domain d;
	uint64_t base = check_iova();
	uint64_t probe = base + 4096u;		/* page 1: writable */
	uint64_t good_pa = 0;
	int good_read = 0, good_write = 0;
	unsigned bad = 0;

	if (!iommu_domain_create(&d, vendor, IOMMU_DOMAIN_PASSTHROUGH + 1u,
				 PT_CHECK_LEVELS))
		return 1;

	for (unsigned i = 0; i < PT_CHECK_PAGES; i++)
		if (!iommu_domain_map(&d, base + (uint64_t)i * 4096u,
				      check_pa(i), 4096u, 1, check_write(i)))
			return 1;

	for (unsigned i = 0; i < PT_CHECK_PAGES; i++) {
		uint64_t pa = 0;
		int read = 0, write = 0;

		(*walked)++;
		if (!iommu_domain_walk(&d, base + (uint64_t)i * 4096u,
				       &pa, &read, &write)
		    || pa != check_pa(i) || !read
		    || write != check_write(i))
			bad++;
	}

	/*
	 * 🔴 AND ONE ADDRESS THAT WAS NEVER MAPPED, WHICH MUST NOT ANSWER.
	 * Every check above is a walk that succeeds, and a walk that answered
	 * for everything would pass all thousand of them.  The page below the
	 * range is in a table that exists, so this asks about an empty entry
	 * and not about an empty table.
	 */
	{
		uint64_t pa = 0;
		int read = 0, write = 0;

		(*walked)++;
		if (iommu_domain_walk(&d, base - 4096u, &pa, &read, &write))
			bad++;
	}

	/*
	 * 🔴 AND A MAPPING ONTO THE INTERRUPT RANGE MUST BE REFUSED.  Both
	 * vendors forbid it in their own words, and the consequence of allowing
	 * it is a device that raises interrupts by writing its own buffer.
	 * Asked at three places: one page below the range, which is legal; the
	 * first page of it; and a range that merely overlaps its end.
	 */
	{
		uint64_t away = base + (uint64_t)PT_CHECK_PAGES * 4096u;

		if (!iommu_domain_map(&d, away, IOMMU_INTERRUPT_RANGE_BASE
					       - 4096u, 4096u, 1, 1))
			bad++;
		if (iommu_domain_map(&d, away + 4096u,
				     IOMMU_INTERRUPT_RANGE_BASE, 4096u, 1, 1))
			bad++;
		if (iommu_domain_map(&d, away + 8192u,
				     IOMMU_INTERRUPT_RANGE_LIMIT + 1u - 4096u,
				     8192u, 1, 1))
			bad++;
	}

	if (!iommu_domain_walk(&d, probe, &good_pa, &good_read, &good_write))
		return bad + 1u;

	for (unsigned a = 0; a < sizeof(pt_ablations) / sizeof(pt_ablations[0]);
	     a++) {
		volatile uint64_t *slot
			= entry_at(&d, probe, pt_ablations[a].level);
		uint64_t saved, damaged, pa = 0;
		int read = 0, write = 0, answered;

		if (slot == 0) {
			bad++;
			continue;
		}

		saved = *slot;
		damaged = saved;

		/*
		 * 🔑 A format that cannot express a mistake cannot make it.
		 * Intel has no field with which to skip a level, so it has no
		 * way to skip one wrongly, and that case is AMD's alone.
		 */
		if (!pt_ablate(vendor, pt_ablations[a].kind,
			       pt_ablations[a].level, &damaged))
			continue;

		*slot = damaged;
		(*walked)++;
		answered = iommu_domain_walk(&d, probe, &pa, &read, &write);

		if (answered != pt_ablations[a].answers)
			bad++;
		else if (answered && pa == good_pa && read == good_read
			 && write == good_write)
			bad++;		/* the damage changed nothing */

		/*
		 * ⚠️ And put it back, then walk again.  Without this the check
		 * would leave a table it had broken and could not say whether
		 * the walk noticed the damage or had simply stopped working.
		 */
		*slot = saved;
		(*walked)++;
		if (!iommu_domain_walk(&d, probe, &pa, &read, &write)
		    || pa != good_pa || read != good_read
		    || write != good_write)
			bad++;
	}

	return bad + check_skipping(&d, probe, walked);
}

int iommu_domain_check(unsigned *walked, unsigned *wrong)
{
	unsigned n = 0, bad = 0;

	bad += check_one_vendor(IOMMU_INTEL, &n);
	bad += check_one_vendor(IOMMU_AMD, &n);

	if (walked)
		*walked = n;
	if (wrong)
		*wrong = bad;

	return bad == 0;
}

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
 */
static void device_fault_seen(uint16_t bdf);
static unsigned device_fault_count(uint16_t bdf);

void iommu_record_fault(const struct iommu_fault *f)
{
	fault_log[fault_total % IOMMU_FAULT_LOG] = *f;
	fault_total++;
	device_fault_seen(f->source);
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

	for (unsigned i = 0; i < nunits; i++)
		if (found_vendor == IOMMU_INTEL)
			found += iommu_vtd_fault_drain(i, &fault_overflow);
		else if (found_vendor == IOMMU_AMD)
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
	unsigned before = fault_total;
	unsigned printed = 0;
	unsigned lost;

	/*
	 * ⚠️ Nothing to poll until a device is in a domain.  Under
	 * pass-through nothing can be refused, so this is not an optimisation
	 * that skips a check -- it is the check having a known answer, and it
	 * is what keeps this off the idle path of every machine that is not
	 * using the feature.
	 */
	if (iommu_domain_count() == 0)
		return 0;

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
	if (iommu_fault_overflowed())
		printf("iommu: an engine ran out of fault records before"
		       " anyone read them — the count above is a floor\n");

	return printed;
}

unsigned iommu_faults_for(uint16_t bdf, uint64_t *last_address)
{
	/*
	 * ⚠️ The COUNT comes from the per-device total and the ADDRESS from
	 * the ring, which is the honest split: the first is a number that only
	 * goes up, the second is a record that can be pushed out.  A caller
	 * given a non-zero count and no address knows the refusal happened and
	 * that this log no longer says where -- which is a worse answer than a
	 * complete one and a much better answer than a wrong one.
	 */
	for (unsigned i = 0; i < iommu_fault_logged(); i++) {
		const struct iommu_fault *f = iommu_fault(i);

		if (f != 0 && f->source == bdf && last_address)
			*last_address = f->address;
	}

	return device_fault_count(bdf);
}

/*
 * ── The fault decode, against the figures ────────────────────────────
 *
 * 🔴 THE ONLY CHECK THIS CODE CAN GET, ON ALMOST EVERY BOOT.  A fault record
 * is read exactly when something has already gone wrong, and a machine whose
 * drivers behave produces none at all -- so without this the reader would be
 * exercised for the first time on the day it was needed, which is the day
 * nobody wants to be debugging it.
 *
 * The words below are written from Rev 5.20 Figure 11-15 and Rev 3.11 Figure
 * 56 by hand, and several are words neither engine we can run would produce.
 */
struct fault_case {
	const char		*what;
	int			 amd;
	uint64_t		 lo;
	uint64_t		 hi;
	int			 decodes;
	uint64_t		 address;
	uint16_t		 source;
	uint16_t		 domain;
	uint8_t			 reason;
	uint8_t			 kind;
	uint8_t			 write;
};

static const struct fault_case fault_cases[] = {
	/*
	 * Intel, a write refused for want of write permission at
	 * 0x00000000deadb000, from 00:1f.2 -- which is q35's own AHCI, and the
	 * device this issue exists to put behind a domain.
	 *
	 * F set, T1=T2=0 (write), FR=5, SID=0x00FA.
	 */
	{ "intel, write denied", 0,
	  0x00000000deadb000ULL,
	  (1ULL << 63) | (0x05ULL << 32) | 0x00FAULL,
	  1, 0x00000000deadb000ULL, 0x00FA, IOMMU_FAULT_NO_DOMAIN,
	  0x05, IOMMU_FAULT_PAGE, 1 },

	/*
	 * The same record with T1 set: a READ refused, and the ONLY difference
	 * in the whole word is bit 126.  A decode that dropped the type would
	 * pass every other assertion here.
	 */
	{ "intel, read denied", 0,
	  0x00000000deadb000ULL,
	  (1ULL << 63) | (1ULL << 62) | (0x06ULL << 32) | 0x00FAULL,
	  1, 0x00000000deadb000ULL, 0x00FA, IOMMU_FAULT_NO_DOMAIN,
	  0x06, IOMMU_FAULT_PAGE, 0 },

	/*
	 * 🔴 T1 CLEAR AND T2 SET IS A PAGE REQUEST, NOT A WRITE.  This is the
	 * case that fails on a reader carrying the pre-5.x single-bit T, and
	 * it cannot fail on any hardware we can run -- QEMU issues no page
	 * requests.
	 */
	{ "intel, page request (synthetic)", 0,
	  0x1000ULL,
	  (1ULL << 63) | (1ULL << 28) | (0x05ULL << 32) | 0x00FAULL,
	  1, 0x1000ULL, 0x00FA, IOMMU_FAULT_NO_DOMAIN,
	  0x05, IOMMU_FAULT_PAGE, 0 },

	/* And 11b, an AtomicOp, which is likewise not a read. */
	{ "intel, atomicop (synthetic)", 0,
	  0x1000ULL,
	  (1ULL << 63) | (1ULL << 62) | (1ULL << 28) | (0x06ULL << 32)
	  | 0x00FAULL,
	  1, 0x1000ULL, 0x00FA, IOMMU_FAULT_NO_DOMAIN,
	  0x06, IOMMU_FAULT_PAGE, 0 },

	/*
	 * 🔴 F CLEAR IS NOT A FAULT, whatever else the record holds.  Every
	 * other field here says "a write from 00:1f.2 was denied", and the
	 * answer must still be no -- this is the state of every unfaulted
	 * record on the machine, so a reader that got it wrong would report
	 * NFR+1 faults on the first poll of every boot.
	 */
	{ "intel, F clear", 0,
	  0x00000000deadb000ULL, (0x05ULL << 32) | 0x00FAULL,
	  0, 0, 0, 0, 0, 0, 0 },

	/* Ah is a ROOT entry's reserved field, and Ch is a page entry's. */
	{ "intel, root reserved", 0,
	  0x2000ULL, (1ULL << 63) | (0x0AULL << 32) | 0x0100ULL,
	  1, 0x2000ULL, 0x0100, IOMMU_FAULT_NO_DOMAIN,
	  0x0A, IOMMU_FAULT_ENTRY, 1 },
	{ "intel, page entry reserved", 0,
	  0x2000ULL, (1ULL << 63) | (0x0CULL << 32) | 0x0100ULL,
	  1, 0x2000ULL, 0x0100, IOMMU_FAULT_NO_DOMAIN,
	  0x0C, IOMMU_FAULT_PAGE, 1 },

	/* An engine that could not read its own table: neither of the above. */
	{ "intel, context unreadable", 0,
	  0x3000ULL, (1ULL << 63) | (0x09ULL << 32) | 0x0100ULL,
	  1, 0x3000ULL, 0x0100, IOMMU_FAULT_NO_DOMAIN,
	  0x09, IOMMU_FAULT_HARDWARE, 1 },

	/*
	 * ⚠️ The low twelve bits of FI are RsvdZ and the address is a PAGE
	 * address.  A record whose low bits are set is one the engine should
	 * not produce -- and a decode that let them through would hand a
	 * caller an address that is not the page that faulted.
	 */
	{ "intel, low bits are not address", 0,
	  0x0000000012345FFFULL, (1ULL << 63) | (0x05ULL << 32) | 0x00FAULL,
	  1, 0x0000000012345000ULL, 0x00FA, IOMMU_FAULT_NO_DOMAIN,
	  0x05, IOMMU_FAULT_PAGE, 1 },

	/*
	 * AMD, an IO_PAGE_FAULT for a page that is not present: PR clear, so
	 * the entry carries NO direction and the decode must not invent one
	 * out of RW -- which is set here precisely to catch a reader that does.
	 */
	{ "amd, page not present", 1,
	  (2ULL << 60) | (1ULL << 53) | (7ULL << 32) | 0x0102ULL,
	  0x00000000deadb000ULL,
	  1, 0x00000000deadb000ULL, 0x0102, 7,
	  0x2, IOMMU_FAULT_PAGE, 0 },

	/* Present, not a translation, not an interrupt, RW set: a write. */
	{ "amd, write denied", 1,
	  (2ULL << 60) | (1ULL << 53) | (1ULL << 52) | (7ULL << 32) | 0x0102ULL,
	  0x00000000deadb000ULL,
	  1, 0x00000000deadb000ULL, 0x0102, 7,
	  0x2, IOMMU_FAULT_PAGE, 1 },

	/* The same with RW clear: a read. */
	{ "amd, read denied", 1,
	  (2ULL << 60) | (1ULL << 52) | (7ULL << 32) | 0x0102ULL,
	  0x00000000deadb000ULL,
	  1, 0x00000000deadb000ULL, 0x0102, 7,
	  0x2, IOMMU_FAULT_PAGE, 0 },

	/*
	 * 🔴 GN SET MAKES D/P A PASID, NOT A DOMAIN.  The sixteen bits are
	 * identical and only this one says which they are, so a decode that
	 * ignored it would report a domain id that is somebody's PASID -- a
	 * number that looks exactly like an answer.
	 */
	{ "amd, guest address carries a pasid", 1,
	  (2ULL << 60) | (1ULL << 52) | (1ULL << 48) | (7ULL << 32) | 0x0102ULL,
	  0x1000ULL,
	  1, 0x1000ULL, 0x0102, IOMMU_FAULT_NO_DOMAIN,
	  0x2, IOMMU_FAULT_PAGE, 0 },

	/* An unusable device table entry is not a page fault. */
	{ "amd, illegal dte", 1,
	  (1ULL << 60) | 0x0102ULL, 0x4000ULL,
	  1, 0x4000ULL, 0x0102, 0, 0x1, IOMMU_FAULT_ENTRY, 0 },

	/* Nor is the engine failing to read the table at all. */
	{ "amd, device table hardware error", 1,
	  (3ULL << 60) | 0x0102ULL, 0x5000ULL,
	  1, 0x5000ULL, 0x0102, 0, 0x3, IOMMU_FAULT_HARDWARE, 0 },

	/*
	 * 🔴 AN ALL-ZERO ENTRY IS NOT AN EVENT.  This is what an unwritten
	 * ring slot reads as, and the ring is ordinary memory -- so a decode
	 * that accepted event code 0000b would turn every empty slot into a
	 * fault at address zero from device 0000.
	 */
	{ "amd, empty ring slot", 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },

	/*
	 * A code Table 42 reserves.  Reported with its raw number and no
	 * reading -- inventing a kind for an event this kernel does not
	 * understand would be worse than saying so.
	 */
	{ "amd, reserved event code (synthetic)", 1,
	  (0xAULL << 60) | 0x0102ULL, 0x6000ULL,
	  1, 0x6000ULL, 0x0102, 0, 0xA, IOMMU_FAULT_UNKNOWN, 0 },
};

/*
 * ── Stage 3d: one domain per device ──────────────────────────────────
 *
 * A flat table, because there are as many entries as there are devices this
 * kernel drives and that is a handful.  ⚠️ Full is REPORTED and not grown: a
 * grant that cannot be recorded must fail, since the alternative is a device
 * that thinks it was granted memory and an engine that was never told.
 *
 * 🔴 THE DOMAIN ID STARTS AT ONE.  IOMMU_DOMAIN_PASSTHROUGH is zero and is the
 * domain every device is in before this, so handing a real domain the same id
 * would make "which domain refused this" unanswerable in exactly the case the
 * fault log exists for.
 */
struct device_domain {
	uint16_t		bdf;
	int			used;
	unsigned		faults;		/* refusals, never wrapping */
	struct iommu_domain	domain;
};

static struct device_domain	device_domains[IOMMU_MAX_DEVICE_DOMAINS];
static unsigned			ndevice_domains;

int iommu_can_isolate(void)
{
	/*
	 * ⚠️ Four separate noes, and the truncation one is the one that would
	 * be missed.  A description that did not fit is a set of engines
	 * nobody programs, and iommu_truncated() says so -- promising
	 * isolation on such a machine would be promising it for the devices
	 * some unrecorded engine covers, which is the promise this whole issue
	 * exists to make true rather than plausible.
	 */
	if (found_vendor == IOMMU_NONE || nunits == 0)
		return 0;
	if (truncated || !walk_exact)
		return 0;
	if (!translating)
		return 0;

	for (unsigned i = 0; i < nunits; i++)
		if (!units[i].answered || units[i].register_va == 0)
			return 0;

	return 1;
}

/*
 * ⚠️ A refusal from a device that is in NO domain is counted nowhere, and that
 * is not a gap: a device passing through cannot be refused, so a fault naming
 * one is an engine translating by a description this kernel did not write --
 * which iommu_fault_report() prints, loudly, and no per-device counter would
 * make more legible.
 */
static void device_fault_seen(uint16_t bdf)
{
	for (unsigned i = 0; i < ndevice_domains; i++)
		if (device_domains[i].used && device_domains[i].bdf == bdf)
			device_domains[i].faults++;
}

static unsigned device_fault_count(uint16_t bdf)
{
	for (unsigned i = 0; i < ndevice_domains; i++)
		if (device_domains[i].used && device_domains[i].bdf == bdf)
			return device_domains[i].faults;

	return 0;
}

static struct device_domain *domain_slot(uint16_t bdf)
{
	for (unsigned i = 0; i < ndevice_domains; i++)
		if (device_domains[i].used && device_domains[i].bdf == bdf)
			return &device_domains[i];

	return 0;
}

const struct iommu_domain *iommu_domain_of(uint16_t bdf)
{
	struct device_domain *s = domain_slot(bdf);

	return s == 0 ? 0 : &s->domain;
}

unsigned iommu_domain_count(void)
{
	return ndevice_domains;
}

/*
 * How deep a table this machine's engines will walk.
 *
 * ⚠️ The SHALLOWEST depth every engine supports, not the deepest.  A domain is
 * one table and a device may be behind any engine, so a depth one engine
 * cannot walk is a root pointer it refuses -- and a refused root pointer is
 * not a wrong translation, it is an engine that stops working with nothing to
 * read.
 */
static unsigned domain_levels(void)
{
	uint32_t common = 0xFFFFFFFFu;

	for (unsigned i = 0; i < nunits; i++)
		common &= units[i].page_levels;

	for (unsigned l = 2; l <= 5; l++)
		if (common & (1u << l))
			return l;

	return 0;
}

/*
 * Create a device's domain and move it into it, in that order, atomically as
 * far as anything outside can tell: a slot is claimed only once both halves
 * have succeeded.
 *
 * 🔴 AN EMPTY DOMAIN DENIES EVERYTHING, which is why the caller of this must
 * map before it lets the device run -- and why iommu_grant() is one call.  A
 * device attached here and not granted anything is a device that has just lost
 * all of memory, and it would look exactly like a device that was never
 * attached until the first transfer failed.
 */
static struct device_domain *domain_open(uint16_t bdf)
{
	struct device_domain *s;
	unsigned levels = domain_levels();
	int ok;

	if (ndevice_domains >= IOMMU_MAX_DEVICE_DOMAINS || levels == 0)
		return 0;

	s = &device_domains[ndevice_domains];
	s->used = 0;
	s->bdf = bdf;

	if (!iommu_domain_create(&s->domain, found_vendor,
				 (uint16_t)(ndevice_domains + 1u), levels))
		return 0;

	ok = found_vendor == IOMMU_INTEL
	     ? iommu_vtd_attach(bdf, &s->domain)
	     : iommu_amd_attach(bdf, &s->domain);

	if (!ok)
		return 0;

	s->used = 1;
	ndevice_domains++;
	return s;
}

static int domain_flush(const struct iommu_domain *d)
{
	return found_vendor == IOMMU_INTEL ? iommu_vtd_flush(d)
					   : iommu_amd_flush(d);
}

int iommu_grant(uint16_t bdf, uint64_t pa, uint64_t size, int read, int write)
{
	struct device_domain *s;

	if (!iommu_can_isolate())
		return 0;

	s = domain_slot(bdf);
	if (s == 0)
		s = domain_open(bdf);
	if (s == 0)
		return 0;

	if (!iommu_domain_map(&s->domain, pa, pa, size, read, write))
		return 0;

	return domain_flush(&s->domain);
}

/*
 * ⚠️ A revoke is a map with no permissions, and the two vendors do not agree
 * about what that IS -- entries_agree() asserts the difference: AMD's entry is
 * present and refuses, Intel's is simply absent.  They agree about what the
 * device can do, which is nothing, and that is the only agreement needed here.
 *
 * 🔑 The frames the table is built from are NOT given back.  A revoke that
 * tore down empty directories would have to know that nothing else in the
 * domain is still reached through them, and getting that wrong unmaps memory
 * a device is using at that moment.  What it costs is a table that only grows,
 * for a driver that maps and unmaps the same buffers.
 */
int iommu_revoke(uint16_t bdf, uint64_t pa, uint64_t size)
{
	struct device_domain *s = domain_slot(bdf);

	if (s == 0)
		return 0;

	if (!iommu_domain_map(&s->domain, pa, pa, size, 0, 0))
		return 0;

	return domain_flush(&s->domain);
}

int iommu_fault_decode_check(unsigned *ran, unsigned *wrong)
{
	unsigned n = 0, bad = 0;

	for (unsigned i = 0;
	     i < sizeof(fault_cases) / sizeof(fault_cases[0]); i++) {
		const struct fault_case *c = &fault_cases[i];
		struct iommu_fault f;
		int got;

		/*
		 * ⚠️ Filled with something that is not the answer first, so
		 * that a decoder which leaves a field untouched fails here
		 * rather than agreeing with a zero the caller supplied.
		 */
		f.address = ~0ULL;
		f.source = 0xFFFF;
		f.domain = 0;
		f.reason = 0xFF;
		f.kind = 0xFF;
		f.write = 0xFF;
		f.vendor = 0xFF;

		got = c->amd ? iommu_amd_fault_decode(c->lo, c->hi, &f)
			     : iommu_vtd_fault_decode(c->lo, c->hi, &f);

		n++;

		if (got != c->decodes) {
			bad++;
			continue;
		}

		if (!got)
			continue;

		if (f.address != c->address || f.source != c->source
		    || f.domain != c->domain || f.reason != c->reason
		    || f.kind != c->kind || f.write != c->write
		    || f.vendor != (c->amd ? IOMMU_AMD : IOMMU_INTEL))
			bad++;
	}

	if (ran)
		*ran = n;
	if (wrong)
		*wrong = bad;

	return bad == 0;
}
