/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 *	iommu.h — what this machine has to police DMA with (#432, stage 1).
 *
 *	🔴 WITHOUT ONE OF THESE, "THE DRIVER RUNS IN USERSPACE" IS A ROBUSTNESS
 *	PROPERTY AND NOT A SECURITY ONE.
 *
 *	A userspace driver programs DMA by writing a physical address into a
 *	device register, and the device then writes wherever it was told.  No
 *	page table stands in the way, because the write is the device's and not
 *	the driver's; no capability stands in the way, because the capability
 *	system does not see it happen.  Today `device_dma_alloc' hands a driver
 *	the physical address of its buffer and the driver programs it, which
 *	means every isolation property we claim for the driver model is at
 *	present enforced by the driver being well behaved.
 *
 *	This file is the first of the three stages #432 describes, and it
 *	changes nothing: it finds the remapping hardware, reads what it can do,
 *	and says so.  The value of doing that on its own is that afterwards the
 *	sentence above is a MEASUREMENT rather than an assumption -- a boot can
 *	state whether this machine could enforce the claim, which is not a
 *	question the kernel could previously answer at all.
 *
 *	── Why the description here is not Intel's ───────────────────────────
 *
 *	Two vendors, two tables, one job.  Intel describes its units in a DMAR
 *	and AMD in an IVRS, and the two disagree about nearly every detail of
 *	encoding while agreeing about the shape of the answer: some number of
 *	remapping engines, each with registers somewhere, each covering some set
 *	of devices, plus regions of memory that certain devices must keep
 *	reaching or the machine breaks.
 *
 *	So the readers fill in this description and everything above them reads
 *	only this.  ⚠️ That the shape is right is a claim to be checked and not
 *	a hope, and the only thing that checks it is a second reader written
 *	against it -- which is why #432 asks for the second vendor to be a
 *	backend rather than a rewrite, and why the description below was
 *	designed against BOTH tables before either was read.
 *
 *	The two places they genuinely differ, and which this description had to
 *	absorb rather than flatten:
 *
 *	  - AMD names devices in RANGES and Intel one at a time, so a scope
 *	    carries a last bus/device/function as well as a first.  For Intel
 *	    they are equal, and a reader that forgets the range case gets one
 *	    device out of a hundred right.
 *
 *	  - Intel's capabilities are in the unit's own registers and AMD's are
 *	    partly in the table and partly in the registers.  Both are read from
 *	    the REGISTERS here, because a table is what the firmware says and a
 *	    register is what the silicon says, and stage 1 exists to find out
 *	    whether those two agree.
 */

#ifndef	_X86_64_CPU_IOMMU_H_
#define	_X86_64_CPU_IOMMU_H_

#include <stdint.h>

/*
 * Which of the two this machine turned out to have.
 *
 * ⚠️ IOMMU_NONE is an ANSWER.  A machine with no remapping hardware is not a
 * machine this kernel refuses to run on -- it is a machine on which stage 3
 * must decline to promise isolation, which is a decision that needs the
 * question asked out loud.
 */
enum iommu_vendor {
	IOMMU_NONE = 0,
	IOMMU_INTEL,			/* VT-d, described by a DMAR */
	IOMMU_AMD			/* AMD-Vi, described by an IVRS */
};

/*
 * What one entry in a unit's scope, or in a reserved region's scope, names.
 *
 * The last three are not PCI functions and cannot be programmed like one; they
 * are here because both vendors put them in the same lists, and dropping them
 * would leave a scope that is shorter than the table said without saying why.
 */
#define	IOMMU_SCOPE_ENDPOINT	1	/* a PCI function                    */
#define	IOMMU_SCOPE_BRIDGE	2	/* a bridge, and everything under it */
#define	IOMMU_SCOPE_IOAPIC	3
#define	IOMMU_SCOPE_HPET	4
#define	IOMMU_SCOPE_NAMESPACE	5	/* named in AML, not by a BDF        */

struct iommu_scope {
	uint8_t		kind;
	uint8_t		enumeration_id;	/* the I/O APIC's id, or the HPET's
					   number; meaningless otherwise     */
	uint16_t	segment;
	uint8_t		bus;
	uint8_t		dev;
	uint8_t		func;

	/*
	 * The end of the range, inclusive.  Equal to the three above unless the
	 * table named a range, which only AMD's does.
	 */
	uint8_t		last_bus;
	uint8_t		last_dev;
	uint8_t		last_func;

	/*
	 * How many hops the table took to reach it.
	 *
	 * 🔴 A depth above one means `bus' IS NOT THE DEVICE'S BUS.  Intel
	 * names a device behind bridges by a starting bus and a path of
	 * device/function pairs, and turning that into the bus the device is
	 * actually on means reading each bridge's secondary bus number out of
	 * configuration space.  Stage 1 does not do that -- it reports the
	 * depth so that a reader can see the difference between a device this
	 * description locates and one it merely describes the route to.
	 */
	uint8_t		depth;
};

