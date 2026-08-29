/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 *	pci_msix.h — programming a device to interrupt without a wire (#457).
 *
 *	🔴 THE KERNEL PROGRAMS THIS TABLE, AND A DRIVER NEVER WILL.
 *
 *	An MSI-X table entry holds an address and a 32-bit value, and the
 *	device raises its interrupt by WRITING that value to that address.  As
 *	far as the device is concerned it is an ordinary store: the address is
 *	an ordinary physical address, and what makes it an interrupt is only
 *	that 0xFEE00000 and up is where the local APICs answer.
 *
 *	So a driver that could write its own table could put any address
 *	there.  Page zero, another task's pages, another processor's APIC --
 *	the device would faithfully write to whichever, and no fault would be
 *	taken because the write is the device's and not the driver's.  That is
 *	the same hole #432 exists to close for DMA and #511 for the master
 *	port, arriving through a different door.
 *
 *	⚠️ Which is why the table lives behind this file rather than behind
 *	`map the BAR and let the driver at it'.  The driver says which device
 *	and which entry; the kernel decides the address and the value.
 *
 *	── Where the table is ────────────────────────────────────────────────
 *
 *	Not in configuration space.  The capability names a BAR -- the `BIR',
 *	three bits of it -- and an offset into that BAR, so finding the table
 *	means decoding a base address register first.  Which is why this file
 *	uses the same pci_bars_decode() hal_server uses: the table is in a
 *	region, and a region is exactly the thing six raw slots do not tell you
 *	about directly (#427).
 */

#ifndef	_X86_64_CPU_PCI_MSIX_H_
#define	_X86_64_CPU_PCI_MSIX_H_

#include <stdint.h>

/*
 * A device's MSI-X, found and mapped.
 *
 * Held by the caller rather than in a table here, because the caller is the
 * one that knows how long it wants the device programmed for -- and because a
 * second table indexed by bus/device/function would be a second place for the
 * question "is this device set up?" to be answered differently.
 */
struct pci_msix {
	uint16_t		segment;
	uint8_t			bus;
	uint8_t			dev;
	uint8_t			func;
	uint16_t		cap;		/* offset in config space  */
	unsigned int		vectors;	/* entries the table holds */
	volatile uint8_t	*table;		/* mapped, or zero         */
};

/*
 * Find the capability, decode the BAR it names, and map the table.
 *
 * Answers zero if the device has no MSI-X, if the BAR the capability names
 * holds no region, or if the table would run past the end of that region --
 * ⚠️ the last one checked rather than trusted, because table size and BAR
 * size come from two different registers written by two different people, and
 * a table that overruns its BAR is a kernel writing into whatever the next
 * region is.
 *
 * ⚠️ Leaves every entry MASKED, which is the state a reset device is already
 * in.  An entry is armed by pci_msix_arm() and by nothing else, so a device
 * cannot start interrupting between being probed and being given a vector.
 */
extern int pci_msix_probe(uint16_t segment, uint8_t bus, uint8_t dev,
			  uint8_t func, struct pci_msix *out);

/*
 * Point one entry at one address with one value, and unmask it.
 *
 * ⚠️ The mask bit is cleared LAST.  The address and the data are two dwords
 * plus a third, and a device that read a half-written entry would write the
 * new address with the old value or the reverse -- so the entry is completed
 * while masked and armed in one final store.
 */
extern void pci_msix_arm(const struct pci_msix *m, unsigned int entry,
			 uint64_t address, uint32_t data);

/*
 * What the device thinks entry `entry' says, read back out of its own table.
 *
 * For the test that has to distinguish "the table was written" from "a write
 * went somewhere" -- the arithmetic that finds the table is a BAR decode, a
 * BIR and an offset, and every one of those can be wrong in a way that lands
 * the store on real memory and reports success.
 */
extern void pci_msix_read(const struct pci_msix *m, unsigned int entry,
			  uint64_t *address_out, uint32_t *data_out,
			  uint32_t *control_out);

/*
 * Let the device use the table.
 *
 * Three bits in two registers, and all three are needed:
 *
 *   MSI-X enable      in the capability's control word
 *   function mask     cleared in the same word -- it masks every vector
 *                     whatever the per-entry bits say, and a reset device has
 *                     it set
 *   bus master        in the COMMAND register.  ⚠️ Easy to forget, and its
 *                     absence looks like a wrong address rather than a
 *                     disabled device: an MSI is a memory write BY THE
 *                     DEVICE, and a device that is not a bus master cannot
 *                     issue one at all.
 *
 * Memory space is enabled too, because the table is in a BAR and a device
 * whose memory decode is off cannot see its own table.
 */
extern void pci_msix_enable(const struct pci_msix *m);

#endif	/* _X86_64_CPU_PCI_MSIX_H_ */
