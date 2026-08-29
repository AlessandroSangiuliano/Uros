/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 *	pci_cfg.h — reaching PCI configuration space on x86-64 (#457).
 *
 *	🔑 TWO mechanisms, and this is not a bridge to be removed later.  They
 *	belong to two different machines, and both of them are machines we run
 *	on:
 *
 *	  ports   0xCF8 selects a register and 0xCFC carries it.  Present on
 *	          every PC ever made, and the only way in on QEMU's default
 *	          board -- an i440FX, which models a chipset from 1996.
 *
 *	  ECAM    configuration space mapped into physical memory, with the
 *	          MCFG table saying where.  Present from Q35 (2009) onward, and
 *	          so on every real machine this port is aimed at.
 *
 *	The port pair is not merely older.  It is two 32-bit registers that
 *	every access on every processor has to take turns on, it has no room to
 *	name a PCI segment at all, and it cannot reach past bus 255.  ECAM has
 *	none of those three properties, which is why a machine that offers it
 *	is asked first.
 *
 *	⚠️ Measured rather than assumed, on the two boards this tree boots:
 *
 *	    -machine pc  (i440FX)   acpi_ecam_base(0, 0) -> 0
 *	    -machine q35 (ICH9)     acpi_ecam_base(0, 0) -> 0xb0000000
 *
 *	So a build that only did ECAM would find nothing on the board the
 *	harness runs by default, and one that only did ports could never
 *	address the machines the fleet is being ported for.
 */

#ifndef	_X86_64_CPU_PCI_CFG_H_
#define	_X86_64_CPU_PCI_CFG_H_

#include <stdint.h>

/*
 * Decide which mechanism this machine offers, and map what needs mapping.
 * Called once, before any access.  Says on the console which one it chose:
 * ⚠️ the choice changes what can be addressed -- a machine on ports cannot
 * describe a device past bus 255 or in a second segment -- so it is not a
 * detail to be inferred later from a symptom.
 */
extern void pci_cfg_init(void);

/*
 * Read and write one aligned 32-bit configuration register.
 *
 * `reg' is a byte offset and must be a multiple of four: both mechanisms
 * address dwords, and rounding one down silently would return a neighbour.
 *
 * ⚠️ A read of a function that is not there answers 0xFFFFFFFF, which is what
 * the bus returns and not an error this layer invents.  A caller enumerating
 * has to know that number; a caller that already knows the device is there
 * can treat it as a fault.
 */
extern uint32_t pci_cfg_read(uint16_t segment, uint8_t bus, uint8_t dev,
			     uint8_t func, uint16_t reg);
extern void	pci_cfg_write(uint16_t segment, uint8_t bus, uint8_t dev,
			      uint8_t func, uint16_t reg, uint32_t value);

/*
 * Whether configuration space is reached through ECAM on this machine.
 * For the boot message and for callers that must refuse what the port pair
 * cannot express, rather than issue it and get a neighbouring device.
 */
extern int pci_cfg_is_ecam(void);

#endif	/* _X86_64_CPU_PCI_CFG_H_ */