/*
 * One remapping engine.
 *
 * The first half is what the firmware's table said.  The second half is what
 * the engine's own registers answered, and `answered' says whether they were
 * readable at all -- a base address that names nothing reads as all-ones,
 * which is exactly what a misparsed table produces and is the only cheap way
 * to tell a correct walk from a plausible one.
 */
struct iommu_unit {
	uint16_t	segment;
	uint64_t	register_base;
	uint64_t	register_size;

	/*
	 * This unit is responsible for everything in its segment that no other
	 * unit claims.  Intel spells it INCLUDE_PCI_ALL, AMD spells it a device
	 * entry meaning `all'; there is at most one such unit per segment, and
	 * a machine with none has devices no engine polices.
	 */
	int		covers_rest;

	unsigned	scope_first;	/* index into the flat scope array */
	unsigned	scope_count;

	/*
	 * Where its registers were mapped, uncached, or zero if they were not.
	 *
	 * Kept because stage 2 writes them and stage 3 reads faults out of
	 * them continuously -- mapping afresh each time would rebuild a
	 * mapping that never changes, and the device region is a bump
	 * allocator that never gives anything back.
	 */
	uint64_t	register_va;

	/* ---- answered by the hardware ---- */
	int		answered;
	uint32_t	version;

	/*
	 * The widest address the engine will translate, in bits.  This is the
	 * ceiling on any IOVA stage 2 or 3 hands out, and it is not the CPU's
	 * -- an engine narrower than physical memory is a real configuration
	 * and the reason a buffer can be unreachable to a device that the
	 * processor can address perfectly well.
	 */
	unsigned	address_bits;

	/*
	 * Which page-table depths the engine will walk, as a bitmask of level
	 * counts: bit 3 means a three-level table, bit 4 a four-level one.
	 *
	 * 🔑 This is what decides the page-table format stage 3 has to build.
	 * It is reported now, three stages early, because building the wrong
	 * depth is not a bug that shows up as a wrong answer -- the engine
	 * simply refuses the root pointer, and there is nothing to read.
	 */
	uint32_t	page_levels;

	int		interrupt_remapping;
	int		coherent_walk;	/* the engine's page walks snoop caches */

	/*
	 * The vendor's own capability words, kept raw.
	 *
	 * ⚠️ Deliberately not decoded past the four fields above.  These are
	 * the evidence for that decode, and a reported number that came out of
	 * the same arithmetic as the decode would agree with it whether or not
	 * either was right.
	 */
	uint64_t	vendor_caps[2];
};

/*
 * Memory a device must go on reaching, whatever domain it is put in.
 *
 * ⚠️ These are the reason stage 2 is not simply "map everything for
 * everybody, then stop".  Firmware leaves devices running across the handover
 * -- a USB controller emulating a PS/2 keyboard, a graphics device scanning
 * out of a framebuffer it was given before we existed -- and the region it
 * reads is not in any list this kernel keeps.  Un-mapping it does not produce
 * an error: it produces a machine whose keyboard stops, and a fault log
 * entry, if we have got as far as a fault log.
 *
 * Intel calls them RMRRs and AMD calls them IVMDs, and they mean the same
 * thing.
 */
struct iommu_reserved {
	uint16_t	segment;
	uint64_t	base;
	uint64_t	limit;		/* inclusive, as both tables give it */
	unsigned	scope_first;
	unsigned	scope_count;
};

/*
 * Find the remapping hardware and read what it can do.  Answers the vendor,
 * or IOMMU_NONE.
 *
 * Reads tables and registers, maps the units' register pages, and changes
 * nothing about how any device reaches memory.  Safe to call once; calling it
 * twice would map the registers a second time, so it does not repeat itself
 * and answers what it found the first time.
 *
 * ⚠️ Must be called after acpi_find_cpus(), which is what remembers the root
 * table, and after the pmap is up, because reading a unit's registers means
 * mapping them uncached.
 */
enum iommu_vendor iommu_discover(void);

enum iommu_vendor iommu_vendor(void);

