/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * ahci.h — AHCI (Advanced Host Controller Interface) register definitions
 *
 * Reference: Serial ATA AHCI 1.3.1 Specification
 */

#ifndef _AHCI_H_
#define _AHCI_H_

#include <stdint.h>

/* PCI class/subclass for AHCI */
#define PCI_CLASS_STORAGE	0x01
#define PCI_SUBCLASS_SATA	0x06
#define PCI_PROGIF_AHCI		0x01

/* PCI configuration registers */
#define PCI_VENDOR_ID		0x00
#define PCI_DEVICE_ID		0x02
#define PCI_COMMAND		0x04
#define PCI_STATUS		0x06
#define PCI_CLASS_REV		0x08
#define PCI_BAR5		0x24	/* AHCI Base Address (ABAR) */
#define PCI_INTERRUPT_LINE	0x3C

/* PCI command bits */
#define PCI_CMD_MEM_ENABLE	(1 << 1)
#define PCI_CMD_BUS_MASTER	(1 << 2)

/* ================================================================
 * Generic Host Control registers (HBA memory, offset from ABAR)
 * ================================================================ */

#define AHCI_CAP		0x00	/* Host Capabilities */
#define AHCI_GHC		0x04	/* Global Host Control */
#define AHCI_IS			0x08	/* Interrupt Status */
#define AHCI_PI			0x0C	/* Ports Implemented */
#define AHCI_VS			0x10	/* Version */
#define AHCI_CAP2		0x24	/* Host Capabilities Extended */
#define AHCI_BOHC		0x28	/* BIOS/OS Handoff Control */

/* GHC bits */
#define GHC_HR			(1u << 0)	/* HBA Reset */
#define GHC_IE			(1u << 1)	/* Interrupt Enable */
#define GHC_AE			(1u << 31)	/* AHCI Enable */

/* CAP bits */
#define CAP_NP_MASK		0x1F		/* Number of Ports - 1 */
#define CAP_S64A		(1u << 31)	/* 64-bit Addressing */

/* CAP2 bits */
#define CAP2_BOH		(1u << 0)	/* BIOS/OS Handoff supported */

/* BOHC bits */
#define BOHC_BOS		(1u << 0)	/* BIOS-Owned Semaphore */
#define BOHC_OOS		(1u << 1)	/* OS-Owned Semaphore */

/* ================================================================
 * Port registers (offset from ABAR + 0x100 + port * 0x80)
 * ================================================================ */

#define AHCI_PORT_BASE		0x100
#define AHCI_PORT_SIZE		0x80

#define PORT_CLB		0x00	/* Command List Base Address */
#define PORT_CLBU		0x04	/* Command List Base Address Upper */
#define PORT_FB			0x08	/* FIS Base Address */
#define PORT_FBU		0x0C	/* FIS Base Address Upper */
#define PORT_IS			0x10	/* Interrupt Status */
#define PORT_IE			0x14	/* Interrupt Enable */
#define PORT_CMD		0x18	/* Command and Status */
#define PORT_TFD		0x20	/* Task File Data */
#define PORT_SIG		0x24	/* Signature */
#define PORT_SSTS		0x28	/* SATA Status (SCR0: SStatus) */
#define PORT_SCTL		0x2C	/* SATA Control (SCR2: SControl) */
#define PORT_SERR		0x30	/* SATA Error (SCR1: SError) */
#define PORT_SACT		0x34	/* SATA Active */
#define PORT_CI			0x38	/* Command Issue */

/* PORT_CMD bits */
#define PORT_CMD_ST		(1u << 0)	/* Start */
#define PORT_CMD_SUD		(1u << 1)	/* Spin-Up Device */
#define PORT_CMD_FRE		(1u << 4)	/* FIS Receive Enable */
#define PORT_CMD_FR		(1u << 14)	/* FIS Receive Running */
#define PORT_CMD_CR		(1u << 15)	/* Command List Running */

/* PORT_IS bits */
#define PORT_IS_DHRS		(1u << 0)	/* Device to Host FIS */
#define PORT_IS_PSS		(1u << 1)	/* PIO Setup FIS */
#define PORT_IS_DSS		(1u << 2)	/* DMA Setup FIS */
#define PORT_IS_SDBS		(1u << 3)	/* Set Device Bits FIS */
#define PORT_IS_TFES		(1u << 30)	/* Task File Error */

/* PORT_IE bits — mirror PORT_IS */
#define PORT_IE_DHRE		(1u << 0)
#define PORT_IE_PSE		(1u << 1)
#define PORT_IE_DSE		(1u << 2)
#define PORT_IE_SDBE		(1u << 3)
#define PORT_IE_TFEE		(1u << 30)

