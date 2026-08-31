/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 *	pci_msix.c — the MSI-X table, written from this side (#457).
 *
 *	See <cpu/pci_msix.h> for why it is written from this side.
 */

#include <stdint.h>

#include <cpu/pci_cfg.h>
#include <cpu/pci_msix.h>
#include <device/pci.h>
#include <kern/misc_protos.h>
#include <pmap/pmap.h>

#include "pci_bar.h"	/* the decode hal_server uses, and now so does this */

int
pci_msix_probe(uint16_t segment, uint8_t bus, uint8_t dev, uint8_t func,
	       struct pci_msix *out)
{
	uint32_t		slots[PCI_NUM_BAR_SLOTS];
	struct pci_bar_region	regions[PCI_NUM_BAR_SLOTS];
	unsigned int		n_regions;
	uint16_t		cap, control;
	uint32_t		table;
	unsigned int		bir;
	uint64_t		offset, base, need, va;
	unsigned int		i;

	if (out == 0)
		return 0;

	out->table = 0;

	cap = pci_cfg_find_cap(segment, bus, dev, func, PCI_CAP_ID_MSIX);
	if (cap == 0)
		return 0;

	control = pci_cfg_read16(segment, bus, dev, func,
				 (uint16_t)(cap + PCI_MSIX_CONTROL));
	table   = pci_cfg_read(segment, bus, dev, func,
			       (uint16_t)(cap + PCI_MSIX_TABLE));

	bir    = table & PCI_MSIX_BIR_MASK;
	offset = table & PCI_MSIX_OFFSET_MASK;

	/*
	 * ⚠️ The six raw slots, decoded rather than indexed.  The BIR names a
	 * SLOT, and a slot is not a region: a 64-bit BAR takes two of them, so
	 * the region a slot belongs to has to be found by looking, which is
	 * exactly what #427 wrote this decode for.
	 */
	for (i = 0; i < PCI_NUM_BAR_SLOTS; i++)
		slots[i] = pci_cfg_read(segment, bus, dev, func,
					(uint16_t)PCI_BAR(i));

	n_regions = pci_bars_decode(slots, PCI_NUM_BAR_SLOTS, regions,
				    PCI_NUM_BAR_SLOTS);

	base = 0;
	for (i = 0; i < n_regions; i++)
		if (regions[i].slot == bir
		    && !(regions[i].flags & PCI_REGION_IO)) {
			base = regions[i].base;
			break;
		}

	if (base == 0) {
		printf("pci: %04x:%02x:%02x.%u names BAR %u for its MSI-X "
		       "table and that slot holds no memory region\n",
		       segment, bus, dev, func, bir);
		return 0;
	}

	out->segment = segment;
	out->bus     = bus;
	out->dev     = dev;
	out->func    = func;
	out->cap     = cap;
	out->vectors = (control & PCI_MSIX_CTL_TABLE_SIZE) + 1u;

	/*
	 * ⚠️ The size the region reports is not consulted here, and saying why
	 * is better than leaving a check that cannot run: sizing a BAR means
	 * WRITING all ones to it and reading back, which is a write to a device
	 * nobody has claimed -- the thing #513 exists to make decidable.  So
	 * the mapping is of exactly what the table needs and no more, and an
	 * offset that is wrong will fault or read as ones rather than silently
	 * reach a neighbour.
	 */
	need = offset + (uint64_t)out->vectors * PCI_MSIX_ENTRY_SIZE;

	va = pmap_map_device(base + (offset & ~0xFFFULL),
			     ((need - (offset & ~0xFFFULL)) + 0xFFFULL)
			     & ~0xFFFULL);
	if (va == 0) {
		printf("pci: %04x:%02x:%02x.%u MSI-X table at 0x%x could not "
		       "be mapped\n", segment, bus, dev, func,
		       (unsigned)(base + offset));
		return 0;
	}

	out->table = (volatile uint8_t *)(uintptr_t)
		     (va + (offset & 0xFFFULL));
	return 1;
}

static volatile uint32_t *
msix_entry(const struct pci_msix *m, unsigned int entry, unsigned int field)
{
	return (volatile uint32_t *)(m->table
				     + entry * PCI_MSIX_ENTRY_SIZE + field);
}

void
pci_msix_arm(const struct pci_msix *m, unsigned int entry, uint64_t address,
	     uint32_t data)
{
	if (m == 0 || m->table == 0 || entry >= m->vectors)
		return;

	/*
	 * ⚠️ Masked while it is written and armed by the last store.  The
	 * entry is three dwords, and a device that read a half-written one
	 * would pair the new address with the old value or the reverse -- and
	 * a wrong pair is a write to a wrong address, not a missing interrupt.
	 */
	*msix_entry(m, entry, PCI_MSIX_ENTRY_CTL) = PCI_MSIX_ENTRY_MASKED;

	*msix_entry(m, entry, PCI_MSIX_ENTRY_ADDR_LO) = (uint32_t)address;
	*msix_entry(m, entry, PCI_MSIX_ENTRY_ADDR_HI) =
		(uint32_t)(address >> 32);
	*msix_entry(m, entry, PCI_MSIX_ENTRY_DATA) = data;

	*msix_entry(m, entry, PCI_MSIX_ENTRY_CTL) = 0;
}

void
pci_msix_disarm(const struct pci_msix *m, unsigned int entry)
{
	if (m == 0 || m->table == 0 || entry >= m->vectors)
		return;

	*msix_entry(m, entry, PCI_MSIX_ENTRY_CTL) = PCI_MSIX_ENTRY_MASKED;
}

void
pci_msix_read(const struct pci_msix *m, unsigned int entry,
	      uint64_t *address_out, uint32_t *data_out, uint32_t *control_out)
{
	if (m == 0 || m->table == 0 || entry >= m->vectors)
		return;

	if (address_out != 0)
		*address_out =
			(uint64_t)*msix_entry(m, entry, PCI_MSIX_ENTRY_ADDR_LO)
			| ((uint64_t)*msix_entry(m, entry, PCI_MSIX_ENTRY_ADDR_HI)
			   << 32);
	if (data_out != 0)
		*data_out = *msix_entry(m, entry, PCI_MSIX_ENTRY_DATA);
	if (control_out != 0)
		*control_out = *msix_entry(m, entry, PCI_MSIX_ENTRY_CTL);
}

void
pci_msix_enable(const struct pci_msix *m)
{
	uint16_t	control;
	uint32_t	command;

	if (m == 0 || m->table == 0)
		return;

	/*
	 * Bus master first, because it is the one whose absence does not look
	 * like itself: an MSI is a memory write BY THE DEVICE, and a device
	 * that is not a bus master cannot issue one -- which reads as a wrong
	 * address rather than as a device that was never allowed to speak.
	 */
	command = pci_cfg_read(m->segment, m->bus, m->dev, m->func,
			       PCI_COMMAND);
	pci_cfg_write(m->segment, m->bus, m->dev, m->func, PCI_COMMAND,
		      command | PCI_CMD_BUS_MASTER | PCI_CMD_MEM_ENABLE);

	control = pci_cfg_read16(m->segment, m->bus, m->dev, m->func,
				 (uint16_t)(m->cap + PCI_MSIX_CONTROL));
	control |= PCI_MSIX_CTL_ENABLE;
	control &= (uint16_t)~PCI_MSIX_CTL_FUNC_MASK;

	/*
	 * ⚠️ A 16-bit field written through a 32-bit access, so the other half
	 * of the dword is read and put back rather than zeroed.  The control
	 * word is at cap+2, which makes its dword cap+0 -- whose low half is
	 * the capability id and the next pointer.  Zeroing those would unlink
	 * this capability from the device's own list.
	 */
	{
		uint32_t dword = pci_cfg_read(m->segment, m->bus, m->dev,
					      m->func, (uint16_t)m->cap);

		dword = (dword & 0x0000FFFFu) | ((uint32_t)control << 16);
		pci_cfg_write(m->segment, m->bus, m->dev, m->func,
			      (uint16_t)m->cap, dword);
	}
}

/*
 * The opposite of the above, which did not exist (#520).
 *
 * 🔴 ENABLING WAS NOT UNDOABLE, and that is not a tidiness complaint.  A task
 * that registered a message-signalled vector and gave it back left the DEVICE
 * with MSI-X switched on: device_md_msi_unregister() disarmed the entry, which
 * is the part that could deliver an interrupt, and nothing put the function's
 * own enable bit back.
 *
 * 🔑 So a device that a test merely TOUCHED was handed to its driver in a state
 * the driver never asked for -- and for one real device that state moves its
 * registers.  A legacy virtio-pci device puts its device-specific
 * configuration at offset 20 when MSI-X is disabled and at 24 when it is
 * enabled, so virtio-blk read its capacity out of two MSI-X vector fields and
 * reported 65535 sectors for a 32768-sector disk.  It was found because
 * irq_claim_test walks every device on bus 0 offering one a vector, ten tasks
 * before the driver starts.
 *
 * ⚠️ The bus-master bit is NOT put back.  Enabling set it because an MSI is a
 * write by the device, but a device may be a bus master for a hundred reasons
 * that have nothing to do with interrupts -- clearing it here would break DMA
 * for a driver that never asked about MSI at all.  Undoing exactly what is
 * being undone is the whole point of this function existing.
 */
void
pci_msix_disable(const struct pci_msix *m)
{
	uint16_t	control;

	if (m == 0 || m->table == 0)
		return;

	control = pci_cfg_read16(m->segment, m->bus, m->dev, m->func,
				 (uint16_t)(m->cap + PCI_MSIX_CONTROL));
	control &= (uint16_t)~PCI_MSIX_CTL_ENABLE;

	/* Same read-modify-write of the other half as above, same reason. */
	{
		uint32_t dword = pci_cfg_read(m->segment, m->bus, m->dev,
					      m->func, (uint16_t)m->cap);

		dword = (dword & 0x0000FFFFu) | ((uint32_t)control << 16);
		pci_cfg_write(m->segment, m->bus, m->dev, m->func,
			      (uint16_t)m->cap, dword);
	}
}