/*
 * The OTHER vendor's table was present too.
 *
 * 🔴 Which vendor a machine has is decided at RUN TIME and by the firmware,
 * not at build time and not by the processor: both readers are always
 * compiled, each asks ACPI for its own table by name, and the one that finds
 * it claims the machine.  ⚠️ The CPU's vendor says nothing about it -- a VT-d
 * engine on an AMD processor is an ordinary thing to emulate and this kernel
 * reads it correctly, which is measured and not assumed.
 *
 * The first reader to succeed ends the search, on the assumption that a
 * machine has one or the other.  This is that assumption looked at: conforming
 * firmware writes one, so a machine with both is describing itself twice and
 * the kernel is believing half of it.  Answering yes does not change which
 * vendor was read -- it says the choice was made where there was something to
 * choose.
 */
int iommu_both_tables(void);

/*
 * ── What the TABLE said about the machine, not what an engine answered ──
 *
 * Kept apart from the per-unit fields deliberately.  These are the firmware's
 * description of the platform, and the per-unit ones are the silicon's
 * description of itself; when they disagree the disagreement is the finding,
 * and a description that had merged them could not have one.
 */

/*
 * How wide an address can travel to a device on this machine.  Zero when no
 * table said.
 *
 * ⚠️ NOT the same number as any unit's address_bits, and the difference is not
 * academic: this is how far a DMA can reach, that is how far an engine can
 * TRANSLATE.  An engine narrower than the platform is a device that can be
 * given an address it will reach and the engine cannot police.
 */
unsigned iommu_platform_address_bits(void);

/*
 * Whether the firmware says the platform can remap interrupts: 1 yes, 0 no,
 * and **-1 when the table does not say**.
 *
 * ⚠️ THREE ANSWERS, because two would make one of them a lie.  Intel's DMAR
 * carries a platform flag for this and AMD's IVRS carries nothing -- so
 * reporting "no" on an AMD machine would be the description asserting
 * something no firmware said, and the cross-check against the engines would
 * fire on every AMD boot for a disagreement that does not exist.
 *
 * A claim about the board, which an engine's own registers may not honour --
 * #457's MSI-X table is policed only where both agree, so both are recorded.
 */
int iommu_platform_interrupt_remapping(void);

/*
 * The firmware asks that x2APIC mode not be used.
 *
 * Recorded because ignoring it on a machine whose interrupt remapping cannot
 * express wide APIC ids gives interrupts that are accepted and delivered
 * nowhere -- a failure with nothing to read, which is the kind this kernel
 * pays most for.
 *
 * ⚠️ Only Intel's table can say this.  AMD's states which wide-id modes are
 * SUPPORTED rather than objecting to them, so there is nothing to translate
 * and the answer on an AMD machine is no -- meaning "nobody objected", not
 * "the firmware approves".
 */
int iommu_x2apic_discouraged(void);

unsigned iommu_unit_count(void);
const struct iommu_unit *iommu_unit(unsigned index);

unsigned iommu_reserved_count(void);
const struct iommu_reserved *iommu_reserved(unsigned index);

const struct iommu_scope *iommu_scope(unsigned index);

/*
 * The table said there were more than this kernel has room to record.
 *
 * 🔴 STAGE 3 MUST REFUSE TO PROMISE ISOLATION WHEN THIS IS SET, and that is
 * the whole reason it exists rather than a panic here.  A unit that was not
 * recorded is an engine nobody programs, which is a set of devices nobody
 * polices -- but discovering that while merely LOOKING at a machine is no
 * reason to stop the machine, and truncating the list without a word would
 * turn a describable limit into an invisible one.
 */
int iommu_truncated(void);

/*
 * The walk consumed the table exactly.
 *
 * 🔑 THE CHEAPEST THING IN THIS FILE, AND THE ONE THAT CHECKS THE MOST.  Both
 * tables are a header followed by variable-length structures, each declaring
 * its own length, and the table's header declares the total.  Those are two
 * independent statements about where the structures end, written by the
 * firmware and by this reader, and they agree only if every structure boundary
 * was understood -- a wrong struct size, a missed variant, a field read at the
 * wrong offset, all land the cursor somewhere other than the end.
 *
 * Which is worth having because the alternative check is "the numbers looked
 * plausible", and a misparsed table produces plausible numbers by
 * construction: they are real bytes from a real table, read one field over.
 *
 * ⚠️ A false answer is not a reason to disbelieve every field -- it is a reason
 * to disbelieve the ones after the first structure this reader did not
 * understand, which is a set nobody can name.  Treat it as fatal to the
 * description rather than to the machine.
 */
