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
 *	pci_bar_test.c — the BAR decode, judged without a device (#427).
 *
 *	🔑 This runs on the machine doing the build, not under an emulator, and
 *	that is the whole reason the decode was written as a pure function.  The
 *	defect it guards against needs a device sitting above four gigabytes to
 *	show itself on real hardware, and no machine we have places one there
 *	unless told to -- so a check that waited for one would be a check that
 *	never ran.  Six numbers and an expected answer need neither.
 *
 *	⚠️ It is built twice, once per ABI, because that is the only thing about
 *	this code that could differ between the targets.  There are no pointers
 *	and no `long' in the computation: the arithmetic is uint32_t in and
 *	uint64_t out, which are the same two types on both.  Building it both
 *	ways is what turns that from a claim into a result.
 *
 *	Every arm is written so a WRONG decode gives a wrong ANSWER.  "It did
 *	not crash" is not evidence here: reading a 64-bit BAR as two 32-bit ones
 *	produces a plausible address and a plausible extra region, and both look
 *	exactly like a device until someone maps them.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pci_bar.h"

static int passed;
static int failed;

static void
arm(const char *name, int ok, const char *detail)
{
	if (ok) {
		printf("pci_bar_test: %s: OK\n", name);
		passed++;
		return;
	}
	printf("pci_bar_test: %s: WRONG — %s\n", name, detail);
	failed++;
}

static void
arm_u64(const char *name, uint64_t got, uint64_t want, const char *detail)
{
	if (got == want) {
		printf("pci_bar_test: %s: OK\n", name);
		passed++;
		return;
	}
	printf("pci_bar_test: %s: WRONG — %s "
	       "(got 0x%016llx, want 0x%016llx)\n",
	       name, detail,
	       (unsigned long long)got, (unsigned long long)want);
	failed++;
}

int
main(void)
{
	struct pci_bar_region	r[8];
	unsigned int		n;

	printf("pci_bar_test: the BAR decode, %u-bit build, no device\n",
	       (unsigned)(sizeof(void *) * 8));

	/*
	 * [1] The one the issue is about.  A 64-bit memory BAR at slot 0 with
	 * its upper half in slot 1, placed above four gigabytes.
	 *
	 * 🔑 The COUNT is the first assertion, before the address: reading the
	 * two halves independently gives two regions whose addresses are both
	 * plausible, and a check that only looked at r[0].base would pass on
	 * the low half and never notice the second region that does not exist.
	 */
	{
		const uint32_t slots[6] = {
			0xC0000004u,	/* 64-bit, prefetchable off, base low */
			0x00000002u,	/* upper half: 0x2_00000000 */
			0, 0, 0, 0
		};

		n = pci_bars_decode(slots, 6, r, 8);
		arm("[1] 64-bit pair is ONE region", n == 1,
		    "the two halves were read as two independent BARs");
		if (n >= 1)
			arm_u64("[1] 64-bit base", r[0].base,
				0x00000002C0000000ull,
				"the upper half did not reach the address");
		if (n >= 1)
			arm("[1] flagged as 64-bit",
			    (r[0].flags & PCI_REGION_MEM_64) != 0,
			    "the region does not say it took two slots");
	}

	/*
	 * [2] An I/O BAR, where bits 2:1 are not a type and the address keeps
	 * two bits a memory BAR does not.
	 *
	 * 🔑 The value is chosen so the two masks DISAGREE: bits 3:2 are set,
	 * so masking with ~0xF -- the memory mask -- loses 0xC.  A test whose
	 * address had zeros there would pass with either mask and prove
	 * nothing about which one ran.
	 */
	{
		const uint32_t slots[6] = { 0x0000C0CDu, 0, 0, 0, 0, 0 };

		n = pci_bars_decode(slots, 6, r, 8);
		arm("[2] I/O BAR is one region", n == 1,
		    "an I/O BAR was not decoded, or swallowed the next slot");
		if (n >= 1)
			arm_u64("[2] I/O address keeps bits 3:2", r[0].base,
				0x0000C0CCull,
				"masked with the memory mask, which drops 0xC");
		if (n >= 1)
			arm("[2] flagged as I/O",
			    (r[0].flags & PCI_REGION_IO) != 0
			    && (r[0].flags & PCI_REGION_MEM_64) == 0,
			    "an I/O BAR came back as memory, or as 64-bit");
	}

	/*
	 * [3] Six slots, three regions: a 64-bit pair, a 32-bit, an I/O.
	 * This is the shape that makes "six slots are not six regions" a
	 * number rather than a sentence.
	 */
	{
		const uint32_t slots[6] = {
			0xFE000004u, 0x00000001u,	/* 64-bit @ 0x1_FE000000 */
			0xFB000000u,			/* 32-bit memory */
			0x0000E001u,			/* I/O */
			0, 0
		};

		n = pci_bars_decode(slots, 6, r, 8);
		arm("[3] six slots, three regions", n == 3,
		    "the count follows the slots instead of the decode");
		if (n == 3) {
			arm_u64("[3] region 0", r[0].base, 0x00000001FE000000ull,
				"the 64-bit pair");
			arm_u64("[3] region 1", r[1].base, 0x00000000FB000000ull,
				"the 32-bit memory BAR after the pair");
			arm_u64("[3] region 2", r[2].base, 0x0000E000ull,
				"the I/O BAR");
			arm("[3] slot indices", r[0].slot == 0
			    && r[1].slot == 2 && r[2].slot == 3,
			    "a region does not name the slot it started at");
		}
	}

	/*
	 * [4] A 64-bit BAR in the LAST slot: malformed, because there is no
	 * slot to hold the upper half.  The value comes off the bus, so this
	 * is something a device can say.
	 *
	 * 🔥 The canary is the point.  A decoder that reads slots[i + 1]
	 * without checking finds whatever follows the array and builds an
	 * address out of it -- which on a good day is a wrong region and on a
	 * bad day is a fault.  Here what follows is a value chosen so that
	 * using it is visible in the answer.
	 */
	{
		struct {
			uint32_t slots[6];
			uint32_t canary;
		} probe = {
			{ 0, 0, 0, 0, 0, 0x40000004u },	/* 64-bit in slot 5 */
			0xDEADBEEFu
		};

		n = pci_bars_decode(probe.slots, 6, r, 8);
		arm("[4] 64-bit in the last slot yields no region", n == 0,
		    "the decode read past the six slots it was given");
		if (n > 0)
			arm_u64("[4] and built nothing from it", r[0].base,
				0ull,
				"a region was made out of the malformed slot, "
				"or out of what follows the array");
	}

	/*
	 * [5] Slots that read zero are not regions, and a device with a hole
	 * between two BARs still reports the two.
	 */
	{
		const uint32_t slots[6] = {
			0xFC000000u, 0, 0, 0xFD000000u, 0, 0
		};

		n = pci_bars_decode(slots, 6, r, 8);
		arm("[5] empty slots are skipped, not counted", n == 2,
		    "an unimplemented BAR was reported as a region");
	}

	/*
	 * [6] The prefetchable bit, and the one case where a region is real
	 * although the slot's own address bits are zero: the upper half
	 * carries it.
	 *
	 * 🔑 This is why the zero test is made on the DECODED base and not on
	 * the raw slot.  A BAR at exactly 0x1_00000000 has a lower half whose
	 * address bits are all zero, and rejecting it early would drop a
	 * device that exists.
	 */
	{
		const uint32_t slots[6] = {
			0x0000000Cu,	/* 64-bit, prefetchable, low bits zero */
			0x00000001u,	/* upper half: 0x1_00000000 */
			0, 0, 0, 0
		};

		n = pci_bars_decode(slots, 6, r, 8);
		arm("[6] a region whose lower half is all flags", n == 1,
		    "dropped a device sitting exactly at a 4 GiB boundary");
		if (n >= 1) {
			arm_u64("[6] base", r[0].base, 0x0000000100000000ull,
				"the upper half alone makes this address");
			arm("[6] prefetchable",
			    (r[0].flags & PCI_REGION_PREFETCH) != 0,
			    "bit 3 did not reach the flags");
		}
	}

	/*
	 * [7] The caller's array is respected: `max' smaller than the number
	 * of regions stops rather than writing past it.
	 */
	{
		const uint32_t slots[6] = {
			0xFC000000u, 0xFD000000u, 0xFE000000u, 0, 0, 0
		};
		struct { struct pci_bar_region r[2]; uint64_t canary; } small;

		small.canary = 0xA5A5A5A5A5A5A5A5ull;
		n = pci_bars_decode(slots, 6, small.r, 2);
		arm("[7] stops at max", n == 2 &&
		    small.canary == 0xA5A5A5A5A5A5A5A5ull,
		    "wrote more regions than the caller had room for");
	}

	/*
	 * ── The size, which is measured and not read (#427) ──────────
	 *
	 * The mutation half -- disable the decoding, write all ones, read back,
	 * put the address back -- needs the bus.  This half is arithmetic, and
	 * the arms below are the read-backs a device would have given, so the
	 * arithmetic is judged here rather than on whatever hardware happens to
	 * be plugged in.
	 */

	/*
	 * [8] Four kilobytes of 32-bit memory: the ordinary case, and the one
	 * every formula gets right.  It is here so the arms that follow are read
	 * as differences from something that works.
	 *
	 * ⚠️ The low four bits come back as the ORIGINAL flags, not as ones:
	 * they are read-only, so a write of all ones does not reach them.  A
	 * computation that forgot to mask them off would answer 1 byte.
	 */
	{
		struct pci_bar_region reg = { 0, 0, 0, 0 };

		arm_u64("[8] 32-bit memory, 4 KiB",
			pci_bar_size_from_probe(&reg, 0xFFFFF000u, 0),
			0x1000ull, "the writable field is bits 31:12");
		arm("[8] one slot", pci_bar_region_slots(&reg) == 1,
		    "a 32-bit region was said to occupy two slots");
	}

	/*
	 * [9] 🔥 THE ARM THAT SEPARATES THE TWO FORMULAS.  An I/O BAR on a device
	 * that decodes sixteen bits reads back zeros in its top half, so the
	 * writable field is NOT every bit above the size.
	 *
	 * `~value + 1' over that read-back gives 0xFFFF0020 -- four gigabytes
	 * minus a bit, for a thirty-two byte port range -- and it gives it
	 * quietly, because a size is a number and every number looks like an
	 * answer.  The lowest set bit gives 0x20.
	 *
	 * This is not a hypothetical device: it is the shape of the SATA
	 * controller's BAR4 on the board these suites boot on.
	 */
	{
		struct pci_bar_region reg = { 0, 0, PCI_REGION_IO, 0 };

		arm_u64("[9] I/O BAR, 32 bytes, upper half unimplemented",
			pci_bar_size_from_probe(&reg, 0x0000FFE1u, 0),
			0x20ull,
			"complementing the read-back turns 32 bytes into 4 GiB");
	}

	/*
	 * [10] The two-gigabyte 64-bit region this issue's last box is about.
	 * The read-back spans both slots, and the size does not fit in the lower
	 * one alone.
	 */
	{
		struct pci_bar_region reg = {
			0, 0, PCI_REGION_MEM_64 | PCI_REGION_PREFETCH, 2
		};

		arm_u64("[10] 64-bit region, 2 GiB",
			pci_bar_size_from_probe(&reg, 0x8000000Cu, 0xFFFFFFFFu),
			0x80000000ull, "the upper slot did not enter the size");
		arm("[10] two slots", pci_bar_region_slots(&reg) == 2,
		    "a 64-bit region was said to occupy one slot, so the "
		    "measurement would have written ones to only half of it");
	}

	/*
	 * [11] A region LARGER than four gigabytes, where the whole answer lives
	 * in the upper slot.
	 *
	 * 🔑 An arithmetic done in 32 bits does not merely truncate this one, it
	 * answers ZERO -- every writable bit is above the boundary -- and a size
	 * of zero reads as "no such region" rather than as a wrong number.  A
	 * defect that turns a device into an absence is the kind nobody goes
	 * looking for.
	 */
	{
		struct pci_bar_region reg = { 0, 0, PCI_REGION_MEM_64, 0 };

		arm_u64("[11] 64-bit region, 8 GiB",
			pci_bar_size_from_probe(&reg, 0x0000000Cu, 0xFFFFFFFEu),
			0x200000000ull,
			"a region bigger than four gigabytes came back as none");
	}

	/*
	 * [12] Nothing writable at all: the honest answer is zero, and it has to
	 * be reached before the arithmetic rather than by it.  Complementing a
	 * read-back of zero gives four gigabytes, which is the largest wrong
	 * answer this function can produce and the one an unimplemented BAR
	 * would produce on every device that has one.
	 */
	{
		struct pci_bar_region reg = { 0, 0, 0, 0 };

		arm_u64("[12] nothing writable is size zero",
			pci_bar_size_from_probe(&reg, 0x00000000u, 0),
			0ull, "an unimplemented BAR was sized at 4 GiB");
	}

	printf("pci_bar_test: %d of %d arms passed\n", passed, passed + failed);

	/*
	 * ⚠️ Zero is success.  Written the other way round first, which ctest
	 * would have reported as a passing test on every failing decode -- the
	 * exact shape of defect the rest of this file exists to catch, in the
	 * file that catches it.
	 */
	return failed == 0 ? 0 : 1;
}
