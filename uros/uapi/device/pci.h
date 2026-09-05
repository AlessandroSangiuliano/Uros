/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

/*
 *	device/pci.h — the PCI configuration header, said once (#427).
 *
 *	It was said four times: in hal_server's pci_scan.c, in ahci.h, again in
 *	ahci_module.c beside the header that already had it, and in
 *	virtio_module.c.  Four copies of a layout that is fixed by a standard is
 *	not four risks of disagreement -- they agreed -- it is four places to
 *	look when the fifth reader wants to know what bit 3 of a BAR means, and
 *	nowhere that says it.
 *
 *	🔑 Only what the standard fixes lives here.  A driver's own registers are
 *	its own business and stay in its own header: this is the part every
 *	device answers the same way because the bus says so.
 */

#ifndef	_DEVICE_PCI_H_
#define	_DEVICE_PCI_H_

/*
 * ── Configuration-space offsets ──────────────────────────────────────
 *
 * Type 0 header (an ordinary device; a bridge is type 1 and differs from
 * 0x10 on, which is why the BAR count below is not six for everything).
 */
#define	PCI_VENDOR_ID		0x00
#define	PCI_DEVICE_ID		0x02
#define	PCI_COMMAND		0x04
#define	PCI_STATUS		0x06
#define	PCI_CLASS_REV		0x08
#define	PCI_HEADER_TYPE		0x0E
#define	PCI_BAR0		0x10

/*
 * ⚠️ 0x0E is a BYTE offset and configuration space is reached a dword at a
 * time, so the header type arrives in the third byte of the dword at 0x0C.
 * Named here rather than shifted at the call site: an off-by-one-byte shift
 * reads the latency timer, which is a small number and looks like a type.
 */
#define	PCI_HDR_DWORD		0x0C
#define	PCI_HEADER_TYPE_OF(d)	(((d) >> 16) & 0xFFu)

#define	PCI_BAR(n)		(PCI_BAR0 + (n) * 4u)
#define	PCI_INTERRUPT_LINE	0x3C

/* The BAR slots a type 0 header has.  SLOTS, not regions -- see below. */
#define	PCI_NUM_BAR_SLOTS	6

/*
 * ── What kind of header this is, and how many BAR slots that leaves ──
 *
 * 🔴 A BRIDGE HAS TWO, AND THE FOUR AFTER THEM ARE NOT BARs.  From 0x18 on, a
 * type 1 header holds the primary, secondary and subordinate bus numbers and
 * the I/O and memory windows the bridge forwards -- so a reader that assumes
 * six decodes a bus topology as if it were addresses, and a WRITER that
 * assumes six reprograms the windows every device behind the bridge is reached
 * through.
 *
 * ⚠️ Which is the difference between this being untidy and being dangerous.
 * Reading six slots on a bridge produced regions nobody used; measuring six
 * writes all ones into them.  The count has to come from the header type
 * before anything touches a slot.
 *
 * Bit 7 of the byte is not part of the type: it says the device has other
 * functions, which is a different question and is asked elsewhere.
 */
#define	PCI_HEADER_TYPE_MASK	0x7Fu
#define	PCI_HEADER_MULTIFUNC	0x80u

#define	PCI_HEADER_TYPE_NORMAL	0x00u	/* an ordinary device: six slots */
#define	PCI_HEADER_TYPE_BRIDGE	0x01u	/* PCI-to-PCI: two, then the windows */
#define	PCI_HEADER_TYPE_CARDBUS	0x02u	/* one, and a layout of its own */

#define	PCI_NUM_BAR_SLOTS_BRIDGE	2

/* ── The command register ─────────────────────────────────────────── */
#define	PCI_CMD_IO_ENABLE	(1u << 0)
#define	PCI_CMD_MEM_ENABLE	(1u << 1)
#define	PCI_CMD_BUS_MASTER	(1u << 2)

/*
 * ── What a base address register says about itself ───────────────────
 *
 * 🔑 The two encodings do not share a layout, and reading one as the other is
 * the defect this file exists to make hard:
 *
 *   memory BAR   bit 0 = 0.  Bits 2:1 are the TYPE, and 0b10 means the
 *                address is 64 bits wide and its upper half is in the NEXT
 *                slot.  Bit 3 is prefetchable.  The address is bits 31:4.
 *
 *   I/O BAR      bit 0 = 1.  Bits 2:1 are not a type at all -- bit 1 is
 *                reserved -- and an I/O BAR is never 64-bit.  The address is
 *                bits 31:2.
 *
 * ⚠️ So the mask depends on which one it is, and the type field means nothing
 * until bit 0 has been read.  virtio_module.c gets this right today for the
 * only reason that counts: it checks `bar0 & 1' before masking with ~3.
 */
#define	PCI_BAR_SPACE_IO	0x1u	/* bit 0: 1 = I/O, 0 = memory */

