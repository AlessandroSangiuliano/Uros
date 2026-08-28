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
#define	PCI_BAR(n)		(PCI_BAR0 + (n) * 4u)
#define	PCI_INTERRUPT_LINE	0x3C

/* The BAR slots a type 0 header has.  SLOTS, not regions -- see below. */
#define	PCI_NUM_BAR_SLOTS	6

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
#define	PCI_SUBCLASS_SATA	0x06
#define	PCI_PROGIF_AHCI		0x01

#endif	/* _DEVICE_PCI_H_ */