int iommu_walk_exact(void);

/*
 * Run each vendor's capability decode against values whose right answers were
 * established somewhere other than this kernel.  Answers non-zero when all of
 * them agree; `ran' and `wrong' may be zero if the counts are not wanted.
 *
 * 🔑 IT NEEDS NO IOMMU, AND THAT IS THE POINT.  The decode is the part most
 * likely to be quietly wrong -- it turns a vendor's bits into numbers that
 * look reasonable whatever they mean -- and it is also the part that a machine
 * without remapping hardware could otherwise say nothing about.  Pure
 * arithmetic over captured words runs on every board.
 *
 * ⚠️ It has already earned this: it caught a decode that read all five bits of
 * Intel's SAGAW when only three of them exist, on the same day it was written,
 * against a case no hardware we can run would ever have produced.
 */
int iommu_decode_check(unsigned *ran, unsigned *wrong);

/*
 * Which engine is responsible for one device, or -1 if none is.
 *
 * 🔴 -1 IS THE ANSWER THAT MATTERS.  A device no engine covers is a device
 * nobody polices, and stage 3 must refuse to promise isolation for it rather
 * than translate for the others and stay quiet about this one.  The question
 * has to be askable before that stage can be honest, which is why it is here
 * and not there.
 *
 * ⚠️ It is a real state and not a malformed table.  QEMU's IVRS under KVM
 * names seven devices, no catch-all, and does not mention the I/O APIC at all
 * -- while the same QEMU under TCG does.  So "covered by nobody" is something
 * a firmware really writes, and something this had better be able to say.
 *
 * ⚠️ A bridge scope covers everything BEHIND it, and resolving that means
 * reading secondary bus numbers out of configuration space.  This does not do
 * that: a device behind a bridge named in a scope is reported as uncovered,
 * which errs toward saying "unpoliced" about something that may be policed.
 * The other direction would be the dangerous one.
 */
int iommu_unit_for(uint16_t segment, uint8_t bus, uint8_t dev, uint8_t func);

/*
 * ── Stage 2a: the tables, built and read back, hardware untouched ────
 *
 * What was built.  All zero when nothing was, which is the state on a machine
 * with no remapping hardware and on one where the frames could not be found.
 */
struct iommu_tables {
	uint64_t	root;		/* device table (AMD) or root table   */
	uint64_t	root_bytes;
	uint64_t	command;	/* AMD's command buffer, else zero    */
	uint64_t	event;		/* AMD's event log, else zero         */
	unsigned	devices;	/* device entries actually written    */
	unsigned	contexts;	/* Intel's per-bus tables, else zero  */
	unsigned	frames;		/* what it cost, in 4K frames         */
	int		verified;	/* every entry read back as written   */
};

/*
 * Build the translation tables with every device passing through, and read
 * them back.  Answers non-zero when they are built and verified.
 *
 * 🔴 NOT ONE REGISTER IS WRITTEN.  A machine that booted before this boots
 * after it, unchanged, which is why it lands separately from the step that
 * enables translation -- that step is the first in #432 that can stop a
 * machine, and it should not arrive in the same commit as the arithmetic it
 * depends on.
 *
 * ⚠️ Reading back is not a formality.  These structures are written once and
 * read only by hardware, so a wrong entry produces no wrong answer: it
 * produces a device that is not policed, or one that faults on everything,
 * and neither says which entry was wrong.  The read-back is the last moment
 * anything can be checked cheaply.
 */
int iommu_build_passthrough(void);

const struct iommu_tables *iommu_tables(void);

/*
 * ── Stage 2b: point the engines at those tables and let them run ─────
 *
 * 🔴 THE FIRST THING IN #432 THAT CAN STOP A MACHINE, and therefore the first
 * that is asked for rather than assumed: it happens only when `-I' is on the
 * boot command line.  A default boot builds the tables and enables nothing, so
 * a machine that cannot survive this can still be booted to find out why.
 *
 * 🔑 And the flag is what makes the cost measurable at all.  #432 asks for the
 * performance cost to be measured rather than assumed, and measuring it means
 * the same kernel, the same image and the same boot, run twice.  A build-time
 * switch would have given two kernels and a comparison between them.
 *
 * Every device passes through, so a machine on which this works behaves
 * exactly as it did -- which is the point of stage 2, and also why a machine
 * on which it does NOT work says so loudly rather than subtly.
 */
