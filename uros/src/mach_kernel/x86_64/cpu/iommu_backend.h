/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 *	iommu_backend.h — the side of <cpu/iommu.h> that WRITES (#432).
 *
 *	Separate from the public header for one reason, and it is not tidiness:
 *	a file that cannot see iommu_record_unit() cannot call it.  The
 *	description in <cpu/iommu.h> is meant to be filled by exactly two files
 *	and read by everything else, and saying so in a comment is a thing that
 *	erodes -- a second header is a thing that does not.
 *
 *	Include this only from cpu/iommu.c and the vendor readers.
 */

#ifndef	_X86_64_CPU_IOMMU_BACKEND_H_
#define	_X86_64_CPU_IOMMU_BACKEND_H_

#include <cpu/iommu.h>

/*
 * Each reader answers whether it found its table, having recorded what was in
 * it.  Answering zero must leave nothing recorded: iommu_discover() tries them
 * in turn, and a reader that gave up halfway would leave the next one's
 * description mixed with its own.
 */
int iommu_vtd_read(void);		/* the DMAR  */
int iommu_amd_read(void);		/* the IVRS  */

/*
 * ── The decode, separated from the reading ────────────────────────────
 *
 * Each vendor's capability words turned into the four things <cpu/iommu.h>
 * holds.  Pure arithmetic: no register is touched, nothing is recorded, and
 * the same word always gives the same answer.
 *
 * 🔑 SEPARATED SO THAT IT CAN BE CHECKED WITHOUT THE HARDWARE.  These are what
 * iommu_decode_check() runs against captured values whose right answers are
 * known -- including one read off a real AMD machine, whose decode another
 * operating system published beside it.  A decode that only ever runs on the
 * machine it was written for is a decode nobody can contradict.
 *
 * An `address_bits' of zero means the register said something these could not
 * read -- a reserved encoding -- and is a refusal, not a width.
 */
void iommu_vtd_decode(uint64_t cap, uint64_t ecap,
		      unsigned *address_bits, uint32_t *page_levels,
		      int *interrupt_remapping, int *coherent);

void iommu_amd_decode(uint64_t efr, uint64_t control,
		      unsigned *address_bits, uint32_t *page_levels,
		      int *interrupt_remapping, int *coherent);

/*
 * The same words asked a second question: what remapping interrupts would need
 * from this engine (#598).  Pure, and checked by iommu_decode_check() against
 * the same cases as the decode above.
 *
 * Separate from it rather than four more arguments, because the two answer for
 * different stages and fail in different ways: a wrong width refuses a root
 * pointer, a wrong one of these writes an entry the engine will not use.
 */
void iommu_vtd_interrupt_decode(uint64_t ecap,
				struct iommu_interrupt_caps *out);
void iommu_amd_interrupt_decode(uint64_t efr,
				struct iommu_interrupt_caps *out);

/*
 * ── The entries stage 2 will write ───────────────────────────────────
 *
 * Pure encoders, for the same reason the decoders are pure: they can be
 * checked against known bit patterns before anything is allocated, let alone
 * before any engine is told about them.
 *
 * 🔴 THE TWO VENDORS' EMPTY ENTRIES MEAN OPPOSITE THINGS.  Intel's not-present
 * context entry BLOCKS; AMD's invalid device table entry FORWARDS WITHOUT
 * TRANSLATION.  So a table allocated and zeroed is a closed door on one
 * machine and an open one on the other, and the paired `blocked' encoders
 * exist so that neither is ever left to a zeroing.
 */
void iommu_amd_dte_passthrough(uint16_t domain, uint64_t out[4]);
void iommu_amd_dte_blocked(uint16_t domain, uint64_t out[4]);

/*
 * ── Building the tables (stage 2a) ───────────────────────────────────
 *
 * Allocate this vendor's translation structures and fill them so that every
 * device passes through, then read them back.  Answers non-zero when the
 * tables are built and verified.
 *
 * 🔴 PROGRAMS NO HARDWARE.  Not one register is written, so a machine that
 * booted before this still boots after it -- which is what makes it worth
 * landing on its own.  Pointing an engine at these and enabling translation is
 * stage 2b, and it is the first step in #432 that can stop a machine.
 *
 * ⚠️ The frames arrive ZEROED, and on one of the two vendors that is already
 * the wrong answer: an all-zero AMD device table entry forwards without
 * translation.  Neither builder may rely on the allocator's zeroing to mean
 * anything; both write every entry they intend.
 */
int iommu_vtd_build(void);
int iommu_amd_build(void);

/*
 * Point the engines at those tables and enable translation, everything
 * passing through.  Answers non-zero when the hardware confirms it is on.
 *
 * ⚠️ Confirmation is read from the hardware and not inferred from the writes
 * succeeding.  Both vendors report their own state -- Intel in a status
 * register, AMD by reading back the control -- and a write that was accepted
 * and ignored is exactly the failure this cannot afford to call success.
 */
int iommu_vtd_enable(void);
int iommu_amd_enable(void);

/* Where a unit's registers were mapped, so stage 2 and 3 need not remap. */
void iommu_record_registers(unsigned index, uint64_t va);

/*
 * The domain every device is put in while everything passes through.
 *
 * ⚠️ One, and not zero.  Intel's specification reserves domain id zero on
 * hardware that reports Caching Mode, so zero is the one value that is not
 * always a domain -- and a table built with it would work on most machines.
 */
#define	IOMMU_DOMAIN_PASSTHROUGH	1

/* What a builder produced, for <cpu/iommu.h>'s struct iommu_tables. */
void iommu_record_tables(uint64_t root, uint64_t root_bytes,
			 uint64_t command, uint64_t event,
			 unsigned devices, unsigned contexts, unsigned frames);

/*
 * #598: build the interrupt tables, every entry written refusing and read
 * back, and record them, for <cpu/iommu.h>'s iommu_build_interrupt_tables().
 * Answers non-zero when everything was built and read back as written.
 */
int iommu_vtd_irt_build(void);
int iommu_amd_irt_build(void);
void iommu_record_interrupt_tables(const struct iommu_interrupt_tables *t);

/*
 * ── Stage 3: page-table entries ──────────────────────────────────────
 *
 * 🔴 TWO ENCODERS BECAUSE THEY ARE TWO FORMATS, not for symmetry.  The bit
 * positions agree almost everywhere -- present low, address in 51:12,
 * permissions -- and the two disagree about what a table IS: an AMD entry
 * carries the LEVEL of the table it points at, so a directory may skip levels,
 * and an Intel entry does not, because there the level is the depth.
 *
 * That is exactly the kind of difference that gets flattened by whoever writes
 * the second one from the first, and the result would be an AMD table whose
 * every directory claimed to be a translation.
 *
 * ⚠️ Both vendors AND permission down the walk, so a directory needs its
 * permissions set for anything below to be reachable, and the place to deny a
 * range is the range and not the road to it.
 */
uint64_t iommu_amd_pte(uint64_t pa, int read, int write);
uint64_t iommu_amd_pde(uint64_t next_table_pa, unsigned next_level);

uint64_t iommu_vtd_ss_pte(uint64_t pa, int read, int write);
uint64_t iommu_vtd_ss_pde(uint64_t next_table_pa);

/*
 * ── Stage 3b: reading one entry back ─────────────────────────────────
 *
 * One step of a walk: what an engine learns from an entry it has just fetched
 * out of a table at `level'.
 *
 * `level' here is where the walk goes NEXT, and zero means it does not go
 * anywhere -- the entry is the translation, of the page size that belongs to
 * the level it was found in.
 */
struct iommu_pt_step {
	uint64_t	next;	/* the table below, or the page itself  */
	unsigned	level;	/* level to index next; 0 = this is it   */
	int		read;
	int		write;
};

/*
 * Bytes one entry at `level' covers, which is also the page size of a
 * translation found there.  Nine address bits per level above the lowest
 * twelve -- the one thing the two vendors agree about completely, both having
 * taken it from the processor's own paging.
 */
static inline uint64_t iommu_level_span(unsigned level)
{
	return 1ULL << (12u + 9u * (level - 1u));
}

/*
 * Decode one entry, answering zero when it translates nothing.
 *
 * 🔴 WRITTEN FROM THE FIGURES, NOT FROM THE ENCODERS ABOVE.  Builder and walker
 * share these, so a decoder derived by inverting an encoder would let the two
 * halves agree on the same wrong bit and call it verified.  What keeps them
 * apart is that iommu_decode_check() feeds these literal words copied out of
 * the specifications -- including words no encoder here produces.
 *
 * 🔴 AND "NOTHING" IS TWO DIFFERENT STATES.  AMD's PR bit is a separate
 * question from its permissions, so an AMD entry can be present and deny
 * everything; Intel has no present bit at all, and Rev 5.20 §3.7 makes R and W
 * both zero mean the entry "is used neither to reference another
 * paging-structure entry nor to map a page".  The same idea, spelt one way that
 * distinguishes denial from absence and one way that cannot.
 */
int iommu_amd_pt_decode(uint64_t entry, unsigned level,
			struct iommu_pt_step *step);
int iommu_vtd_pt_decode(uint64_t entry, unsigned level,
			struct iommu_pt_step *step);

/*
 * ── The ways an entry can be wrong, so the walk can prove it notices ──
 *
 * Each vendor names its own, because they are not the same mistakes: what makes
 * a directory look like a page is a cleared Next Level on one machine and a set
 * page-size bit on the other, and skipping a level is not expressible at all on
 * Intel.
 *
 * Answers zero when this format cannot make that mistake -- which is a result
 * and not a failure. 🔑 A format that cannot express a mistake cannot make it,
 * and asking each vendor what it can get wrong is what keeps the missing case
 * a written-down absence rather than one nobody thought to run.
 */
#define	IOMMU_ABLATE_ROAD_IS_DESTINATION	1	/* directory read as a page  */
#define	IOMMU_ABLATE_DENY_ON_THE_ROAD		2	/* write denied on the way   */
#define	IOMMU_ABLATE_SKIP_A_LEVEL		3	/* over bits that are not 0  */

int iommu_amd_pt_ablate(unsigned kind, unsigned level, uint64_t *entry);
int iommu_vtd_pt_ablate(unsigned kind, unsigned level, uint64_t *entry);

/*
 * A directory pointing at a table of level `next_level' when the level below
 * this one is not that -- the level skipping AMD's format has and Intel's does
 * not.  Answers zero where the format cannot express one.
 *
 * 🔴 THE ONLY LEGAL SKIP ANYTHING HERE BUILDS, and it exists to be walked.  A
 * walk that refused every skip would pass an ablation that skips wrongly, pass
 * every table this kernel builds -- none of which skip -- and be wrong about
 * the one thing the two formats genuinely disagree on.  Proving the refusal
 * needs a case that must be ACCEPTED beside it.
 */
int iommu_amd_pt_skip(uint64_t next_table_pa, unsigned next_level,
		      uint64_t *entry);
int iommu_vtd_pt_skip(uint64_t next_table_pa, unsigned next_level,
		      uint64_t *entry);

void iommu_vtd_root_entry(uint64_t context_table_pa, uint64_t out[2]);
void iommu_vtd_context_passthrough(uint16_t domain, unsigned levels,
				   uint64_t out[2]);
void iommu_vtd_context_blocked(uint64_t out[2]);

/*
 * ── Stage 3c: the entry that points a device at a table ──────────────
 *
 * The counterpart of the pass-through pair above, and NOT a variation on it:
 * on one vendor the pass-through entry ignores the root pointer entirely, and
 * on the other the width field means a different thing once translation is on.
 * Written from the tables rather than by adding a pointer to what is there.
 *
 * `levels' is THIS DOMAIN'S depth, which under pass-through was the engine's
 * deepest.  Same argument, opposite value.
 */
void iommu_vtd_context_domain(uint16_t domain, unsigned levels,
			      uint64_t root_pa, uint64_t out[2]);
void iommu_amd_dte_domain(uint16_t domain, unsigned levels, uint64_t root_pa,
			  uint64_t out[4]);

/*
 * The range both vendors reserve for interrupts, which no translation may ever
 * produce.
 *
 * 🔴 STATED SEPARATELY BY BOTH, WHICH IS WHY IT IS HERE AND NOT IN ONE READER.
 *
 *	Intel Rev 5.20 §3.15: "Software must not program paging-structure
 *	entries to remap any address to the interrupt address range."
 *
 *	AMD Rev 3.11 §2.1.4.2: "The IOMMU should not be configured such that an
 *	address translation results in a special address such as the interrupt
 *	address range."
 *
 * ⚠️ And the same two sections rule out the obvious way to demonstrate this
 * issue's whole point.  A single-DWORD write to FEEx_xxxxh is an INTERRUPT
 * request on both vendors and "not subjected to DMA remapping (even if
 * translation structures specify a mapping for this range)" -- so the MSI
 * doorbell #457 already makes a device write cannot be blocked by any domain
 * built here.  That hole is interrupt remapping's to close, not this one's.
 */
#define	IOMMU_INTERRUPT_RANGE_BASE	0xFEE00000ULL
#define	IOMMU_INTERRUPT_RANGE_LIMIT	0xFEEFFFFFULL

/*
 * ── Stage 3d: reading a refusal out of an engine ─────────────────────
 *
 * The two vendors do not merely use different bit positions here: Intel keeps
 * a small array of REGISTERS that hold the most recent faults and stop
 * recording when they are full, and AMD writes variable-meaning ENTRIES into a
 * ring in memory that it wraps around.  So one is read by scanning registers
 * and clearing the ones that are set, and the other by following a head
 * pointer -- and there is no shape both fit into that is not a lie about one
 * of them.
 *
 * What they do share is the decode's inputs and outputs: two 64-bit words in,
 * one struct iommu_fault out, no register access.  That is deliberately the
 * part that is pure, because it is the part that can be checked on a machine
 * that has never faulted -- see iommu_fault_decode_check().
 *
 * Answers non-zero when the words described a fault at all.  Zero means the
 * record was empty, which for Intel is the ordinary state of a register nobody
 * has faulted into.
 */
int iommu_vtd_fault_decode(uint64_t lo, uint64_t hi, struct iommu_fault *out);
int iommu_amd_fault_decode(uint64_t lo, uint64_t hi, struct iommu_fault *out);

/*
 * #599: where a drain puts what it finds.  Complete only in iommu_fault.c;
 * a live one exists only while iommu_fault_lock is held, and recording into
 * it asserts that.
 */
struct iommu_fault_sink;

/* One decoded refusal. */
void iommu_fault_sink_record(struct iommu_fault_sink *s,
			     const struct iommu_fault *f);

/*
 * The engine may have discarded refusals in this drain; `why' says how.
 * Counted, never cleared: it is the "floor" every answer carries.
 */
#define	IOMMU_LOST_OVERFLOW	0x1u	/* the engine's own flag */
#define	IOMMU_LOST_FULL		0x2u	/* its log was full, or filled while
					   it was read */
#define	IOMMU_LOST_EMPTY	0x4u	/* it logged an entry it never wrote */
#define	IOMMU_LOST_STOPPED	0x8u	/* it was not logging */
void iommu_fault_sink_lost(struct iommu_fault_sink *s, unsigned unit,
			   unsigned why);

/*
 * #599: whether the unit's event log was found not running in this drain.
 * One drain that finds it stopped restarts it; the next that still finds it
 * stopped calls it blind, and every drain while blind counts a loss, until one
 * finds it running again.  The states live in the ledger, not in the vendor.
 */
void iommu_fault_sink_stopped(struct iommu_fault_sink *s, unsigned unit,
			      int stopped);

/*
 * Drain one engine's records into `s'.  Answers how many were found.  Called
 * only from iommu_fault.c, with iommu_fault_lock held.
 *
 * ⚠️ The unit is passed by index and not by pointer because both readers need
 * its capability words as well as its register mapping, and a caller that
 * passed only the base address would have to re-derive where the records are.
 */
unsigned iommu_vtd_fault_drain(unsigned unit, struct iommu_fault_sink *s);
unsigned iommu_amd_fault_drain(unsigned unit, struct iommu_fault_sink *s);

/*
 * #599: each drain is a core over a view of the registers it touches, so the
 * cores can run against fabricated engines at boot as well as live ones.  On
 * a live engine a write-one-to-clear goes to the register it reads
 * (status_w1c == status, fsts_w1c == fsts); a fabricated one gives them
 * separate words, so reads stay put and the check sees what was written.
 * CONTROL is written by the restart twice, EventLogEn off and then on, and
 * ends as it began, so a fabricated engine also keeps a log of the writes in
 * order (control_log, IOMMU_AMD_CONTROL_LOG deep; NULL live): the check sees
 * both writes and which came first (found in review: two separate words saw
 * both, but not the order, and the order is the restart).
 */
#define	IOMMU_AMD_CONTROL_LOG	4

struct iommu_amd_evtlog {
	volatile uint64_t	*head, *tail, *status, *status_w1c, *control;
	uint64_t		*control_log;	/* fabricated only */
	unsigned		*control_logged;
	volatile uint8_t	*log;
	unsigned		 bytes;
};
struct iommu_vtd_records {
	volatile uint32_t	*fsts, *fsts_w1c;
	volatile uint8_t	*records;	/* 16 bytes each */
	unsigned		 count;
};
int iommu_amd_evtlog_of(unsigned unit, struct iommu_amd_evtlog *v);
unsigned iommu_amd_evtlog_drain(const struct iommu_amd_evtlog *v,
				unsigned unit, struct iommu_fault_sink *s);
int iommu_vtd_records_of(unsigned unit, struct iommu_vtd_records *v);
unsigned iommu_vtd_records_drain(const struct iommu_vtd_records *v,
				 unsigned unit, struct iommu_fault_sink *s);

/*
 * ── #598: the entry an interrupt is remapped through ─────────────────
 *
 * Rev 5.20 §9.9 Figure 9-9, 128 bits; Rev 3.11 §2.2.5.1 Figure 15, 32 bits in
 * the basic format, which is the only one this kernel writes.  Pure, like every
 * encoder above, and read back by decoders WRITTEN FROM THE FIGURES rather than
 * by inverting the encoders, for the reason the page-table pair gives.
 *
 * 🔑 HERE BOTH VENDORS' EMPTY ENTRIES REFUSE.  Intel's P=0 and AMD's RemapEn=0
 * both block the message and report it -- the opposite of the device table
 * entry, where AMD's zero forwards.  So a zeroed interrupt table is closed on
 * both, and the danger runs the other way: an entry still valid for a device
 * that has since been given something else.
 *
 * Physical destination, fixed delivery, no redirection hint: the choices
 * ioapic_route() and device_md_msi_register() already make, for the reason
 * ioapic_route() gives.
 */
struct iommu_irte {
	uint8_t		vector;
	uint32_t	destination;	/* an APIC id */
	int		level;		/* level-triggered; Intel's TM      */
	uint16_t	source;		/* the requester accepted; Intel's SID */
};

/*
 * Encode an entry delivering `vector' to `destination'.  Answers zero when the
 * destination does not fit: wider than eight bits in Intel's xAPIC mode, and
 * at all in AMD's basic format, which has eight bits and nothing else.
 *
 * 🔴 INTEL'S ALWAYS NAMES ITS SOURCE: SVT 01b, SQ 00b, SID = `source'.  Without
 * that, an entry is usable by any device that writes its handle (§5.1.2.2), and
 * remapping would hand out vectors without isolating anybody.  AMD needs no
 * such field, because the table itself belongs to one device.
 *
 * ⚠️ AMD's basic entry has no trigger mode, so `level' is not written there:
 * the trigger travels with the message, not in the entry.
 */
int iommu_vtd_irte(const struct iommu_irte *e, int x2apic, uint64_t out[2]);
int iommu_amd_irte(const struct iommu_irte *e, uint32_t *out);

/*
 * Read an entry back.  Answers 1 when it remaps, 0 when it refuses, and -1
 * when it is something this kernel never writes -- reserved bits set, a posted
 * or guest-mode entry, a delivery other than fixed and physical, an Intel entry
 * that does not name its source.  `out' is filled only on 1.
 *
 * ⚠️ `x2apic' is Intel's EIME, and it changes what the same word means: in
 * xAPIC mode bits 63:48 and 39:32 of the entry are reserved, in x2APIC mode
 * they are part of the destination.
 */
int iommu_vtd_irte_decode(const uint64_t in[2], int x2apic,
			  struct iommu_irte *out);
int iommu_amd_irte_decode(uint32_t in, struct iommu_irte *out);

/*
 * ── #598: what a source writes so that its interrupt finds its entry ──
 *
 * Intel only, and that asymmetry is a design decision rather than a gap.
 *
 * On Intel the source must be reprogrammed: the message names its entry by a
 * handle in the ADDRESS, in a format of its own (Rev 5.20 §5.1.2.2), and an
 * old-style message is the very thing remapping is meant to block (fault 25h).
 *
 * 🔑 On AMD the message's DATA is the index -- bits 10:0 (Rev 3.11 §2.2.5,
 * Figure 14) -- and the table belongs to the device.  So if entry N of every
 * table delivers vector N, every message a source writes today is already the
 * right one, and nothing about MSI-X or the I/O APIC changes.  It is also what
 * keeps a level-triggered pin working under a broadcast EOI, which compares
 * the delivered vector with the RTE's vector field (#598 point 3).
 */

/*
 * An MSI or MSI-X message in remappable format for entry `index' (§5.1.5.2):
 * 0xFEE in 31:20, index[14:0] in 19:5, the format bit 4, SHV in bit 3,
 * index[15] in bit 2, and a data word of zero.  Answers zero for an index
 * beyond the sixteen bits a handle has.
 */
int iommu_vtd_msi(uint32_t index, uint32_t *address, uint32_t *data);

/*
 * The entry a message will select, the way the engine computes it (§5.1.3):
 * the handle, plus the data's subhandle when SHV is set.  Answers 1 for a
 * remappable message, 0 for a compatibility one, -1 for one the engine
 * refuses as malformed: not in the interrupt range, or reserved data bits set.
 */
int iommu_vtd_msi_decode(uint32_t address, uint32_t data, uint32_t *index);

/*
 * An I/O APIC redirection entry in remappable format (§5.1.5.1): index[14:0]
 * in 63:49, the format bit 48, index[15] in bit 11, delivery 000b in 10:8 so
 * that SHV is clear, and `vector' in 7:0, which must be the entry's own for a
 * level-triggered pin under a broadcast EOI.
 *
 * As the two halves ioapic_route() writes: `lo' is bits 31:0, `hi' 63:32.
 */
int iommu_vtd_ioapic_rte(uint32_t index, uint8_t vector, int level,
			 int active_low, int masked, uint32_t *lo, uint32_t *hi);

/*
 * The entry a redirection entry will select: 1 remappable, 0 compatibility,
 * -1 with a delivery mode other than fixed, which would set SHV.
 */
int iommu_vtd_ioapic_rte_decode(uint32_t lo, uint32_t hi, uint32_t *index);

/*
 * ── #598: which entry of intel's one table is whose ──────────────────
 *
 * Divided once, here, between the two kinds of source this kernel programs:
 * an I/O APIC pin's entry is its pin number, 0 to 127, and MSI slot s is
 * entry 128 + s.  Fixed rather than allocated, because the sources are fixed
 * already -- a pin, one of DEVICE_MD_MSI_MAX slots -- and a map with no state
 * cannot be left disagreeing with itself by a source that went away.
 *
 * The HPET has no range: while interrupts are remapped its comparator is
 * routed through its I/O APIC pin (#598 point 4), never as a message of its
 * own.
 *
 * Answers zero for a source past its range, which a caller must take as
 * "this source cannot be remapped" and never as entry zero.
 */
enum iommu_vtd_irt_source {
	IOMMU_VTD_IRT_PIN,
	IOMMU_VTD_IRT_MSI
};

int iommu_vtd_irt_index(enum iommu_vtd_irt_source kind, unsigned n,
			uint32_t *index);

/*
 * ── #598: where an intel engine finds its interrupt table ────────────
 *
 * Rev 5.20 §11.4.10, IRTA_REG at 0B8h: the table's address in 63:12, EIME in
 * bit 11, and in 3:0 a size S that is neither the number of entries nor its
 * logarithm but one less: the table has 2^(S+1) entries.
 *
 * 🔴 ONE TOO LARGE IS THE SILENT WAY TO BE WRONG.  The engine then takes the
 * table to run on into the next frame, and an index past the real end selects
 * bytes nobody wrote as an entry -- present or not by accident, and never
 * invalidated because nobody knows it is one.  One too small refuses the top
 * half of the indices (21h), which at least says so.
 *
 * Answers zero for a table not aligned to 4 Kbytes, or a count that is not a
 * power of two from 2 to 65536.  ⚠️ `x2apic' is EIME, reserved-zero on an
 * engine reporting EIM clear: whether to set it is the engine's question.
 */
int iommu_vtd_irta(uint64_t table_pa, unsigned entries, int x2apic,
		   uint64_t *out);

/*
 * ── #598: the device table entry's interrupt half, on AMD ─────────────
 *
 * Rev 3.11 §2.2.2.1, Table 7, bits 191:128 -- the third word of the entry:
 * IV, IntTabLen, IG, the interrupt table root, the pass bits, IntCtl.
 *
 * Written INTO an entry the page-table encoders above already produced, and
 * touching only that word: the two halves of a device table entry answer two
 * different questions, and an encoder for one must not decide the other.
 *
 * 🔴 AND THE CONVERSE IS A HAZARD TODAY.  Every encoder above writes this word
 * as zero -- IV clear, "passed through unmapped" -- so once remapping is on, any
 * path that rewrites a device's entry (an attach, a detach) would quietly turn
 * that device's interrupt remapping off.  Whoever turns it on owns that.
 *
 * Remapped, with every pass bit clear: NMI, INIT, ExtInt and LINT0/1 from the
 * device are target aborted, because a device that sends one of those has no
 * business doing so, and IG clear so that a refusal is logged.  `log2_entries'
 * is IntTabLen; the table must be aligned to 128 bytes.  Answers zero, and
 * leaves the entry alone, when either is not encodable.
 */
int iommu_amd_dte_interrupts(uint64_t table_pa, unsigned log2_entries,
			     uint64_t dte[4]);

/*
 * Read that half back: 1 remapped through a table, 0 passed through unmapped
 * (IV clear, or IntCtl 01b), -1 anything this kernel does not write --
 * interrupts aborted wholesale, a reserved IntCtl or IntTabLen, a pass bit set.
 */
int iommu_amd_dte_interrupts_decode(const uint64_t dte[4], uint64_t *table_pa,
				    unsigned *log2_entries);

/*
 * ── #598: Intel's invalidation queue, the descriptors ─────────────────
 *
 * Rev 5.20 §6.5.2, the 128-bit versions.  🔴 THE QUEUE IS NOT OPTIONAL FOR
 * REMAPPING INTERRUPTS: "Register-based invalidation cannot invalidate the
 * interrupt entry cache" (§6.5.1), and once the queue is on, "software must
 * submit invalidation commands only through the IQ" (§6.5.2) -- so #432's
 * context-cache and IOTLB invalidations become descriptors too, at the same
 * global granularity they use through the registers today.
 *
 * Encoders only.  The engine reads these and nothing in the kernel ever does,
 * so there is no reading to check; the check is against words written from the
 * figures, as entries_agree() does for the context and device table entries.
 */
void iommu_vtd_qi_context_global(uint64_t out[2]);	/* type 1, G 01b */
void iommu_vtd_qi_iotlb_global(uint64_t out[2]);	/* type 2, G 01b */

/*
 * Type 4: the whole interrupt entry cache, or the 2^`mask_log2' entries from
 * `index', which must be aligned to that many -- the engine ignores the low
 * bits the mask covers, so an unaligned index would invalidate a block that
 * does not start where the caller thinks.  Answers zero for either mistake.
 */
void iommu_vtd_qi_iec_global(uint64_t out[2]);
int iommu_vtd_qi_iec(uint32_t index, unsigned mask_log2, uint64_t out[2]);

/*
 * Type 5 with Status Write: the engine writes `data' to `status_pa' once every
 * descriptor before this one has completed.  The address must be four-byte
 * aligned, its bits 1:0 not being part of the field.
 */
int iommu_vtd_qi_wait(uint64_t status_pa, uint32_t data, uint64_t out[2]);

/*
 * ── #598: Intel's invalidation queue, the ring ───────────────────────
 *
 * Rev 5.20 §6.5.2 and §11.4.9.  One page of 128-bit descriptors (IQA's QS 0,
 * DW 0: 256 of them), a head the engine moves (IQH) and a tail software moves
 * (IQT), both an index in bits 18:4.  Empty when the two are equal, full when
 * the tail is one behind the head, so 255 at most are ever outstanding.
 *
 * A core over a view of the registers, as #599 made the fault drains: placing
 * descriptors and reading the answer to a wait are things a fabricated engine
 * can be asked about at every boot, and only ringing needs a real one.
 *
 * 🔑 A submission ends with a wait whose status write carries a number never
 * used before, and the submission is over when that number is in the cell.
 * Appendix A: the engine's reads of the ring and its status write are snooped
 * whatever ECAP.C says, so both sides are plain memory to the processor.
 */
#define	IOMMU_VTD_QUEUE_SLOTS	256u

/*
 * The size of <cpu/iommu.c>'s unit table, here because each vendor keeps
 * per-engine state sized by it too (#598: one queue per intel engine).
 */
#define	IOMMU_MAX_UNITS		8

struct iommu_vtd_queue {
	volatile uint64_t	*iqh, *iqt;	/* the engine's head, our tail  */
	volatile uint32_t	*fsts;		/* IQE and ITE stop the queue   */
	volatile uint64_t	*ring;		/* the slots, two words each    */
	volatile uint32_t	*status;	/* the wait's cell, as we read it */
	uint64_t		 status_pa;	/* ... and as the engine writes it */
	unsigned		 tail;		/* where the next descriptor goes */
	uint32_t		 seq;		/* the last wait's data, never 0 */
};

/*
 * Write `n' descriptors and a wait behind them at the tail, and answer the
 * wait's data -- or zero, having written nothing, when the ring has no room
 * for all n + 1.  The engine is not told: see iommu_vtd_queue_ring().
 */
uint32_t iommu_vtd_queue_place(struct iommu_vtd_queue *q,
			       const uint64_t (*desc)[2], unsigned n);

/* Tell the engine where the tail is now. */
void iommu_vtd_queue_ring(const struct iommu_vtd_queue *q);

/*
 * Wait for the wait whose data is `seq': 1 when its status write has arrived,
 * -1 when the engine reports what stops the queue (IQE or ITE), 0 when
 * `spins' rounds went by with neither.
 */
int iommu_vtd_queue_wait(const struct iommu_vtd_queue *q, uint32_t seq,
			 unsigned spins);

/*
 * ── Stage 3d: pointing a live engine at a new table ──────────────────
 *
 * Rewrite the entry the engine reads for `bdf' so that it walks `d', and make
 * the engine forget what it cached about that device.  Answers non-zero when
 * the entry was written and the invalidation completed.
 *
 * 🔴 THE INVALIDATION IS THE HALF THAT CANNOT BE SEEN TO BE MISSING.  Both
 * engines cache the device's entry and its translations, and both are entitled
 * to go on using what they cached -- so an attach without one produces a
 * device that is in its new domain according to memory and in its old one
 * according to the silicon, for an unbounded time, with every read-back
 * agreeing.  It is the same shape as a TLB shootdown skipped (#350), and it
 * fails the same way: correctly, until it does not.
 *
 * ⚠️ Structured as one call and not as "write the entry" plus "invalidate",
 * because those two are only ever right together and an interface that can
 * express one without the other will eventually be used that way.
 */
int iommu_vtd_attach(uint16_t bdf, const struct iommu_domain *d);
int iommu_amd_attach(uint16_t bdf, const struct iommu_domain *d);

/*
 * A mapping inside `d' changed; make the engine forget what it translated
 * through the old one.
 *
 * 🔴 CALLED EVEN WHEN A PAGE GOES FROM ABSENT TO PRESENT.  The instinct is
 * that only removals need invalidating -- nothing can have cached a
 * translation that did not exist.  Both vendors are entitled to cache the
 * ABSENCE: Intel says so through CAP.CM and AMD through its page directory
 * cache, so a grant that skipped this would give a device a buffer the engine
 * goes on refusing, and the fault would name a page the tables say is mapped.
 */
int iommu_vtd_flush(const struct iommu_domain *d);
int iommu_amd_flush(const struct iommu_domain *d);

/*
 * Take a device out of its domain and leave it reaching NOTHING.
 *
 * 🔴 BLOCKED AND NOT PASS-THROUGH, which is the whole content of the call.  A
 * device whose driver's capability was revoked has just lost its authority; if
 * detaching restored the pass-through entry it started in, the revocation
 * would have GIVEN it all of memory.  A revocation that widens what something
 * can reach is worse than no revocation, because it is one somebody trusted.
 *
 * ⚠️ The domain's page tables are left where they are.  Nothing walks them any
 * more -- the device's entry does not point at them -- and freeing them means
 * knowing no engine still holds a cached translation through them, which is
 * what the invalidation here establishes and what a later reuse would have to
 * establish again.  Frames are cheaper than that argument.
 */
int iommu_vtd_detach(uint16_t bdf);
int iommu_amd_detach(uint16_t bdf);

/*
 * Start a unit, and get back the index that iommu_record_scope() will attach
 * scopes to.  Answers -1 when there is no room, having set the truncation
 * flag -- and a reader that ignores the -1 will write into a unit that is not
 * there, so it is checked.
 */
int iommu_record_unit(uint16_t segment, uint64_t base, uint64_t size,
		      int covers_rest);

/*
 * Start a reserved region.  Same contract as above, and the same -1.
 */
int iommu_record_reserved(uint16_t segment, uint64_t base, uint64_t limit);

/*
 * Attach one scope to whichever unit or region was recorded last.
 *
 * ⚠️ To the LAST one, deliberately, rather than to an index the caller passes.
 * Both tables interleave a header with the scopes belonging to it, so the
 * reader is always adding to the thing it has just started; an index parameter
 * would be a second way to say the same thing, and the two ways would
 * eventually disagree.
 */
void iommu_record_scope(const struct iommu_scope *scope);

/*
 * What a reader learned from a unit's own registers.
 *
 * Called with the unit's index rather than "the last one" because the two
 * readers do this at different moments: the DMAR's walks the whole table and
 * then follows each engine's base, while the IVRS's confirms each engine as it
 * records it -- its own filter for duplicate descriptions of one engine would
 * otherwise have to be repeated in a second pass, and a rule with two copies
 * is a rule that gets fixed in one of them.
 *
 * Either way the table walk and the register read stay two distinguishable
 * failures, which is what lets the self-test say which one happened.
 */
void iommu_record_hardware(unsigned index, uint32_t version,
			   unsigned address_bits, uint32_t page_levels,
			   int interrupt_remapping, int coherent_walk,
			   uint64_t caps0, uint64_t caps1);

/* What the interrupt decode answered for that unit, beside the above. */
void iommu_record_interrupt(unsigned index,
			    const struct iommu_interrupt_caps *caps);

/* Say which vendor's tables were the ones read. */
void iommu_record_vendor(enum iommu_vendor vendor);

/*
 * What the table's own header said about the platform.  Zero address bits
 * means the table did not say, which is a real state and not a default.
 */
void iommu_record_platform(unsigned address_bits, int interrupt_remapping,
			   int x2apic_discouraged);

/*
 * Whether the reader's cursor finished exactly on the end the table's own
 * header declared.  Recorded rather than returned, because a reader that
 * misparsed still found a table and still has things to report -- see
 * iommu_walk_exact().
 */
void iommu_record_walk(int exact);

/*
 * Forget everything recorded so far.
 *
 * For a reader that found its table, started recording, and then decided the
 * table was not usable.  Without this the only two outcomes would be "recorded
 * correctly" and "recorded partially", and the second is indistinguishable
 * from the first to everything downstream.
 */
void iommu_record_reset(void);

#endif	/* _X86_64_CPU_IOMMU_BACKEND_H_ */
