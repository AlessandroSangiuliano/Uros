/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 *	device_machdep.h — what the device master needs from the machine (#457).
 *
 *	device_master.c is fourteen RPCs through which a user-space driver
 *	reaches hardware, and almost none of it is machine-dependent: the port
 *	checks, the task lookups, the mapping and the interrupt bookkeeping are
 *	the same everywhere.  What differs is six operations, and they are
 *	named here so the RPC file can be read without knowing which machine it
 *	is on.
 *
 *	🔑 A contract and not a ladder of #if.  The file used to include
 *	<i386/pci/pci.h>, <i386/pci/pcibios.h>, <i386/ipl.h>, <i386/pio.h>,
 *	<i386/misc_protos.h> and <i386/ioapic.h> outside any conditional --
 *	which is why compiling it for x86-64 pulled in <mach/i386/vm_types.h>
 *	behind them, where vm_offset_t is thirty-two bits.  Six declarations in
 *	one place cost less than six conditionals in six, and the next machine
 *	inherits a list of what it has to answer rather than a diff to make.
 *
 *	⚠️ The two implementations are not the same shape underneath, and that
 *	is the point of naming the operation instead of the mechanism:
 *
 *	  configuration space   i386 reaches it through the BIOS-era tag;
 *	                        x86-64 through ECAM where the machine has one
 *	                        and the port pair where it does not
 *
 *	  masking an interrupt  i386 has an 8259 behind an I/O APIC that may or
 *	                        may not be in use; x86-64 has no 8259 in the
 *	                        picture at all and masks the pin
 */

#ifndef	_DEVICE_DEVICE_MACHDEP_H_
#define	_DEVICE_DEVICE_MACHDEP_H_

#include <mach/machine/vm_types.h>

/*
 * One aligned 32-bit configuration register.
 *
 * ⚠️ Addressed by bus/device/function and not by a tag: a tag is a thing one
 * of the two mechanisms happens to have, and the RPC does not have one to
 * give.  Segment zero is implied -- the interface above has no field for a
 * segment either, which is #427's other half and is written down there.
 *
 * A read of a function that is not there answers 0xFFFFFFFF, because that is
 * what the bus returns.  A caller enumerating has to know that number.
 */
extern unsigned int	device_md_pci_read(unsigned int bus, unsigned int slot,
					   unsigned int func, unsigned int reg);
extern void		device_md_pci_write(unsigned int bus, unsigned int slot,
					    unsigned int func, unsigned int reg,
					    unsigned int value);

/*
 * Is this interrupt level-triggered?
 *
 * Asked because a level-triggered line has to be masked until the driver
 * says it has been serviced, and an edge-triggered one does not.  A machine
 * that cannot tell answers 0: treating a level line as edge loses interrupts,
 * treating an edge line as level does not, so the safe direction is the one
 * that keeps the mask in place.
 */
extern int		device_md_irq_is_level(unsigned int irq);

/* Stop and resume delivery of one interrupt. */
extern void		device_md_irq_mask(unsigned int irq);
extern void		device_md_irq_unmask(unsigned int irq);

/*
 * One I/O port access, `size' bytes wide (1, 2 or 4).
 *
 * 🔑 Here rather than behind a per-machine <io.h>, because the operation is
 * what the RPC has and the instruction is what the machine has.  i386 spells
 * the port type i386_ioport_t and x86-64 has no such name; naming the access
 * means neither spelling reaches this file.
 *
 * ⚠️ A size that is not 1, 2 or 4 reads as zero and writes nothing.  The RPC
 * above validates it, and this is the second half of that check rather than a
 * repetition of it: a machine asked for a width it does not have must not
 * pick the nearest one.
 */
extern unsigned int	device_md_io_read(unsigned int port, unsigned int size);
extern void		device_md_io_write(unsigned int port, unsigned int size,
					   unsigned int value);

#endif	/* _DEVICE_DEVICE_MACHDEP_H_ */
