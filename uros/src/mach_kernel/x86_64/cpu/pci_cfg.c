/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 *	pci_cfg.c — configuration space, through ECAM where there is one and
 *	through the port pair where there is not (#457).
 *
 *	See pci_cfg.h for why both, and for the measurement that settled it.
 */

#include <stdint.h>

#include <cpu/acpi.h>
#include <cpu/pci_cfg.h>
#include <cpu/regs.h>	/* inb/outl and the other widths */
#include <pmap/pmap.h>
#include <kern/misc_protos.h>
#include <device/pci.h>	/* the capability list, as the standard fixes it */

/* The port pair.  Two 32-bit registers, and every access takes turns on them. */
#define PCI_CONFIG_ADDRESS	0x0CF8
#define PCI_CONFIG_DATA		0x0CFC
#define PCI_CONFIG_ENABLE	0x80000000u

/*
 * How much of configuration space one bus occupies when it is memory mapped:
 * 32 devices x 8 functions x 4096 bytes.
 */
#define ECAM_BUS_SIZE		(1u << 20)

/*
 * The mapped window, and the bus range it covers.
 *
 * ⚠️ ONE segment and a bus range decided at init, rather than a mapping made
 * on demand.  A mapping made per access would be a page table edit and a TLB
 * shootdown inside what callers treat as a register read.  A machine with
 * more than one segment group is a thing this does not do yet, and it says so
 * at init rather than addressing the wrong one quietly.
 */
static uint64_t	ecam_va;		/* 0 when the ports are being used */
static uint8_t	ecam_bus_first;
static uint8_t	ecam_bus_last;

int
pci_cfg_is_ecam(void)
{
	return ecam_va != 0;
}

/*
 * ⚠️ The bus range is discovered by asking for one bus at a time rather than
 * read out of the MCFG entry.  acpi_ecam_base() folds the bus offset into
 * what it returns, which is the right shape for a caller that wants one bus
 * -- and it means the range has to be found the way a caller would find it.
 * Sixteen probes at boot, and no second reader of the MCFG's layout.
 */
static void
find_bus_range(uint64_t base0)
{
	unsigned b;

	ecam_bus_first = 0;
	ecam_bus_last = 0;

	for (b = 1; b < 256; b++) {
		uint64_t want = base0 + (uint64_t)b * ECAM_BUS_SIZE;

		if (acpi_ecam_base(0, (uint8_t)b) != want)
			break;
		ecam_bus_last = (uint8_t)b;
	}
}

void
pci_cfg_init(void)
{
	uint64_t base0 = acpi_ecam_base(0, 0);
	uint64_t span;

	if (base0 == 0) {
		/*
		 * No MCFG.  Not a failure and not a fallback in the apologetic
		 * sense: this is what an i440FX is, and it is the board this
		 * tree's harness boots by default.
		 */
		ecam_va = 0;
		printf("pci: configuration space through ports 0xCF8/0xCFC — "
		       "no MCFG, so bus 255 is the ceiling and there is one "
		       "segment\n");
		return;
	}

	find_bus_range(base0);
	span = (uint64_t)(ecam_bus_last - ecam_bus_first + 1) * ECAM_BUS_SIZE;

	ecam_va = pmap_map_device(base0, span);

	printf("pci: configuration space through ECAM at 0x%08x%08x, "
	       "buses %u..%u (%u KiB mapped)\n",
	       (unsigned int)(base0 >> 32), (unsigned int)(base0 & 0xFFFFFFFFu),
	       ecam_bus_first, ecam_bus_last, (unsigned int)(span >> 10));
}

/*
 * The address of one register in the mapped window, or 0 if this machine
 * cannot express the request.
 */
static volatile uint32_t *
ecam_at(uint16_t segment, uint8_t bus, uint8_t dev, uint8_t func, uint16_t reg)
{
	uint64_t off;

	if (segment != 0 || bus < ecam_bus_first || bus > ecam_bus_last)
		return 0;

	off = ((uint64_t)(bus - ecam_bus_first) << 20)
	    | ((uint64_t)dev << 15)
	    | ((uint64_t)func << 12)
	    | (uint64_t)reg;

	return (volatile uint32_t *)(uintptr_t)(ecam_va + off);
}

uint32_t
pci_cfg_read(uint16_t segment, uint8_t bus, uint8_t dev, uint8_t func,
	     uint16_t reg)
{
	if (ecam_va != 0) {
		volatile uint32_t *p = ecam_at(segment, bus, dev, func, reg);

		/*
		 * A request this machine's window does not cover reads as an
		 * absent function.  ⚠️ Not silently redirected to the ports:
		 * the two mechanisms disagree about what exists -- the port
		 * pair cannot see a second segment at all -- so answering from
		 * the other one would report a device from somewhere else
		 * under the name of the one that was asked for.
		 */
		if (p == 0)
			return 0xFFFFFFFFu;
		return *p;
	}

	/*
	 * ⚠️ A segment other than zero cannot be said here, and the request is
	 * refused rather than served without it.  The port pair has no field
	 * for a segment: issuing the access anyway would read the identically
	 * numbered device of segment zero and answer as though it were the one
	 * asked about.
	 */
	if (segment != 0)
		return 0xFFFFFFFFu;

	outl(PCI_CONFIG_ADDRESS,
	     PCI_CONFIG_ENABLE
	     | ((uint32_t)bus << 16)
	     | ((uint32_t)dev << 11)
	     | ((uint32_t)func << 8)
	     | ((uint32_t)reg & 0xFCu));
	return inl(PCI_CONFIG_DATA);
}

void
pci_cfg_write(uint16_t segment, uint8_t bus, uint8_t dev, uint8_t func,
	      uint16_t reg, uint32_t value)
{
	if (ecam_va != 0) {
		volatile uint32_t *p = ecam_at(segment, bus, dev, func, reg);

		/*
		 * 🔑 A write that cannot be addressed is DROPPED, and that is
		 * the asymmetry worth reading.  A read that cannot be served
		 * can answer "absent", which is true.  A write has no such
		 * answer: performing it somewhere else would program a device
		 * nobody asked about, and there is no value that means "this
		 * did not happen".  So it does not happen, and says so.
		 */
		if (p == 0) {
			printf("pci: write to %04x:%02x:%02x.%u reg 0x%x is "
			       "outside the mapped window — dropped\n",
			       segment, bus, dev, func, reg);
			return;
		}
		*p = value;
		return;
	}

	if (segment != 0) {
		printf("pci: write to segment %u cannot be expressed through "
		       "the port pair — dropped\n", segment);
		return;
	}

	outl(PCI_CONFIG_ADDRESS,
	     PCI_CONFIG_ENABLE
	     | ((uint32_t)bus << 16)
	     | ((uint32_t)dev << 11)
	     | ((uint32_t)func << 8)
	     | ((uint32_t)reg & 0xFCu));
	outl(PCI_CONFIG_DATA, value);
}

/*
 * ── Reading less than a dword ────────────────────────────────────────
 *
 * Both mechanisms address dwords, and the capability list does not: its
 * offsets are byte-granular, so a capability at 0x52 lives in the dword at
 * 0x50, sixteen bits up.  Said once here rather than at every reader.
 *
 * ⚠️ Little-endian, which is a property of the bus and not only of this
 * processor: PCI configuration space is defined little-endian, so the byte at
 * offset n is bits 8n..8n+7 of the dword containing it whatever the CPU is.
 */
uint8_t
pci_cfg_read8(uint16_t segment, uint8_t bus, uint8_t dev, uint8_t func,
	      uint16_t reg)
{
	uint32_t dword = pci_cfg_read(segment, bus, dev, func, reg & ~3u);

	return (uint8_t)(dword >> ((reg & 3u) * 8));
}

uint16_t
pci_cfg_read16(uint16_t segment, uint8_t bus, uint8_t dev, uint8_t func,
	       uint16_t reg)
{
	uint32_t dword = pci_cfg_read(segment, bus, dev, func, reg & ~3u);

	return (uint16_t)(dword >> ((reg & 2u) * 8));
}

/*
 * ── Walking the capability list ──────────────────────────────────────
 *
 * See <cpu/pci_cfg.h> for why this is in the kernel, why the status bit is
 * consulted before the pointer, and why the walk is bounded.
 */
#define	PCI_CAP_MAX_HOPS	48

uint16_t
pci_cfg_find_cap(uint16_t segment, uint8_t bus, uint8_t dev, uint8_t func,
		 uint8_t cap_id)
{
	uint16_t	status;
	uint16_t	offset;
	unsigned	hops;

	status = pci_cfg_read16(segment, bus, dev, func, PCI_STATUS);

	/*
	 * ⚠️ 0xFFFF is what an absent function answers, and it has the
	 * capability-list bit set.  Checked before the bit, or every empty
	 * slot on the bus appears to have a list.
	 */
	if (status == 0xFFFFu || !(status & PCI_STATUS_CAP_LIST))
		return 0;

	offset = pci_cfg_read8(segment, bus, dev, func, PCI_CAP_POINTER);

	for (hops = 0; hops < PCI_CAP_MAX_HOPS; hops++) {
		uint8_t id;

		/*
		 * The list lives above the 64-byte standard header and inside
		 * the 256-byte space, and the entries are dword-aligned.  A
		 * pointer outside that is not a capability this walk can
		 * follow -- and following it anyway is how a malformed device
		 * makes the kernel read its own neighbours.
		 */
		offset &= 0xFCu;
		if (offset < 0x40u)
			return 0;

		id = pci_cfg_read8(segment, bus, dev, func, offset);
		if (id == cap_id)
			return offset;

		offset = pci_cfg_read8(segment, bus, dev, func,
				       (uint16_t)(offset + 1));
	}

	printf("pci: %04x:%02x:%02x.%u capability list did not end in %u hops "
	       "— giving up rather than following it\n",
	       segment, bus, dev, func, PCI_CAP_MAX_HOPS);
	return 0;
}
