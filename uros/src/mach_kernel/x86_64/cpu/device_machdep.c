/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 *	device_machdep.c — x86-64's answers to <device/device_machdep.h> (#457).
 *
 *	Thin on purpose.  Everything here already existed and was reachable
 *	under a different name; what was missing was not machinery but the
 *	place where the device master could ask for it without knowing which
 *	machine it was on.
 */

#include <device/device_machdep.h>

#include <cpu/acpi.h>
#include <cpu/ioapic.h>
#include <cpu/pci_cfg.h>
#include <cpu/regs.h>	/* inb/outl and the other widths */

unsigned int
device_md_pci_read(unsigned int bus, unsigned int slot, unsigned int func,
		   unsigned int reg)
{
	return pci_cfg_read(0, (uint8_t)bus, (uint8_t)slot, (uint8_t)func,
			    (uint16_t)reg);
}

void
device_md_pci_write(unsigned int bus, unsigned int slot, unsigned int func,
		    unsigned int reg, unsigned int value)
{
	pci_cfg_write(0, (uint8_t)bus, (uint8_t)slot, (uint8_t)func,
		      (uint16_t)reg, (uint32_t)value);
}

/*
 * The MADT's interrupt-source overrides carry a two-bit trigger-mode field:
 * 0 means "as the bus does it", 1 edge, 3 level.  For an ISA interrupt "as
 * the bus does it" is edge, which is why the default is not level.
 *
 * ⚠️ There is no 8259 in this answer.  i386 asks whether an I/O APIC is in
 * use before deciding whom to ask; here there is nothing else to ask, and a
 * machine with no I/O APIC has no interrupts routed at all -- which
 * ioapic_present() reports rather than this guessing from a level.
 */
#define	MADT_TRIGGER_MASK	0x0Cu
#define	MADT_TRIGGER_LEVEL	0x0Cu

int
device_md_irq_is_level(unsigned int irq)
{
	if (!ioapic_present())
		return 0;

	return (acpi_irq_flags((uint8_t)irq) & MADT_TRIGGER_MASK)
		== MADT_TRIGGER_LEVEL;
}

/*
 * ⚠️ The ISA interrupt number is translated to a global system interrupt
 * before the pin is touched.  They are not the same number: the firmware may
 * say that ISA 0 arrives on GSI 2, and it usually does.  Masking pin 0
 * because the caller said 0 would leave the timer running and silence
 * something else.
 */
void
device_md_irq_mask(unsigned int irq)
{
	if (ioapic_present())
		ioapic_mask(acpi_irq_to_gsi((uint8_t)irq));
}

void
device_md_irq_unmask(unsigned int irq)
{
	if (ioapic_present())
		ioapic_unmask(acpi_irq_to_gsi((uint8_t)irq));
}

unsigned int
device_md_io_read(unsigned int port, unsigned int size)
{
	switch (size) {
	case 1:	return inb((uint16_t)port);
	case 2:	return inw((uint16_t)port);
	case 4:	return inl((uint16_t)port);
	}
	return 0;
}

void
device_md_io_write(unsigned int port, unsigned int size, unsigned int value)
{
	switch (size) {
	case 1:	outb((uint16_t)port, (uint8_t)value); break;
	case 2:	outw((uint16_t)port, (uint16_t)value); break;
	case 4:	outl((uint16_t)port, (uint32_t)value); break;
	}
}