int iommu_enable_passthrough(void);

/* Whether translation is on, and what the hardware says about it. */
int iommu_translating(void);

/*
 * Whether an address width, in bits, is one every unit can reach.
 *
 * Asked rather than derived by the caller because the answer is the MINIMUM
 * over the units and not the first one's: a buffer is reachable by a device
 * only if the engine covering that device can address it, and a machine may
 * have engines of different widths.
 */
int iommu_address_bits_ok(unsigned bits);

/*
 * ── Stage 3b: a translation table, and the walk that reads it back ───
 *
 * A domain is a page table and the id the engine knows it by.  Stage 2 gave
 * every device the same one and let it all pass through; this is the structure
 * that can say something different about one address than about another, and
 * it is the whole of what "isolation" eventually means.
 *
 * 🔴 THE VENDOR IS IN THE DOMAIN, not taken from the machine.  A page table is
 * arithmetic and memory, and asking the machine which format to build would
 * mean the AMD format is only ever built on AMD -- so the format that is wrong
 * is the one nobody here can run, which is the one nobody would find out about.
 * With the vendor named, both are built and walked on every boot.
 */
struct iommu_domain {
	uint64_t		root;	/* the top-level table, physical    */
	enum iommu_vendor	vendor;
	unsigned		levels;	/* how deep the walk starts         */
	uint16_t		id;	/* what an engine would call it     */
	unsigned		frames;	/* what the table has cost so far   */
	unsigned		pages;	/* 4 KiB pages mapped into it       */

	/*
	 * The next address this domain will hand out (#432 stage 3e).
	 *
	 * 🔴 A BUMP AND NO FREE LIST, deliberately.  An IOVA space is 2^39
	 * bytes at the shallowest depth this kernel will build, and a driver
	 * that allocated a buffer a second would take nine thousand years to
	 * exhaust it -- so reusing an address buys nothing and costs the one
	 * property worth having here: an address that is never handed out
	 * twice cannot be a stale mapping mistaken for a live one.  Running
	 * out is REPORTED rather than wrapped.
	 */
	uint64_t		next_iova;
};

/*
 * An empty domain: one top-level table, nothing mapped, everything faulting.
 *
 * ⚠️ "Everything faulting" is true on both vendors here and it is NOT the same
 * statement as stage 2's -- a zeroed page-table entry denies on both, while a
 * zeroed DEVICE table entry denies on one and forwards on the other.  The
 * asymmetry is in the tables that name devices, not in the tables that map
 * addresses.
 */
int iommu_domain_create(struct iommu_domain *d, enum iommu_vendor vendor,
			uint16_t id, unsigned levels);

/*
 * Map `size' bytes at `iova' onto `pa', building whatever levels are missing.
 * Answers non-zero when the whole range is mapped.
 *
 * ⚠️ Fails PART WAY when it runs out of frames, and says so by answering zero
 * rather than by undoing what it did.  A domain that is not attached to any
 * device translates for nobody, so a half-built one is inert; the caller that
 * would attach it is the one that has to check, and stage 3c is where that
 * matters.
 */
int iommu_domain_map(struct iommu_domain *d, uint64_t iova, uint64_t pa,
		     uint64_t size, int read, int write);

/*
 * Translate one address the way the engine would: from the root, indexing each
 * level with the address's own bits, deciding from each ENTRY whether it is a
 * road or a destination.  Answers zero when there is no translation.
 *
 * 🔑 THIS IS THE ONLY THING THAT CAN CONTRADICT THE BUILDER.  These tables are
 * written once and read by hardware alone, so a builder that gets a field
 * wrong produces no wrong answer -- it produces a device reaching memory that
 * was never meant for it, and nothing on the machine says a word.  A walk that
 * starts from the root physical address and remembers nothing else is the one
 * reader available before the hardware becomes the reader.
 *
 * ⚠️ It must therefore not be the builder run backwards.  It shares the
 * decoders with it, and those are checked against bit patterns copied out of
 * the specifications rather than against the encoders -- which is what stops
 * the two halves from agreeing on the same mistake.
 */
int iommu_domain_walk(const struct iommu_domain *d, uint64_t iova,
		      uint64_t *pa, int *read, int *write);

