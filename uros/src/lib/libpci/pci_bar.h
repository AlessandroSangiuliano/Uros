/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

/*
 *	pci_bar.h — six raw BAR slots to N device regions (#427).
 *
 *	🔑 SIX SLOTS ARE NOT SIX REGIONS.  A memory BAR that declares itself
 *	64-bit occupies two consecutive slots, because configuration space is
 *	made of 32-bit registers and an address above four gigabytes has nowhere
 *	else to live.  So how many regions a device has is not known until the
 *	slots have been walked, and a loop with a fixed step cannot express the
 *	answer -- it is wrong in SHAPE, not in value.
 *
 *	⚠️ On i386 the upper half was always zero, so reading the two halves as
 *	two independent 32-bit BARs cost nothing: the address was right and the
 *	spurious second region named zero and was ignored.  Neither is true on a
 *	machine where a device can sit above four gigabytes.
 *
 *	🔑 This header and its one translation unit contain NO RPC.  That is
 *	deliberate and it is what makes the decode checkable: a pure function
 *	from six numbers to a list can be handed a synthetic 64-bit pair and
 *	judged on both targets, with no device above four gigabytes and no
 *	emulator.  Only the end-to-end mapping needs a machine that places one
 *	high on purpose.
 */

#ifndef	_LIBPCI_PCI_BAR_H_
#define	_LIBPCI_PCI_BAR_H_

/*
 * ⚠️ Included here rather than relied upon.  Every target build in this tree
 * passes `-include stdint.h', so a header that needs uint64_t appears to work
 * without saying so -- until it is compiled somewhere that does not, which is
 * exactly what the host-side unit test is.  A header that names its own
 * dependencies is the difference between "it builds here" and "it builds".
 */
#include <stdint.h>
#include <stddef.h>

#include <device/pci.h>

/* What a region is, as opposed to what a slot said. */
#define	PCI_REGION_IO		0x1u	/* I/O space; never 64-bit */
#define	PCI_REGION_MEM_64	0x2u	/* took two slots */
#define	PCI_REGION_PREFETCH	0x4u	/* memory only */

/*
 * One device region.
 *
 * ⚠️ `base' is a PHYSICAL address and fixed at 64 bits on both targets, not
 * vm_offset_t.  Two reasons and they point the same way: a bus address is not
 * a width of the processor that reads it -- a 32-bit machine can be told about
 * a 64-bit BAR and has to be able to say so rather than silently agree -- and
 * this struct travels in an untyped out-of-line buffer, where a member whose
 * width follows the target would make the two targets' wire formats differ for
 * no reason anyone asked for.
 *
 * `size' is zero as the decode leaves it, and is filled in by whoever can
 * reach the bus.  The size of a region is not read, it is measured -- write all
 * ones, read back, restore -- and that is a WRITE to the device, so it does not
 * belong in a decode.  pci_bar_size_from_probe() below is the arithmetic half
 * of that measurement; hal_server's pci_scan module is the half that writes.
 *
 * ⚠️ A size of zero therefore means "nobody measured this one", and it is not
 * the same statement as a base of zero.  A consumer that maps a region has to
 * treat it as a refusal rather than as an empty region.
 */
struct pci_bar_region {
	uint64_t	base;
	uint64_t	size;
	uint32_t	flags;		/* PCI_REGION_* */
	uint32_t	slot;		/* the BAR slot it starts at, 0..5 */
};

/*
 * Decode `nslots' raw BAR values into at most `max' regions.
 * Returns how many regions there are.
 *
 * Slots reading zero are not regions: an unimplemented BAR reads as zero, and
 * reporting one would hand a driver an address of nothing.
 *
 * ⚠️ A slot claiming to be 64-bit with no slot after it is malformed -- the
 * standard forbids it precisely because there would be nowhere to put the
 * upper half -- and it produces NO region rather than a region built from
 * whatever follows the array.  That is the one place a decoder written in a
 * hurry reads out of bounds, and it is reachable from a device: the value
 * comes off the bus.
 */
extern unsigned int pci_bars_decode(const uint32_t *slots,
				    unsigned int nslots,
				    struct pci_bar_region *out,
				    unsigned int max);

/*
 * How many BAR slots this region occupies: two for a 64-bit memory region,
 * one for anything else.
 *
 * 🔑 Asked rather than recomputed.  The caller that measures a region has to
 * write to every slot the region occupies, so it needs the same "two slots or
 * one" answer the decode reached -- and a second place that works it out from
 * the flags is a second place that can disagree.  The decode already decided;
 * this hands back its decision.
 */
extern unsigned int pci_bar_region_slots(const struct pci_bar_region *r);

/*
 * The size of a region, from the value its slot(s) read back after all ones
 * were written to them.  `hi' is the upper slot's read-back and is ignored
 * for a region that does not have one.
 *
 * 🔑 A SIZE IS NOT READ, IT IS MEASURED, and this is the half of the
 * measurement that is arithmetic.  A BAR implements a writable high field and
 * hardwires the bits below it to zero, so the count of writable bits IS the
 * size -- write ones, and what comes back has zeros exactly where the device
 * refuses to be moved.  The mutation belongs to whoever can reach the bus; the
 * arithmetic belongs here, where it can be handed a synthetic read-back and
 * judged with no device at all.
 *
 * ⚠️ The answer is the LOWEST SET BIT of the writable field, not
 * `~value + 1'.  The two agree only when every bit above the size is writable,
 * and for an I/O BAR they routinely do not: a device that decodes sixteen bits
 * of I/O space reads back zero in the top half, and the complement of that is
 * an enormous number instead of thirty-two bytes.
 *
 * A region whose slots read back all zeros has no writable bits at all and
 * gets size 0 -- which is what an unimplemented BAR looks like, and it is a
 * better answer than the four gigabytes the arithmetic would otherwise give.
 */
extern uint64_t pci_bar_size_from_probe(const struct pci_bar_region *r,
					uint32_t lo, uint32_t hi);

#endif	/* _LIBPCI_PCI_BAR_H_ */
