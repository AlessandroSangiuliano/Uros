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

#include <cpu/iommu_backend.h>

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

	if (iommu_vtd_read())
		iommu_record_vendor(IOMMU_INTEL);
	else if (iommu_amd_read())
		iommu_record_vendor(IOMMU_AMD);
	else
		iommu_record_vendor(IOMMU_NONE);

	return found_vendor;
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