/*
 * Build one domain per vendor, map a range into each, and walk every page of
 * it back.  `walked' counts the walks, `wrong' the ones that disagreed.
 *
 * 🔴 AND IT ABLATES ITSELF.  After the range verifies, it damages one directory
 * entry in each of the ways that vendor's format allows, walks again, and
 * requires the answer to CHANGE -- then restores it and requires it to change
 * back.  A check that has never been seen to fail is a check nobody has tested,
 * and this one is tested on every boot rather than once by whoever wrote it.
 */
int iommu_domain_check(unsigned *walked, unsigned *wrong);

/*
 * ── Stage 3d: what the engine says when it refuses a DMA ─────────────
 *
 * 🔴 THIS IS THE HALF OF #432 THAT MAKES THE OTHER HALF WORTH HAVING.  A
 * domain that blocks and says nothing turns one failure mode into another: a
 * driver that reached memory it should not have becomes a driver whose
 * transfer quietly did not happen, and the second is harder to diagnose than
 * the first, not easier.  The issue asks for "a diagnosable event, not
 * silence", and this is where the event is read.
 *
 * ⚠️ And it is a POLL, not an interrupt, on purpose for now.  An engine can
 * raise a message-signalled interrupt when it records a fault, and it should
 * -- but a reporter that only ever runs from an interrupt handler cannot be
 * asked "has anything gone wrong since the last time I asked", which is the
 * question a self-test needs and the question a driver's own retry path needs.
 * The interrupt, when it lands, will call this.
 */
enum iommu_fault_kind {
	IOMMU_FAULT_UNKNOWN = 0,
	IOMMU_FAULT_PAGE,		/* no translation, or no permission  */
	IOMMU_FAULT_ENTRY,		/* the device's own entry is unusable */
	IOMMU_FAULT_HARDWARE		/* the engine failed to read a table  */
};

/*
 * One refusal, as the engine recorded it.
 *
 * 🔑 `reason' is the VENDOR'S OWN CODE, kept raw, and `kind' is the reading of
 * it.  Both, because the two answer different questions: the raw code is what
 * a specification can be looked up against, and the kind is what a caller can
 * branch on without knowing which vendor it is running on.  A structure that
 * carried only the reading would have thrown away the evidence for it.
 */
struct iommu_fault {
	uint64_t		address;   /* what the device asked for      */
	uint16_t		source;	   /* bus:dev.func that asked         */
	uint16_t		domain;	   /* or IOMMU_FAULT_NO_DOMAIN        */
	uint8_t			reason;	   /* the vendor's own code, raw      */
	uint8_t			kind;	   /* enum iommu_fault_kind           */
	uint8_t			write;	   /* 1 a write, 0 a read             */
	uint8_t			vendor;	   /* which encoding `reason' is in   */
};

/*
 * Intel's fault record does not carry one, AMD's does.
 *
 * ⚠️ A sentinel and not zero, because zero is a domain id and one of ours:
 * IOMMU_DOMAIN_PASSTHROUGH is the domain everything starts in, and reporting
 * "domain 0" for a record that said nothing would name the wrong culprit in
 * exactly the case that matters.
 */
#define	IOMMU_FAULT_NO_DOMAIN	0xFFFFu

/*
 * How many refusals this boot has seen, and the last IOMMU_FAULT_LOG of them.
 *
 * 🔑 A COUNT AND A RING, not one or the other.  The count is what a self-test
 * compares before and after, and it must not saturate; the ring is what says
 * WHICH, and it must not grow without bound in a kernel that has no allocator
 * running when the first fault can arrive.  A design with only the ring cannot
 * tell "none" from "more than fits", and that is the difference between a
 * driver that is fine and a driver that is faulting on every transfer.
 */
#define	IOMMU_FAULT_LOG		16

unsigned iommu_fault_count(void);
const struct iommu_fault *iommu_fault(unsigned index);	/* oldest first */
unsigned iommu_fault_logged(void);			/* how many the ring holds */

/*
 * Drain every engine's fault registers into the log.  Answers how many new
 * ones were found.
 *
 * ⚠️ DRAIN, not peek: the records are cleared as they are read, because an
 * engine with every record full stops recording and sets an overflow bit
 * instead -- so a reader that left them in place would see the first sixteen
 * faults of the boot forever and none of the ones being caused right now.
 */
unsigned iommu_fault_poll(void);

/*
 * An engine ran out of fault records before anyone drained them.
 *
 * Reported rather than folded into the count, because the two are different
 * facts: the count says how many were read, and this says that the number is
 * a floor rather than a total.
 */
int iommu_fault_overflowed(void);