/* PORT_TFD bits */
#define PORT_TFD_STS_BSY	(1u << 7)
#define PORT_TFD_STS_DRQ	(1u << 3)
#define PORT_TFD_STS_ERR	(1u << 0)

/* PORT_SSTS bits */
#define SSTS_DET_MASK		0x0F
#define SSTS_DET_PRESENT	0x03	/* device present + phy established */

/* ================================================================
 * Command structures (in DMA memory)
 * ================================================================ */

/* Command Header (32 bytes each, 32 per port = 1 KB) */
struct ahci_cmd_hdr {
	uint16_t	opts;		/* CFL:5, A:1, W:1, P:1, R:1, B:1, C:1, rsvd:1, PMP:4 */
	uint16_t	prdtl;		/* Physical Region Descriptor Table Length */
	uint32_t	prdbc;		/* PRD Byte Count (updated by HBA) */
	uint32_t	ctba;		/* Command Table Base Address */
	uint32_t	ctbau;		/* Command Table Base Address Upper */
	uint32_t	rsvd[4];
} __attribute__((packed));

#define CMD_HDR_CFL(n)		((n) & 0x1F)	/* Command FIS Length in DWORDs */
#define CMD_HDR_W		(1u << 6)	/* Write */
#define CMD_HDR_P		(1u << 7)	/* Prefetchable */

/* Physical Region Descriptor Table entry (16 bytes) */
struct ahci_prdt {
	uint32_t	dba;		/* Data Base Address */
	uint32_t	dbau;		/* Data Base Address Upper */
	uint32_t	rsvd;
	uint32_t	dbc;		/* Byte Count (bit 31 = Interrupt on Completion) */
} __attribute__((packed));

#define PRDT_DBC(n)		(((n) - 1) & 0x3FFFFF)
#define PRDT_IOC		(1u << 31)

/* Command Table: FIS (64 bytes) + ATAPI (16) + reserved (48) + PRDTs */
struct ahci_cmd_table {
	uint8_t		cfis[64];	/* Command FIS */
	uint8_t		acmd[16];	/* ATAPI Command */
	uint8_t		rsvd[48];	/* Reserved */
	struct ahci_prdt prdt[1];	/* Variable-length PRDT array */
} __attribute__((packed));

/* Received FIS area (256 bytes per port) */
struct ahci_recv_fis {
	uint8_t		dsfis[28];	/* DMA Setup FIS */
	uint8_t		rsvd0[4];
	uint8_t		psfis[20];	/* PIO Setup FIS */
	uint8_t		rsvd1[12];
	uint8_t		rfis[20];	/* D2H Register FIS */
	uint8_t		rsvd2[4];
	uint8_t		sdbfis[8];	/* Set Device Bits FIS */
	uint8_t		ufis[64];	/* Unknown FIS */
	uint8_t		rsvd3[96];
} __attribute__((packed));

/* ================================================================
 * ATA Command FIS (Host to Device, FIS type 0x27)
 * ================================================================ */

#define FIS_TYPE_H2D		0x27

struct ata_fis_h2d {
	uint8_t		fis_type;	/* 0x27 */
	uint8_t		flags;		/* bit 7: C (command/control) */
	uint8_t		command;
	uint8_t		features;
	uint8_t		lba_lo;
	uint8_t		lba_mid;
	uint8_t		lba_hi;
	uint8_t		device;
	uint8_t		lba_lo_exp;
	uint8_t		lba_mid_exp;
	uint8_t		lba_hi_exp;
	uint8_t		features_exp;
	uint8_t		sector_count;
	uint8_t		sector_count_exp;
	uint8_t		rsvd;
	uint8_t		control;
	uint8_t		pad[4];
} __attribute__((packed));

/* ================================================================
 * ATA H2D FIS flags
 * ================================================================ */
#define FIS_H2D_FLAG_CMD	(1u << 7)	/* C bit: this is a command */

/* ATA device register bits */
#define ATA_DEV_LBA		(1u << 6)	/* LBA addressing mode */

/* ATA commands */
#define ATA_CMD_IDENTIFY	0xEC		/* IDENTIFY DEVICE */
#define ATA_CMD_READ_DMA_EXT	0x25		/* READ DMA EXT (LBA48) */
#define ATA_CMD_WRITE_DMA_EXT	0x35		/* WRITE DMA EXT (LBA48) */

/* AHCI ABAR size (covers 32 ports: 0x100 + 32*0x80 = 0x1100) */
#define AHCI_ABAR_SIZE		8192		/* 8 KB, 2 pages */

/* Maximum ports and command slots */
#define AHCI_MAX_PORTS		32
#define AHCI_MAX_SLOTS		32

#endif /* _AHCI_H_ */
