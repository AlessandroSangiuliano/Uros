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
#include <pmap/bootmem.h>
#include <pmap/layout.h>

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

int iommu_domain_create(struct iommu_domain *d, enum iommu_vendor vendor,
			uint16_t id, unsigned levels)
{
	if (vendor == IOMMU_NONE || levels < 2 || levels > 5)
		return 0;

	d->root = boot_frame_alloc();
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

			below = boot_frame_alloc();
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