/*
 * Decode fault records whose right answers were established from the
 * specifications, on every boot and on every board.
 *
 * 🔑 Same argument as iommu_decode_check(), and a sharper one.  A fault record
 * is read exactly when something has already gone wrong, which is the worst
 * moment to find out that the reader has the source id in the wrong bits --
 * and a machine that never faults never exercises it at all.  The bit patterns
 * below are written from the figures, so the decode is tested on a machine
 * with no remapping hardware and on one that has never refused anything.
 */
int iommu_fault_decode_check(unsigned *ran, unsigned *wrong);

/*
 * ── Stage 3d: a domain of its own, for one device ────────────────────
 *
 * 🔴 THIS IS WHERE #432 STOPS BEING A DESCRIPTION.  Everything before it built
 * tables, read them back and let every device pass through -- true statements
 * about arithmetic that changed nothing about what a device can reach.  From
 * here a device reaches what it was GRANTED and nothing else, which is the
 * property the userspace driver model has been claiming all along.
 *
 * The model is deliberately small: a device is named by its bus/device/
 * function, its domain is created the first time anything is granted to it,
 * and the grant is what moves it off pass-through.  There is no attach call in
 * this header, because an attached domain with nothing in it is a device that
 * has just lost its memory -- and an interface that lets a caller do that in
 * two steps is an interface that will be half-done somewhere.
 */
#define	IOMMU_MAX_DEVICE_DOMAINS	16

/*
 * Where a domain's addresses start.
 *
 * 🔴 FOUR GIBIBYTES, AND THE VALUE IS PART OF THE POINT.  An IOVA has to be a
 * number the driver cannot mistake for the physical address of its buffer, and
 * the cheapest way to make that visible in a log is to put the window
 * somewhere no buffer of ours lives.  It also has to fit the shallowest page
 * table this kernel will build -- three levels reach 39 bits, and this needs
 * 33.
 *
 * ⚠️ NOT ZERO, and not near it.  A driver that programs a device with an
 * uninitialised field programs it with zero, and an IOVA space that begins at
 * zero would translate that instead of faulting on it.
 */
#define	IOMMU_IOVA_BASE		0x0000000100000000ULL

/*
 * Make `size' bytes of physical memory at `pa' reachable by `bdf', and answer
 * the address the DEVICE must be programmed with.  Non-zero on success.
 *
 * 🔴 THE ADDRESS THAT COMES BACK IS NOT THE PHYSICAL ONE, and that is this
 * stage's whole content (#432 stage 3e).  #457 closed with the clause "a
 * userspace driver server does DMA to a buffer it does not know the physical
 * address of", and until now it knew by construction.  The IOVA is meaningless
 * outside this device's domain: another device programmed with it reaches
 * nothing, and the processor cannot use it at all.
 *
 * 🔑 So the isolation stops depending on the driver being well behaved in a
 * second way.  Confining a device to what it was granted stops it reaching
 * elsewhere BY ACCIDENT; not telling it where its memory is stops it doing so
 * ON PURPOSE, because it no longer holds a number that means anything to any
 * other device.
 *
 * ⚠️ Answering zero can mean the device is now in a domain that is missing
 * part of what was asked for.  The caller must treat a failed grant as a
 * failed allocation -- there is no half-granted buffer that is safe to hand to
 * a device, and the fault it takes would name an address the driver believes
 * it owns.
 */
int iommu_grant(uint16_t bdf, uint64_t pa, uint64_t size, int read, int write,
		uint64_t *iova_out);

/*
 * The same for pages that are not physically contiguous: `n' frames, each
 * mapped at consecutive addresses starting from the one answered.
 *
 * 🔑 ONE CALL AND ONE CONTIGUOUS WINDOW, not n grants.  A scatter-gather
 * buffer is scattered in PHYSICAL memory and there is no reason for it to be
 * scattered in the device's -- so what the driver receives is one address and
 * a length, and the scattering stays a fact about the machine rather than
 * something every driver has to carry.  It is also what keeps the record of
 * the grant one entry instead of a thousand.
 */
int iommu_grant_pages(uint16_t bdf, const uint64_t *pa, unsigned n,
		      int read, int write, uint64_t *iova_out);

/*
 * This device must be programmed with physical addresses: map its grants at
 * the address the memory is really at.  Answers non-zero when the domain was
 * opened that way.
 *
 * 🔴 STILL CONFINED, and that is the whole distinction.  An identity domain
 * contains only what was granted, so every other address in the machine faults
 * for this device exactly as before -- what is lost is that the driver knows
 * where its buffer is.  #432 stage 3d is kept and stage 3e is given up, for a
 * device that cannot accept what 3e hands out.
 *
 * ⚠️ Before the first grant only.  A domain that already holds translated
 * buffers cannot become an identity one without moving them, and moving them
 * means a device reading through addresses that changed under it.
 */
