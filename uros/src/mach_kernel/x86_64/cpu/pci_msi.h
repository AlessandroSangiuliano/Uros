/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 *	pci_msi.h — a device's MSI, the message without a table (#977).
 *
 *	🔴 THE KERNEL WRITES THE MESSAGE, AND A DRIVER NEVER WILL, for the
 *	reason <cpu/pci_msix.h> gives for the table: the device writes the
 *	value to the address it holds, as an ordinary store, and a driver that
 *	chose the address could aim the device anywhere.
 *
 *	The capability is smaller than MSI-X's.  The message is in
 *	configuration space itself -- an address, its upper half when the
 *	function can write 64 bits, and 16 bits of data -- so there is no BAR
 *	to decode and nothing to map.  A function may ask for several messages,
 *	a power of two whose data differ in their low bits; this enables one.
 *
 *	⚠️ And one enable bit is the whole of its masking, for a function
 *	without per-vector masking, as AHCI 1.3.1 §2.3 lays the capability out
 *	(the PCI specification is not on our machines).  So the message is
 *	written with MSI disabled, enabled last, and disarming is disabling.
 */

#ifndef	_X86_64_CPU_PCI_MSI_H_
#define	_X86_64_CPU_PCI_MSI_H_

#include <stdint.h>

/*
 * A device's MSI, found.  Held by the caller, as struct pci_msix is, for the
 * reason that header gives.
 */
struct pci_msi {
	uint16_t	segment;
	uint8_t		bus;
	uint8_t		dev;
	uint8_t		func;
	uint16_t	cap;		/* offset in config space, or zero */
	int		addr64;		/* it can write a 64-bit address    */
};

/*
 * Find the capability.  Zero if the function has none.
 */
extern int pci_msi_probe(uint16_t segment, uint8_t bus, uint8_t dev,
			 uint8_t func, struct pci_msi *out);

/*
 * Give the function one message, the address and the data, and enable it.
 *
 * Zero, and nothing written, when the message does not fit: an address above
 * 4 GiB for a function that writes 32 bits, or data wider than its 16.
 *
 * ⚠️ MSI is disabled while the message is written, and enabled by the last
 * store: a function that read a half-written message would write the new
 * address with the old data or the reverse.  And bus mastering is turned on,
 * because a message is a write by the device, as pci_msix_enable() says.
 */
extern int pci_msi_arm(const struct pci_msi *m, uint64_t address,
		       uint32_t data);

/*
 * Stop the function using messages: MSI disabled.  For a function without
 * per-vector masking this is the only way to, and it hands the function back
 * to its pin, as the capability's enable bit says it does.  Bus mastering is
 * left as it is, for the reason pci_msix_disable() gives.
 */
extern void pci_msi_disarm(const struct pci_msi *m);

/*
 * What the function holds: its message and its control word, read back.  For
 * the test that must tell "written" from "a write went somewhere".
 */
extern void pci_msi_read(const struct pci_msi *m, uint64_t *address_out,
			 uint32_t *data_out, uint16_t *control_out);

#endif	/* _X86_64_CPU_PCI_MSI_H_ */
