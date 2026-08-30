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
 * holds no region, or if the table could not be mapped.
 *
 * 🔴 IT DOES NOT CHECK THAT THE TABLE FITS INSIDE THAT BAR, and the reason is
 * worth more than the check would be: knowing a BAR's SIZE means writing all
 * ones into it and reading back what sticks, which is a write to a device
 * nobody has claimed -- exactly the thing #513 exists to make decidable.  So
 * this maps precisely what the table's own entry count needs, and a wrong
 * offset reads as all-ones or faults rather than quietly reaching a
 * neighbouring region.  ⚠️ An earlier version of this comment claimed the
 * check; the code never did it.
 *
 * ⚠️ It does not mask the entries either.  A device out of reset has every
 * entry masked and its function mask set, and probe leaves that alone -- so
 * the guarantee is the hardware's and not this function's.  A device someone
 * else already armed is not covered by either, which is what
 * pci_msix_disarm() is for.
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
 * Stop the entry being used, whatever the device's own masks say.
 *
 * 🔴 THE CALLER MUST DO THIS BEFORE GIVING UP THE SLOT.  Releasing a
 * message-signalled slot removes the handler and nothing else -- see
 * device_md_msi_unregister(), which says so and cannot do better, because it
 * does not know which device was programmed with the address.  A device left
 * armed at a vector whose handler is gone raises an interrupt nobody claims,
 * and an unclaimed vector falls through trap_dispatch() into the fault
 * machinery and halts the machine.
 *
 * ⚠️ Masking the CAUSES in the device's own registers is not this.  That is a
 * bit in a register belonging to the driver's half of the world, which the
 * next thing to touch that device may set again; this is a bit in the table
 * the kernel owns, and it stops the write at its source.
 */
extern void pci_msix_disarm(const struct pci_msix *m, unsigned int entry);

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

/*
 * ── The vector half, which is not PCI's ──────────────────────────────
 *
 * Claim a message-signalled vector and answer what a device must WRITE to
 * reach it.  Defined in cpu/device_machdep.c, where the slots and the
 * trampoline live, and declared here because this is where the other half of
 * the same story is -- a table entry is exactly an address and a value, and
 * these are the address and the value.
 *
 * 🔴 It hands out an ADDRESS, which is the thing <device/device_machdep.h>
 * refuses to hand out.  The difference between the two is not what they do
 * but who may call them: this one is for the kernel, and for the boot
 * self-test that has no device to program.  A driver reaches the other.
 */
extern int msi_claim_vector(void (*handler)(int), unsigned int *slot_out,
			    unsigned long long *address_out,
			    unsigned int *data_out);

#endif	/* _X86_64_CPU_PCI_MSIX_H_ */
