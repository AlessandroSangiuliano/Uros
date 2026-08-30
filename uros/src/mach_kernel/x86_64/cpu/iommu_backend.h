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
