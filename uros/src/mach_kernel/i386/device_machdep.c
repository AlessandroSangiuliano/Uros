/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 *	device_machdep.c — i386's answers to <device/device_machdep.h> (#457).
 *
 *	🔑 This is a MOVE and not a port.  Every line below was in
 *	device/device_master.c, calling the same functions with the same
 *	arguments; the only change is that it is now behind a name the RPC file
 *	can use without including <i386/...> outside a conditional.
 *
 *	⚠️ Which is what makes it checkable: i386's behaviour has to be
 *	unchanged, and "unchanged" is a claim about generated code rather than
 *	about intent.
 */

#include <device/device_machdep.h>

#include <i386/pci/pci.h>
#include <i386/pci/pcibios.h>
#include <i386/ioapic.h>
#include <i386/misc_protos.h>
#include <i386/pio.h>

unsigned int
device_md_pci_read(unsigned int bus, unsigned int slot, unsigned int func,
		   unsigned int reg)
{
	pcici_t tag = pcitag((unsigned char)bus,
			     (unsigned char)slot,
			     (unsigned char)func);

	/*
	 * A tag the BIOS interface could not form names no device, and the bus
	 * answers for an absent function with all ones.  Saying so here keeps
	 * the caller from having to know that pcitag has a failure at all.
	 */
	if (!tag.cfg1)
		return 0xFFFFFFFFu;

	return (unsigned int)pci_conf_read(tag, reg);
}

void
device_md_pci_write(unsigned int bus, unsigned int slot, unsigned int func,
		    unsigned int reg, unsigned int value)
{
	pcici_t tag = pcitag((unsigned char)bus,
			     (unsigned char)slot,
			     (unsigned char)func);

	if (!tag.cfg1)
		return;

	pci_conf_write(tag, reg, value);
}

/*
 * ⚠️ Asked of the I/O APIC only when one is in use.  With the 8259 alone the
 * trigger mode is whatever the bus does, and this machine has no table that
 * says otherwise -- so it answers edge, which is the direction that does not
 * lose interrupts if it is wrong.
 */
int
device_md_irq_is_level(unsigned int irq)
{
	if (ioapic_active())
		return ioapic_irq_is_level(irq);
	return 0;
}

void
device_md_irq_mask(unsigned int irq)
{
	pic_irq_mask(irq);
}

void
device_md_irq_unmask(unsigned int irq)
{
	pic_irq_unmask(irq);
}

/*
 * ⚠️ i386_ioport_t, which is a name only this machine has.  Keeping the cast
 * here is the reason device_master.c no longer needs to know it exists.
 */
unsigned int
device_md_io_read(unsigned int port, unsigned int size)
{
	switch (size) {
	case 1:	return inb((i386_ioport_t)port);
	case 2:	return inw((i386_ioport_t)port);
	case 4:	return inl((i386_ioport_t)port);
	}
	return 0;
}

void
device_md_io_write(unsigned int port, unsigned int size, unsigned int value)
{
	switch (size) {
	case 1:	outb((i386_ioport_t)port, (unsigned char)value); break;
	case 2:	outw((i386_ioport_t)port, (unsigned short)value); break;
	case 4:	outl((i386_ioport_t)port, (unsigned long)value); break;
	}
}
