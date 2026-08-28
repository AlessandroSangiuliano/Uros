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
 *	pci_bar.c — the decode, and nothing else (#427).
 *
 *	No RPC, no globals, no I/O.  See pci_bar.h for why that is the design
 *	and not an accident of this file being small.
 */

#include "pci_bar.h"

unsigned int
pci_bars_decode(const uint32_t *slots, unsigned int nslots,
		struct pci_bar_region *out, unsigned int max)
{
	unsigned int i = 0;
	unsigned int n = 0;

	if (slots == NULL || out == NULL)
		return 0;

	/*
	 * ⚠️ `i += 1 or 2' inside the body, not in the for-step.  The step is
	 * the whole subject of this function: a 64-bit BAR consumes the slot
	 * after it, and a loop that advances by one regardless is the defect
	 * being fixed, not a simpler spelling of it.
	 */
	while (i < nslots && n < max) {
		uint32_t	raw = slots[i];
		uint64_t	base;
		uint32_t	flags = 0;

		/* An unimplemented BAR reads zero.  Not a region. */
		if (raw == 0) {
			i++;
			continue;
		}

		if (raw & PCI_BAR_SPACE_IO) {
			/*
			 * I/O space.  Bits 2:1 are NOT a type here -- bit 1 is
			 * reserved -- so the 64-bit question is not asked, and
			 * the address keeps two more bits than a memory BAR
			 * does.  Reading this one as memory would both mask
			 * off a real address bit and, if bits 2:1 happened to
			 * read 0b10, swallow the following slot.
			 */
			base = (uint64_t)(raw & PCI_BAR_IO_ADDR_MASK);
			flags = PCI_REGION_IO;
			i++;
		} else if ((raw & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64) {
			/*
			 * 64-bit memory: the upper half is the next slot.
			 *
			 * ⚠️ If there is no next slot the device is lying --
			 * the standard forbids a 64-bit BAR in the last slot
			 * because there would be nowhere to put the upper half
			 * -- and this stops rather than reading past the
			 * array.  The value came off the bus, so "malformed"
			 * is a thing a device can say.
			 */
			if (i + 1 >= nslots)
				break;

			base = (uint64_t)(raw & PCI_BAR_MEM_ADDR_MASK)
			     | ((uint64_t)slots[i + 1] << 32);
			flags = PCI_REGION_MEM_64;
			if (raw & PCI_BAR_MEM_PREFETCH)
				flags |= PCI_REGION_PREFETCH;
			i += 2;
		} else {
			/*
			 * 32-bit memory.  The reserved type 0b01 lands here
			 * too: it meant "below one megabyte" on a bus nobody
			 * has any more, and treating it as an ordinary 32-bit
			 * region is what it decodes to anyway.
			 */
			base = (uint64_t)(raw & PCI_BAR_MEM_ADDR_MASK);
			if (raw & PCI_BAR_MEM_PREFETCH)
				flags |= PCI_REGION_PREFETCH;
			i++;
		}

		/*
		 * A BAR whose address bits are all zero is not a region
		 * either -- the flag bits alone can make `raw' non-zero, and
		 * an address of zero is not somewhere a device lives.
		 *
		 * 🔑 Checked AFTER the width was decided, not before: an upper
		 * half that is non-zero makes the region real even when the
		 * lower half's address bits are not, and testing `raw' would
		 * have thrown that away.
		 */
		if (base == 0)
			continue;

		out[n].base = base;
		out[n].size = 0;		/* not read; measured, later */
		out[n].flags = flags;
		out[n].slot = (i - ((flags & PCI_REGION_MEM_64) ? 2u : 1u));
		n++;
	}

	return n;
}
