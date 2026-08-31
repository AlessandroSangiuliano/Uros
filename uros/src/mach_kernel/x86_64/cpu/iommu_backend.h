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