int iommu_domain_identity(uint16_t bdf);

/*
 * Take this device out of its domain and leave it reaching NOTHING.
 *
 * 🔴 BLOCKED AND NOT PASS-THROUGH.  The caller is a revocation: something has
 * just decided this driver may no longer drive this device.  Restoring the
 * pass-through entry the device started in would make that revocation GIVE it
 * all of memory -- a widening dressed as a withdrawal, and one somebody
 * trusted.
 *
 * 🔑 THIS IS WHAT A MATERIALISED CAPABILITY OWES.  A capability that is
 * checked on every use is revoked by refusing the next one; a capability that
 * is turned into a MAPPING -- which is what a domain is, and what a page table
 * has always been -- can only be revoked by tearing the mapping down.  Without
 * this call the token could be revoked and the device would keep its reach,
 * which is the difference between a capability system and a capability-shaped
 * one.
 *
 * ⚠️ The page tables are left allocated.  Nothing walks them once the device's
 * entry stops pointing at them, and freeing them would mean establishing that
 * no engine holds a cached translation through them -- which the invalidation
 * here does establish, and which a later reuse would have to establish again.
 */
int iommu_domain_release(uint16_t bdf);

/*
 * Take a granted range back, naming it by the PHYSICAL address it was granted
 * from.  Answers non-zero when the range was there and is now unreachable.
 *
 * 🔑 BY THE PHYSICAL ADDRESS AND NOT BY THE IOVA, because the caller is a free
 * path and what a free path has is the memory: device_dma_free is handed a
 * kernel address, extracts the frame under it, and knows nothing about what
 * any device was told.  Asking it for the IOVA would mean every caller keeping
 * a table the kernel already has.
 *
 * ⚠️ The domain STAYS, and the device stays in it.  A device whose last buffer
 * is freed is a device between transfers, not a device that should go back to
 * reaching all of memory -- and re-attaching pass-through under it would make
 * every free a window.
 */
int iommu_revoke(uint16_t bdf, uint64_t pa, uint64_t size);

/*
 * The domain a device is in, or null when it is still passing through.
 *
 * 🔑 Null IS the answer for most devices, and it is the one worth reporting: a
 * device with no domain is a device this kernel is not policing, which is what
 * #432 exists to stop being invisible.
 */
const struct iommu_domain *iommu_domain_of(uint16_t bdf);

/* How many devices have been taken off pass-through. */
unsigned iommu_domain_count(void);

/*
 * Drain the engines and print anything new.
 *
 * 🔴 THIS IS "a diagnosable event, not silence" (#432), and it is a POLL
 * because the alternative is not ready.  An engine can raise a
 * message-signalled interrupt when it records a fault -- VT-d through
 * FECTL/FEDATA/FEADDR, AMD through its event-log interrupt -- and it should,
 * because a poll reports late and a fault that arrives while a driver is
 * spinning on a transfer is exactly the one it needs now.  What a poll does
 * give is that no refusal goes unreported, which is the property worth having
 * first.
 *
 * ⚠️ Answers how many were printed, and prints nothing when there is nothing.
 * Cheap to call: one uncached register read per engine when no fault is
 * pending, and none at all when no device is in a domain.
 */
unsigned iommu_fault_report(void);

/*
 * How many refusals this device has been given, and the last address it was
 * refused.  Answers zero when it has never been refused.
 *
 * 🔑 PER DEVICE, because that is the question a DRIVER asks.  A transfer that
 * failed has two ordinary explanations -- the device is broken, or the
 * driver programmed an address it was never granted -- and they are told apart
 * by nothing the device reports.  This is the second one, answered.
 */
unsigned iommu_faults_for(uint16_t bdf, uint64_t *last_address);

/*
 * Whether a domain could be given to a device at all on this machine.
 *
 * ⚠️ Asked rather than assumed, because there are four separate ways for the
 * answer to be no and each of them is a real machine: no engine at all, an
 * engine that did not answer, a description that was truncated, and
 * translation not turned on.  A grant that failed for one of those is not a
 * bug in the caller, and a caller that cannot tell them apart will report it
 * as one.
 */
int iommu_can_isolate(void);

#endif	/* _X86_64_CPU_IOMMU_H_ */
