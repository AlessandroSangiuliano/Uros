/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 *	pci_msi.c — a device's MSI, written from this side (#977).
 *
 *	See <cpu/pci_msi.h> for why, and <device/pci.h> for the layout, which
 *	is AHCI 1.3.1 §2.3's.
 */

#include <stdint.h>

#include <cpu/pci_cfg.h>
#include <cpu/pci_msi.h>
#include <device/pci.h>

/*
 * The control word at cap+2, written through its dword at cap+0, whose low
 * half is the capability's id and next pointer and is put back as read:
 * zeroed, it would unlink the capability from the function's list.  The same
 * read-modify-write as pci_msix_enable()'s, for the same reason.
 */
static void
msi_control_write(const struct pci_msi *m, uint16_t control)
{
	uint32_t dword = pci_cfg_read(m->segment, m->bus, m->dev, m->func,
				      m->cap);

	dword = (dword & 0x0000FFFFu) | ((uint32_t)control << 16);
	pci_cfg_write(m->segment, m->bus, m->dev, m->func, m->cap, dword);
}

static uint16_t
msi_control_read(const struct pci_msi *m)
{
	return pci_cfg_read16(m->segment, m->bus, m->dev, m->func,
			      (uint16_t)(m->cap + PCI_MSI_CONTROL));
}

static uint16_t
msi_data_offset(const struct pci_msi *m)
{
	return (uint16_t)(m->cap + (m->addr64 ? PCI_MSI_DATA_64
					      : PCI_MSI_DATA_32));
}

int
pci_msi_probe(uint16_t segment, uint8_t bus, uint8_t dev, uint8_t func,
	      struct pci_msi *out)
{
	uint16_t cap;

	if (out == 0)
		return 0;
	out->cap = 0;

	cap = pci_cfg_find_cap(segment, bus, dev, func, PCI_CAP_ID_MSI);
	if (cap == 0)
		return 0;

	out->segment = segment;
	out->bus     = bus;
	out->dev     = dev;
	out->func    = func;
	out->cap     = cap;
	out->addr64  = (pci_cfg_read16(segment, bus, dev, func,
				       (uint16_t)(cap + PCI_MSI_CONTROL))
			& PCI_MSI_CTL_64BIT) != 0;
	return 1;
}

int
pci_msi_arm(const struct pci_msi *m, uint64_t address, uint32_t data)
{
	uint16_t	control;
	uint32_t	dword;

	if (m == 0 || m->cap == 0)
		return 0;
	if ((address >> 32) != 0 && !m->addr64)
		return 0;
	if (data > 0xFFFFu)
		return 0;

	/* Disabled first, and one message: what follows is not yet a message. */
	control = msi_control_read(m);
	control &= (uint16_t)~(PCI_MSI_CTL_ENABLE | PCI_MSI_CTL_MME);
	msi_control_write(m, control);

	pci_cfg_write(m->segment, m->bus, m->dev, m->func,
		      (uint16_t)(m->cap + PCI_MSI_ADDR), (uint32_t)address);
	if (m->addr64)
		pci_cfg_write(m->segment, m->bus, m->dev, m->func,
			      (uint16_t)(m->cap + PCI_MSI_ADDR_HI),
			      (uint32_t)(address >> 32));

	/*
	 * The data is the low 16 bits of its dword, and the upper half is put
	 * back as read rather than zeroed: it is not this capability's to
	 * decide in AHCI 1.3.1's layout.
	 */
	dword = pci_cfg_read(m->segment, m->bus, m->dev, m->func,
			     msi_data_offset(m));
	dword = (dword & 0xFFFF0000u) | data;
	pci_cfg_write(m->segment, m->bus, m->dev, m->func, msi_data_offset(m),
		      dword);

	/* A message is a write by the device: no bus master, no message. */
	pci_cfg_command(m->segment, m->bus, m->dev, m->func,
			PCI_CMD_BUS_MASTER, 0);

	msi_control_write(m, (uint16_t)(control | PCI_MSI_CTL_ENABLE));
	return 1;
}

void
pci_msi_disarm(const struct pci_msi *m)
{
	if (m == 0 || m->cap == 0)
		return;
	msi_control_write(m, (uint16_t)(msi_control_read(m)
					& ~PCI_MSI_CTL_ENABLE));
}

void
pci_msi_read(const struct pci_msi *m, uint64_t *address_out,
	     uint32_t *data_out, uint16_t *control_out)
{
	uint64_t address = 0;
	uint32_t data = 0;
	uint16_t control = 0;

	if (m != 0 && m->cap != 0) {
		address = pci_cfg_read(m->segment, m->bus, m->dev, m->func,
				       (uint16_t)(m->cap + PCI_MSI_ADDR));
		if (m->addr64)
			address |= (uint64_t)pci_cfg_read(m->segment, m->bus,
							  m->dev, m->func,
							  (uint16_t)(m->cap
							  + PCI_MSI_ADDR_HI))
				   << 32;
		data = pci_cfg_read(m->segment, m->bus, m->dev, m->func,
				    msi_data_offset(m)) & 0xFFFFu;
		control = msi_control_read(m);
	}
	if (address_out != 0)
		*address_out = address;
	if (data_out != 0)
		*data_out = data;
	if (control_out != 0)
		*control_out = control;
}