#define	PCI_BAR_MEM_TYPE_MASK	0x6u	/* bits 2:1, memory BARs only */
#define	PCI_BAR_MEM_TYPE_32	0x0u
#define	PCI_BAR_MEM_TYPE_64	0x4u	/* the upper half is the next slot */

#define	PCI_BAR_MEM_PREFETCH	0x8u	/* bit 3, memory BARs only */

#define	PCI_BAR_MEM_ADDR_MASK	0xFFFFFFF0u
#define	PCI_BAR_IO_ADDR_MASK	0xFFFFFFFCu

/*
 * ── Class codes, as PCI_CLASS_REV carries them ───────────────────────
 *
 * The register is (class << 24) | (subclass << 16) | (progif << 8) | rev.
 */
#define	PCI_CLASS_OF(class_rev)		(((class_rev) >> 24) & 0xFFu)
#define	PCI_SUBCLASS_OF(class_rev)	(((class_rev) >> 16) & 0xFFu)
#define	PCI_PROGIF_OF(class_rev)	(((class_rev) >> 8) & 0xFFu)

#define	PCI_CLASS_STORAGE	0x01
#define	PCI_CLASS_DISPLAY	0x03

/*
 * 🔑 A device that says it IS memory.  Class 5 subclass 0 is a RAM controller,
 * and the distinction matters to anything deciding whether a region may be
 * WRITTEN to: the prefetchable bit is not that answer, because a device is free
 * to mark a register aperture prefetchable and QEMU's virtio does exactly that.
 * What the device declares itself to be is the answer.
 */
#define	PCI_CLASS_MEMORY	0x05
#define	PCI_SUBCLASS_RAM	0x00
#define	PCI_SUBCLASS_SATA	0x06
#define	PCI_PROGIF_AHCI		0x01

/*
 * ── The capability list ──────────────────────────────────────────────
 *
 * A singly linked list inside configuration space: PCI_CAP_POINTER holds the
 * offset of the first capability, each capability's first byte is its id and
 * its second is the offset of the next, and zero ends it.
 *
 * ⚠️ The list is only there if PCI_STATUS says so.  A device without the
 * capability-list bit has whatever the vendor left at 0x34, and following it
 * walks arbitrary bytes of that device's own configuration space -- which
 * reads as a capability of some id and is not one.
 *
 * ⚠️ And the offsets are DEVICE-relative and byte-granular, while the two
 * mechanisms that reach configuration space both read dwords.  A capability
 * at 0x52 is in the dword at 0x50, shifted.
 */
#define	PCI_STATUS_CAP_LIST	(1u << 4)	/* in PCI_STATUS */
#define	PCI_CAP_POINTER		0x34

#define	PCI_CAP_ID_MSI		0x05
#define	PCI_CAP_ID_MSIX		0x11

/*
 * ── MSI-X ────────────────────────────────────────────────────────────
 *
 * 🔑 An interrupt that is not a wire.  The device is told an address and a
 * value, and it raises its interrupt by WRITING that value to that address --
 * so there is no pin, no routing table in the firmware's AML to interpret,
 * and no sixteen-line ceiling.  Which is why this is the answer to a PCI
 * device on a Q35 arriving on a global system interrupt of 16 or above.
 *
 * ⚠️ And it is why an MSI-X table a driver could write is the same hole as a
 * DMA engine a driver could aim (#432/#511): the address is an ordinary
 * memory address as far as the device is concerned.
 *
 * The capability itself is small -- a control word and two BAR references.
 * The table it points at is in one of the device's own BARs, which is what
 * `BIR' selects.
 */
#define	PCI_MSIX_CONTROL	0x02	/* 16 bits, from the capability */
#define	PCI_MSIX_TABLE		0x04	/* BIR in bits 2:0, offset above */
#define	PCI_MSIX_PBA		0x08	/* same shape, the pending array */

#define	PCI_MSIX_CTL_TABLE_SIZE	0x07FFu	/* entries MINUS ONE, per the spec */
#define	PCI_MSIX_CTL_FUNC_MASK	0x4000u	/* mask every vector, whatever the
					 * per-entry bits say */
#define	PCI_MSIX_CTL_ENABLE	0x8000u

#define	PCI_MSIX_BIR_MASK	0x7u
#define	PCI_MSIX_OFFSET_MASK	0xFFFFFFF8u

/*
 * One table entry, and the layout is the standard's rather than ours.
 *
 * ⚠️ `vector_control' bit 0 is a MASK, so a freshly reset entry is masked and
 * an entry is armed by CLEARING a bit rather than setting one.
 */
#define	PCI_MSIX_ENTRY_SIZE	16
#define	PCI_MSIX_ENTRY_ADDR_LO	0x0
#define	PCI_MSIX_ENTRY_ADDR_HI	0x4
#define	PCI_MSIX_ENTRY_DATA	0x8
#define	PCI_MSIX_ENTRY_CTL	0xC
#define	PCI_MSIX_ENTRY_MASKED	0x1u

#endif	/* _DEVICE_PCI_H_ */
