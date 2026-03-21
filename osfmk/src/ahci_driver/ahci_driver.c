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
 * ahci_driver.c — Userspace AHCI (SATA) driver for Uros
 *
 * Approach B: kernel provides PCI config, DMA alloc, MMIO mapping and
 * IRQ forwarding via device_master RPCs; all AHCI logic runs here.
 *
 * Boot flow:
 *   bootstrap → main() → pci_scan() → ahci_init()
 *   → ahci_hba_reset() → ahci_port_find() → ahci_port_init()
 *   → ahci_identify() → ahci_benchmark() → message loop
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/bootstrap.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <mach/mig_errors.h>
#include <sa_mach.h>
#include <device/device.h>
#include <device/device_types.h>
#include <servers/netname.h>
#include <stdio.h>
#include "device_master.h"
#include "device_server.h"
#include "ahci_batch_server.h"
#include "ahci.h"

/* msgh_id base for IRQ notifications from device_intr_register */
#define IRQ_NOTIFY_MSGH_BASE	3000

/* ================================================================
 * Batching constants and runtime configuration
 * ================================================================ */

/*
 * Scatter-gather: each slot uses PRDT_PER_SLOT pages (32 KB),
 * allowing multi-entry PRDTs with non-contiguous physical pages.
 */
#define SLOT_DATA_SIZE		(PRDT_PER_SLOT * 4096u)	/* 32 KB per slot */
#define CT_STRIDE		256u	/* command table spacing (128-byte aligned) */
#define SECTORS_PER_SLOT	(SLOT_DATA_SIZE / 512u)

/*
 * batch_slots — determined at runtime from HBA CAP.NCS and (if NCQ)
 * drive IDENTIFY queue depth.  Set by ahci_identify(), default 1
 * until detection runs.
 */
static unsigned int	batch_slots = 1;
static unsigned int	batch_data_size;	/* batch_slots * SLOT_DATA_SIZE */
static unsigned int	ra_sectors;		/* batch_slots * SECTORS_PER_SLOT */

/* ================================================================
 * Readahead cache
 *
 * When a sequential read pattern is detected (current LBA ==
 * previous end LBA), we read extra sectors and cache them.
 * The next ds_device_read for the continuation is served from
 * the cache without hitting the disk.
 * ================================================================ */

/* ra_sectors = batch_slots * SECTORS_PER_SLOT — set dynamically */

static struct {
	uint32_t	lba_start;	/* first cached LBA */
	uint32_t	lba_count;	/* number of cached sectors */
	vm_offset_t	buf;		/* vm_allocate'd buffer (0 = invalid) */
	unsigned int	buf_size;	/* allocated size */
} ra_cache;

/* ================================================================
 * Globals
 * ================================================================ */

static mach_port_t	host_port;
static mach_port_t	device_port;
static mach_port_t	master_device;
static mach_port_t	security_port;
static mach_port_t	root_ledger_wired;
static mach_port_t	root_ledger_paged;
static mach_port_t	irq_port;
static mach_port_t	ahci_service_port;	/* RPC port registered with netname */
static mach_port_t	port_set;	/* IRQ + RPC combined receive set */

/* PCI location of the AHCI controller */
static unsigned int	ahci_bus, ahci_slot, ahci_func;
static unsigned int	ahci_irq;
static unsigned int	ahci_abar_phys;

/*
 * ABAR mapped into our address space via device_mmio_map.
 * All AHCI register accesses go through ahci_read/ahci_write.
 */
static volatile uint32_t *abar;

/* Active SATA port */
static int		active_port = -1;

/* Total disk sectors (from IDENTIFY) */
static uint32_t		disk_sectors;

/* NCQ support: set by ahci_identify() if both HBA and drive support it */
static int		ncq_supported;
static unsigned int	ncq_depth;	/* max queue depth (1-32) */

/*
 * DMA buffers — each has:
 *   kva  : kernel VA (from device_dma_alloc, used as handle for dma_free)
 *   uva  : user VA   (from device_dma_map_user, CPU-accessible here)
 *   pa   : physical address (programmed into AHCI registers)
 */
static unsigned int	clb_kva,  clb_uva,  clb_pa;	/* Command List Base  1 KB */
static unsigned int	fb_kva,   fb_uva,   fb_pa;	/* FIS Base          256 B */
static unsigned int	ct_kva,   ct_uva,   ct_pa;	/* Command Table     4 KB */
static unsigned int	data_kva, data_uva, data_pa;	/* Data buffer (dynamic) */

/*
 * Scatter-gather PA list for the data buffer.
 * After ahci_realloc_batch_buffers(), data_pa_list[i] holds the
 * physical address of page i (4 KB).  data_pa is kept as alias
 * for data_pa_list[0] for single-command paths (IDENTIFY, benchmark).
 */
static unsigned int	data_pa_list[256];
static unsigned int	data_n_pages;

/* ================================================================
 * MMIO accessors
 * ================================================================ */

static inline uint32_t
ahci_read(unsigned int reg)
{
	return *(volatile uint32_t *)((uint8_t *)abar + reg);
}

static inline void
ahci_write(unsigned int reg, uint32_t val)
{
	*(volatile uint32_t *)((uint8_t *)abar + reg) = val;
}

static inline uint32_t
port_read(int port, unsigned int reg)
{
	return ahci_read(AHCI_PORT_BASE + port * AHCI_PORT_SIZE + reg);
}

static inline void
port_write(int port, unsigned int reg, uint32_t val)
{
	ahci_write(AHCI_PORT_BASE + port * AHCI_PORT_SIZE + reg, val);
}

/* ================================================================
 * Timing — RDTSC
 * ================================================================ */

static inline uint64_t
rdtsc(void)
{
	uint64_t tsc;
	__asm__ volatile("rdtsc" : "=A" (tsc));
	return tsc;
}

/* ================================================================
 * PCI scan — find AHCI controller
 * ================================================================ */

static int
pci_scan_ahci(void)
{
	unsigned int bus, slot, func;
	unsigned int vendor_device, class_rev;
	kern_return_t kr;

	printf("ahci: scanning PCI bus...\n");

	for (bus = 0; bus < 256; bus++) {
		for (slot = 0; slot < 32; slot++) {
			for (func = 0; func < 8; func++) {
				kr = device_pci_config_read(master_device,
					bus, slot, func, PCI_VENDOR_ID,
					&vendor_device);
				if (kr != KERN_SUCCESS)
					continue;
				if (vendor_device == 0xFFFFFFFF ||
				    vendor_device == 0)
					continue;

				kr = device_pci_config_read(master_device,
					bus, slot, func, PCI_CLASS_REV,
					&class_rev);
				if (kr != KERN_SUCCESS)
					continue;

				if (((class_rev >> 24) & 0xFF) == PCI_CLASS_STORAGE &&
				    ((class_rev >> 16) & 0xFF) == PCI_SUBCLASS_SATA &&
				    ((class_rev >>  8) & 0xFF) == PCI_PROGIF_AHCI) {
					ahci_bus  = bus;
					ahci_slot = slot;
					ahci_func = func;
					printf("ahci: found controller at PCI %u:%u.%u "
					       "vendor:device=0x%08X\n",
					       bus, slot, func, vendor_device);
					return 1;
				}
			}
		}
	}

	printf("ahci: no AHCI controller found\n");
	return 0;
}

/* ================================================================
 * Port start / stop helpers
 * ================================================================ */

static void
port_stop(int port)
{
	int i;
	uint32_t cmd;

	/* Clear ST — stop command list processing */
	cmd = port_read(port, PORT_CMD);
	port_write(port, PORT_CMD, cmd & ~PORT_CMD_ST);
	for (i = 0; i < 500; i++)
		if (!(port_read(port, PORT_CMD) & PORT_CMD_CR))
			break;

	/* Clear FRE — stop FIS receive */
	cmd = port_read(port, PORT_CMD);
	port_write(port, PORT_CMD, cmd & ~PORT_CMD_FRE);
	for (i = 0; i < 500; i++)
		if (!(port_read(port, PORT_CMD) & PORT_CMD_FR))
			break;
}

static void
port_start(int port)
{
	uint32_t cmd;
	int i;

	/*
	 * Enable FIS Receive first so the device can send its signature FIS.
	 * Do NOT wait for BSY/DRQ here: right after HBA reset PORT_TFD may
	 * show BSY until the device responds, causing an infinite spin.
	 */
	cmd = port_read(port, PORT_CMD);
	port_write(port, PORT_CMD, cmd | PORT_CMD_FRE | PORT_CMD_SUD);

	/* Wait up to 500 ms for BSY/DRQ to clear before starting DMA engine */
	for (i = 0; i < 500000; i++)
		if (!(port_read(port, PORT_TFD) &
		      (PORT_TFD_STS_BSY | PORT_TFD_STS_DRQ)))
			break;

	/* Start command list processing */
	cmd = port_read(port, PORT_CMD);
	port_write(port, PORT_CMD, cmd | PORT_CMD_ST);
}

/* ================================================================
 * HBA reset and global init
 * ================================================================ */

static int
ahci_hba_reset(void)
{
	int i;

	/* BIOS/OS handoff if supported */
	if (ahci_read(AHCI_CAP2) & CAP2_BOH) {
		ahci_write(AHCI_BOHC,
			   ahci_read(AHCI_BOHC) | BOHC_OOS);
		for (i = 0; i < 25000; i++)
			if (!(ahci_read(AHCI_BOHC) & BOHC_BOS))
				break;
	}

	/* Enable AHCI mode before reset */
	ahci_write(AHCI_GHC, GHC_AE);

	/* HBA reset */
	ahci_write(AHCI_GHC, ahci_read(AHCI_GHC) | GHC_HR);
	for (i = 0; i < 1000000; i++)
		if (!(ahci_read(AHCI_GHC) & GHC_HR))
			break;
	if (ahci_read(AHCI_GHC) & GHC_HR) {
		printf("ahci: HBA reset timed out\n");
		return -1;
	}

	/* Re-enable AHCI after reset */
	ahci_write(AHCI_GHC, GHC_AE);

	printf("ahci: HBA reset OK  version=0x%08X  cap=0x%08X\n",
	       ahci_read(AHCI_VS), ahci_read(AHCI_CAP));
	return 0;
}

/* ================================================================
 * Port discovery
 * ================================================================ */

static int
ahci_port_find(void)
{
	uint32_t pi = ahci_read(AHCI_PI);
	int port;

	for (port = 0; port < AHCI_MAX_PORTS; port++) {
		if (!(pi & (1u << port)))
			continue;
		if ((port_read(port, PORT_SSTS) & SSTS_DET_MASK)
		    == SSTS_DET_PRESENT) {
			printf("ahci: device on port %d  sig=0x%08X\n",
			       port, port_read(port, PORT_SIG));
			return port;
		}
	}

	printf("ahci: no device found (PI=0x%08X)\n", pi);
	return -1;
}

/* ================================================================
 * Port initialisation
 * ================================================================ */

static int
ahci_port_init(int port)
{
	port_stop(port);

	/* Set command list and FIS receive areas */
	port_write(port, PORT_CLB,  clb_pa);
	port_write(port, PORT_CLBU, 0);
	port_write(port, PORT_FB,   fb_pa);
	port_write(port, PORT_FBU,  0);

	/* Clear errors and interrupt status */
	port_write(port, PORT_SERR, ~0u);
	port_write(port, PORT_IS,   ~0u);

	/* Enable interrupts: D2H FIS, SDB FIS (for NCQ), and error */
	port_write(port, PORT_IE,
		   PORT_IE_DHRE | PORT_IE_SDBE | PORT_IE_TFEE);

	port_start(port);

	printf("ahci: port %d initialised  cmd=0x%08X  tfd=0x%08X\n",
	       port,
	       port_read(port, PORT_CMD),
	       port_read(port, PORT_TFD));
	return 0;
}

/* ================================================================
 * Command submission (polling, slot 0 only)
 * ================================================================ */

static int
ahci_submit_cmd(int port, struct ata_fis_h2d *fis,
		unsigned int buf_pa, unsigned int buf_size,
		int write)
{
	struct ahci_cmd_hdr   *hdr = (struct ahci_cmd_hdr *)clb_uva;
	struct ahci_cmd_table *tbl = (struct ahci_cmd_table *)ct_uva;
	int i;

	/* Wait for port idle */
	for (i = 0; i < 1000000; i++)
		if (!(port_read(port, PORT_TFD) &
		      (PORT_TFD_STS_BSY | PORT_TFD_STS_DRQ)))
			break;

	/* Clear interrupt status */
	port_write(port, PORT_IS, ~0u);

	/* Fill command header for slot 0 */
	hdr[0].opts  = CMD_HDR_CFL(5);
	if (write)
		hdr[0].opts |= CMD_HDR_W;
	hdr[0].prdtl = (buf_size > 0) ? 1 : 0;
	hdr[0].prdbc = 0;
	hdr[0].ctba  = ct_pa;
	hdr[0].ctbau = 0;
	for (i = 0; i < 4; i++)
		hdr[0].rsvd[i] = 0;

	/* Fill command table */
	memset(tbl, 0, sizeof(struct ahci_cmd_table));
	memcpy(tbl->cfis, fis, 20);

	if (buf_size > 0) {
		tbl->prdt[0].dba  = buf_pa;
		tbl->prdt[0].dbau = 0;
		tbl->prdt[0].rsvd = 0;
		tbl->prdt[0].dbc  = PRDT_DBC(buf_size) | PRDT_IOC;
	}

	/* Issue command on slot 0 */
	port_write(port, PORT_CI, 1u);

	/* Poll for completion (~5 s timeout) */
	for (i = 0; i < 5000000; i++) {
		uint32_t is = port_read(port, PORT_IS);
		if (is & PORT_IS_TFES) {
			printf("ahci: task file error  IS=0x%08X  TFD=0x%08X\n",
			       is, port_read(port, PORT_TFD));
			port_write(port, PORT_IS, PORT_IS_TFES);
			return -1;
		}
		if (!(port_read(port, PORT_CI) & 1u))
			return 0;
	}

	printf("ahci: command timed out  CI=0x%08X\n",
	       port_read(port, PORT_CI));
	return -1;
}

/* ================================================================
 * IDENTIFY DEVICE
 * ================================================================ */

static int
ahci_identify(int port)
{
	struct ata_fis_h2d fis;
	uint16_t *buf = (uint16_t *)data_uva;
	char model[41];
	uint32_t lba28;
	unsigned int i;

	memset(&fis, 0, sizeof(fis));
	fis.fis_type = FIS_TYPE_H2D;
	fis.flags    = FIS_H2D_FLAG_CMD;
	fis.command  = ATA_CMD_IDENTIFY;
	fis.device   = 0;

	if (ahci_submit_cmd(port, &fis, data_pa, 512, 0) < 0) {
		printf("ahci: IDENTIFY failed\n");
		return -1;
	}

	/* Model string: words 27-46, big-endian bytes within each word */
	for (i = 0; i < 20; i++) {
		uint16_t w = buf[27 + i];
		model[i * 2]     = (char)((w >> 8) & 0xFF);
		model[i * 2 + 1] = (char)(w & 0xFF);
	}
	model[40] = '\0';
	for (i = 39; i > 0 && model[i] == ' '; i--)
		model[i] = '\0';

	/* LBA28 total addressable sectors (words 60-61) */
	lba28 = ((uint32_t)buf[61] << 16) | buf[60];
	disk_sectors = lba28;

	printf("ahci: model    : %s\n", model);
	printf("ahci: sectors  : %u  capacity: ~%u MB\n",
	       lba28, lba28 / 2048);

	/*
	 * Detect HBA command slot count and NCQ support.
	 *
	 * CAP.NCS (bits 12:8): number of command slots - 1 (0-31).
	 * This determines how many slots the HBA supports regardless
	 * of NCQ.  For NCQ, the effective depth is further limited by
	 * the drive's advertised queue depth (IDENTIFY word 75).
	 *
	 * batch_slots is set to the usable slot count; the caller
	 * must reallocate DMA buffers accordingly.
	 */
	{
		uint32_t cap = ahci_read(AHCI_CAP);
		int hba_ncq = (cap & CAP_SNCQ) != 0;
		unsigned int hba_slots =
			((cap >> CAP_NCS_SHIFT) & CAP_NCS_MASK) + 1;
		int dev_ncq = (buf[ATA_ID_SATA_CAP] & ATA_ID_SATA_CAP_NCQ) != 0;
		unsigned int dev_qdepth = (buf[ATA_ID_QUEUE_DEPTH] & 0x1F) + 1;

		if (hba_ncq && dev_ncq) {
			ncq_supported = 1;
			ncq_depth = hba_slots < dev_qdepth
				    ? hba_slots : dev_qdepth;
			batch_slots = ncq_depth;
			printf("ahci: NCQ supported  depth=%u "
			       "(hba=%u, dev=%u)\n",
			       ncq_depth, hba_slots, dev_qdepth);
		} else {
			ncq_supported = 0;
			batch_slots = hba_slots;
			printf("ahci: NCQ not available "
			       "(hba=%s, dev=%s)  slots=%u\n",
			       hba_ncq ? "yes" : "no",
			       dev_ncq ? "yes" : "no",
			       hba_slots);
		}

		batch_data_size = batch_slots * SLOT_DATA_SIZE;
		ra_sectors = batch_slots * SECTORS_PER_SLOT;
		printf("ahci: batch_slots=%u  data=%u KB  "
		       "readahead=%u sectors\n",
		       batch_slots, batch_data_size / 1024, ra_sectors);
	}

	return 0;
}

/* ================================================================
 * READ DMA EXT (LBA48)
 * ================================================================ */

static int
ahci_read_sectors(int port, uint32_t lba, uint16_t count)
{
	struct ata_fis_h2d fis;

	memset(&fis, 0, sizeof(fis));
	fis.fis_type         = FIS_TYPE_H2D;
	fis.flags            = FIS_H2D_FLAG_CMD;
	fis.command          = ATA_CMD_READ_DMA_EXT;
	fis.device           = ATA_DEV_LBA;
	fis.lba_lo           = (lba >>  0) & 0xFF;
	fis.lba_mid          = (lba >>  8) & 0xFF;
	fis.lba_hi           = (lba >> 16) & 0xFF;
	fis.lba_lo_exp       = (lba >> 24) & 0xFF;
	fis.lba_mid_exp      = 0;
	fis.lba_hi_exp       = 0;
	fis.sector_count     = count & 0xFF;
	fis.sector_count_exp = (count >> 8) & 0xFF;

	return ahci_submit_cmd(port, &fis, data_pa, (unsigned int)count * 512, 0);
}

/* ================================================================
 * WRITE DMA EXT (LBA48)
 * ================================================================ */

static int
ahci_write_sectors(int port, uint32_t lba, uint16_t count)
{
	struct ata_fis_h2d fis;

	memset(&fis, 0, sizeof(fis));
	fis.fis_type         = FIS_TYPE_H2D;
	fis.flags            = FIS_H2D_FLAG_CMD;
	fis.command          = ATA_CMD_WRITE_DMA_EXT;
	fis.device           = ATA_DEV_LBA;
	fis.lba_lo           = (lba >>  0) & 0xFF;
	fis.lba_mid          = (lba >>  8) & 0xFF;
	fis.lba_hi           = (lba >> 16) & 0xFF;
	fis.lba_lo_exp       = (lba >> 24) & 0xFF;
	fis.lba_mid_exp      = 0;
	fis.lba_hi_exp       = 0;
	fis.sector_count     = count & 0xFF;
	fis.sector_count_exp = (count >> 8) & 0xFF;

	/* write=1: device reads from host memory (DMA write to disk) */
	return ahci_submit_cmd(port, &fis, data_pa, (unsigned int)count * 512, 1);
}

/* ================================================================
 * Batched command submission (scatter-gather)
 *
 * Fills up to batch_slots command headers/tables and issues them
 * all with a single PORT_CI write.  Each slot transfers up to
 * SLOT_DATA_SIZE (32 KB) using PRDT_PER_SLOT PRDT entries.
 *
 * Data layout (scatter-gather):
 *   slot i, page p → data_pa_list[i * PRDT_PER_SLOT + p]  (PA)
 *                     data_uva + (i * PRDT_PER_SLOT + p) * 4096 (VA)
 *
 * Command tables:
 *   slot i → ct_pa + i * CT_STRIDE
 * ================================================================ */

static int
ahci_submit_batch(int port, uint32_t start_lba, unsigned int nsectors,
		  int write)
{
	struct ahci_cmd_hdr   *hdr = (struct ahci_cmd_hdr *)clb_uva;
	unsigned int nslots, slot, sects_per_slot;
	unsigned int ci_mask;
	int i;

	/* How many slots do we need? */
	nslots = (nsectors + SECTORS_PER_SLOT - 1) / SECTORS_PER_SLOT;
	if (nslots > batch_slots)
		nslots = batch_slots;

	/* Wait for port idle */
	for (i = 0; i < 1000000; i++)
		if (!(port_read(port, PORT_TFD) &
		      (PORT_TFD_STS_BSY | PORT_TFD_STS_DRQ)))
			break;

	/* Clear interrupt status */
	port_write(port, PORT_IS, ~0u);

	/* Fill command headers and tables for each slot */
	for (slot = 0; slot < nslots; slot++) {
		struct ahci_cmd_table *tbl;
		struct ata_fis_h2d *fis;
		uint32_t lba;
		unsigned int slot_bytes, n_prdt, p;

		sects_per_slot = nsectors > SECTORS_PER_SLOT
			? SECTORS_PER_SLOT : nsectors;
		slot_bytes = sects_per_slot * 512u;
		lba = start_lba + slot * SECTORS_PER_SLOT;

		/*
		 * Build scatter-gather PRDT: one entry per 4 KB page.
		 * Physical addresses come from data_pa_list[].
		 */
		n_prdt = (slot_bytes + 4095u) / 4096u;
		if (n_prdt > PRDT_PER_SLOT)
			n_prdt = PRDT_PER_SLOT;

		/* Command header */
		hdr[slot].opts  = CMD_HDR_CFL(5);
		if (write)
			hdr[slot].opts |= CMD_HDR_W;
		hdr[slot].prdtl = n_prdt;
		hdr[slot].prdbc = 0;
		hdr[slot].ctba  = ct_pa + slot * CT_STRIDE;
		hdr[slot].ctbau = 0;
		hdr[slot].rsvd[0] = 0;
		hdr[slot].rsvd[1] = 0;
		hdr[slot].rsvd[2] = 0;
		hdr[slot].rsvd[3] = 0;

		/* Command table */
		tbl = (struct ahci_cmd_table *)(ct_uva + slot * CT_STRIDE);
		memset(tbl, 0, CT_STRIDE);

		fis = (struct ata_fis_h2d *)tbl->cfis;
		fis->fis_type         = FIS_TYPE_H2D;
		fis->flags            = FIS_H2D_FLAG_CMD;
		fis->device           = ATA_DEV_LBA;
		fis->lba_lo           = (lba >>  0) & 0xFF;
		fis->lba_mid          = (lba >>  8) & 0xFF;
		fis->lba_hi           = (lba >> 16) & 0xFF;
		fis->lba_lo_exp       = (lba >> 24) & 0xFF;
		fis->lba_mid_exp      = 0;
		fis->lba_hi_exp       = 0;

		if (ncq_supported) {
			/*
			 * NCQ FPDMA: sector count in features field,
			 * tag in sector_count bits 7:3.
			 */
			fis->command          = write
				? ATA_CMD_WRITE_FPDMA_QUEUED
				: ATA_CMD_READ_FPDMA_QUEUED;
			fis->features         = sects_per_slot & 0xFF;
			fis->features_exp     = (sects_per_slot >> 8) & 0xFF;
			fis->sector_count     = (slot << 3);
			fis->sector_count_exp = 0;
		} else {
			fis->command          = write
				? ATA_CMD_WRITE_DMA_EXT
				: ATA_CMD_READ_DMA_EXT;
			fis->sector_count     = sects_per_slot & 0xFF;
			fis->sector_count_exp = (sects_per_slot >> 8) & 0xFF;
		}

		/* Scatter-gather PRDT: one entry per page */
		for (p = 0; p < n_prdt; p++) {
			unsigned int page_idx =
				slot * PRDT_PER_SLOT + p;
			unsigned int page_bytes = 4096u;

			/* Last entry may be partial page */
			if (p == n_prdt - 1) {
				unsigned int rem =
					slot_bytes - p * 4096u;
				if (rem < 4096u)
					page_bytes = rem;
			}

			tbl->prdt[p].dba  = data_pa_list[page_idx];
			tbl->prdt[p].dbau = 0;
			tbl->prdt[p].rsvd = 0;
			tbl->prdt[p].dbc  = PRDT_DBC(page_bytes);
			if (p == n_prdt - 1)
				tbl->prdt[p].dbc |= PRDT_IOC;
		}

		nsectors -= sects_per_slot;
	}

	/* Issue all slots at once */
	ci_mask = (1u << nslots) - 1;
	if (ncq_supported) {
		/* NCQ: set SACT bits before CI */
		port_write(port, PORT_SACT, ci_mask);
	}
	port_write(port, PORT_CI, ci_mask);

	/* Poll for completion (~5 s timeout) */
	for (i = 0; i < 5000000; i++) {
		uint32_t is = port_read(port, PORT_IS);
		if (is & PORT_IS_TFES) {
			printf("ahci: batch task file error  IS=0x%08X  "
			       "TFD=0x%08X\n",
			       is, port_read(port, PORT_TFD));
			port_write(port, PORT_IS, PORT_IS_TFES);
			return -1;
		}
		if (ncq_supported) {
			/* NCQ: SACT bits cleared by SDB FIS on completion */
			if (!(port_read(port, PORT_SACT) & ci_mask))
				return 0;
		} else {
			if (!(port_read(port, PORT_CI) & ci_mask))
				return 0;
		}
	}

	printf("ahci: batch timed out  CI=0x%08X  SACT=0x%08X\n",
	       port_read(port, PORT_CI),
	       port_read(port, PORT_SACT));
	return -1;
}

/* ================================================================
 * Sequential read benchmark
 * ================================================================ */

#define BENCH_SECTORS		256u	/* 128 KB total */
#define BENCH_SECTORS_PER_CMD	8u	/* 4 KB per command (= data buf size) */

static void
ahci_benchmark(int port)
{
	uint64_t t0, t1, elapsed;
	uint32_t lba, i;
	unsigned int cmds = BENCH_SECTORS / BENCH_SECTORS_PER_CMD;
	unsigned int bytes = BENCH_SECTORS * 512u;

	printf("ahci: benchmark: %u sectors (%u KB) x %u-sector cmds\n",
	       BENCH_SECTORS, BENCH_SECTORS / 2, BENCH_SECTORS_PER_CMD);

	/* Warm-up: one command before measuring */
	if (ahci_read_sectors(port, 0, BENCH_SECTORS_PER_CMD) < 0) {
		printf("ahci: benchmark warm-up failed\n");
		return;
	}

	t0 = rdtsc();
	for (i = 0, lba = 0; i < cmds; i++, lba += BENCH_SECTORS_PER_CMD) {
		if (ahci_read_sectors(port, lba, BENCH_SECTORS_PER_CMD) < 0) {
			printf("ahci: benchmark read error at LBA %u\n", lba);
			return;
		}
	}
	t1 = rdtsc();

	elapsed = t1 - t0;

	/*
	 * Use only the lower 32 bits for division — in QEMU, reading
	 * 256 sectors takes well under 2^32 TSC cycles (~1 GHz TSC).
	 */
	{
		unsigned int cyc = (unsigned int)(elapsed & 0xFFFFFFFFu);

		printf("ahci: %u sectors (%u KB) in %u TSC cycles\n",
		       BENCH_SECTORS, BENCH_SECTORS / 2, cyc);
		printf("ahci: cycles/sector : %u\n", cyc / BENCH_SECTORS);
		/* MB/s = bytes * 1000 / cycles  (1 GHz TSC assumed) */
		if (cyc > 0)
			printf("ahci: ~%u MB/s  (assumes 1 GHz TSC)\n",
			       bytes * 1000u / cyc);
	}
}

/* ================================================================
 * Reallocate CT and data DMA buffers for detected batch_slots
 *
 * Called after ahci_identify() sets batch_slots.  Frees the initial
 * 1-page CT and data buffers and allocates properly-sized ones:
 *   CT:   batch_slots * CT_STRIDE bytes (contiguous, for HBA)
 *   Data: batch_slots * PRDT_PER_SLOT pages (scatter-gather)
 * ================================================================ */

static int
ahci_realloc_batch_buffers(void)
{
	kern_return_t kr;
	unsigned int ct_size;
	unsigned int n_pages;
	mach_msg_type_number_t pa_count;

	if (batch_slots <= 1)
		return 0;	/* already sized for 1 slot */

	ct_size = batch_slots * CT_STRIDE;
	/* Round up to page boundary */
	ct_size = (ct_size + 4095u) & ~4095u;

	/* Free old CT buffer */
	device_dma_free(master_device, ct_kva, 4096);

	kr = device_dma_alloc(master_device, ct_size, &ct_kva, &ct_pa);
	if (kr != KERN_SUCCESS) {
		printf("ahci: CT realloc (%u bytes) failed\n", ct_size);
		return -1;
	}
	kr = device_dma_map_user(master_device, ct_kva, ct_size,
				 mach_task_self(), &ct_uva);
	if (kr != KERN_SUCCESS) {
		printf("ahci: CT remap failed\n");
		return -1;
	}

	/* Free old data buffer (contiguous, from initial alloc) */
	device_dma_free(master_device, data_kva, 4096);

	/*
	 * Scatter-gather data buffer: allocate individually wired
	 * pages (no physical contiguity needed).  Each slot gets
	 * PRDT_PER_SLOT pages; the PRDT entries point to each page.
	 */
	n_pages = batch_slots * PRDT_PER_SLOT;
	if (n_pages > 256)
		n_pages = 256;

	kr = device_dma_alloc_sg(master_device, n_pages,
				 mach_task_self(),
				 &data_kva, &data_uva,
				 data_pa_list, &pa_count);
	if (kr != KERN_SUCCESS) {
		printf("ahci: scatter-gather alloc (%u pages) "
		       "failed (kr=%d)\n", n_pages, kr);
		return -1;
	}
	data_n_pages = n_pages;
	data_pa = data_pa_list[0];	/* alias for single-cmd paths */

	printf("ahci: DMA reallocated: ct=%u bytes  data=%u pages "
	       "(%u KB, scatter-gather, %u slots)\n",
	       ct_size, n_pages, n_pages * 4, batch_slots);

	return 0;
}

/* ================================================================
 * Full AHCI initialisation
 * ================================================================ */

static int
ahci_init(void)
{
	kern_return_t kr;
	unsigned int bar5, cmd_reg, irq_reg;

	/* Read and save BAR5 (ABAR physical address) */
	kr = device_pci_config_read(master_device,
		ahci_bus, ahci_slot, ahci_func, PCI_BAR5, &bar5);
	if (kr != KERN_SUCCESS) {
		printf("ahci: failed to read BAR5\n");
		return -1;
	}
	ahci_abar_phys = bar5 & ~0xFu;
	printf("ahci: ABAR phys = 0x%08X\n", ahci_abar_phys);

	/* Enable PCI bus master + memory space */
	kr = device_pci_config_read(master_device,
		ahci_bus, ahci_slot, ahci_func, PCI_COMMAND, &cmd_reg);
	if (kr == KERN_SUCCESS) {
		cmd_reg |= PCI_CMD_MEM_ENABLE | PCI_CMD_BUS_MASTER;
		device_pci_config_write(master_device,
			ahci_bus, ahci_slot, ahci_func, PCI_COMMAND, cmd_reg);
	}

	/* Read IRQ */
	kr = device_pci_config_read(master_device,
		ahci_bus, ahci_slot, ahci_func, PCI_INTERRUPT_LINE, &irq_reg);
	if (kr == KERN_SUCCESS) {
		ahci_irq = irq_reg & 0xFF;
		printf("ahci: IRQ = %u\n", ahci_irq);
	}

	/* Map ABAR into our address space */
	kr = device_mmio_map(master_device,
		ahci_abar_phys, AHCI_ABAR_SIZE,
		mach_task_self(), (unsigned int *)&abar);
	if (kr != KERN_SUCCESS) {
		printf("ahci: device_mmio_map failed (kr=%d)\n", kr);
		return -1;
	}
	printf("ahci: ABAR mapped at uva=0x%08X\n", (unsigned int)abar);

	/* Allocate DMA buffers and map them into our address space */
	kr = device_dma_alloc(master_device, 4096, &clb_kva, &clb_pa);
	if (kr != KERN_SUCCESS) { printf("ahci: CLB alloc failed\n"); return -1; }
	kr = device_dma_map_user(master_device, clb_kva, 4096,
				 mach_task_self(), &clb_uva);
	if (kr != KERN_SUCCESS) { printf("ahci: CLB map failed\n"); return -1; }

	kr = device_dma_alloc(master_device, 4096, &fb_kva, &fb_pa);
	if (kr != KERN_SUCCESS) { printf("ahci: FB alloc failed\n"); return -1; }
	kr = device_dma_map_user(master_device, fb_kva, 4096,
				 mach_task_self(), &fb_uva);
	if (kr != KERN_SUCCESS) { printf("ahci: FB map failed\n"); return -1; }

	kr = device_dma_alloc(master_device, 4096, &ct_kva, &ct_pa);
	if (kr != KERN_SUCCESS) { printf("ahci: CT alloc failed\n"); return -1; }
	kr = device_dma_map_user(master_device, ct_kva, 4096,
				 mach_task_self(), &ct_uva);
	if (kr != KERN_SUCCESS) { printf("ahci: CT map failed\n"); return -1; }

	/*
	 * Initial data buffer: 1 page, enough for IDENTIFY DEVICE.
	 * After ahci_identify() detects the slot count we reallocate.
	 */
	kr = device_dma_alloc(master_device, 4096, &data_kva, &data_pa);
	if (kr != KERN_SUCCESS) { printf("ahci: data alloc failed\n"); return -1; }
	kr = device_dma_map_user(master_device, data_kva, 4096,
				 mach_task_self(), &data_uva);
	if (kr != KERN_SUCCESS) { printf("ahci: data map failed\n"); return -1; }

	/* Initial scatter-gather state: 1 page, contiguous is fine */
	data_pa_list[0] = data_pa;
	data_n_pages = 1;

	printf("ahci: DMA: clb pa=0x%08X fb pa=0x%08X ct pa=0x%08X data pa=0x%08X\n",
	       clb_pa, fb_pa, ct_pa, data_pa);

	/* HBA reset + AHCI enable */
	if (ahci_hba_reset() < 0)
		return -1;

	/* Find first port with a device */
	active_port = ahci_port_find();
	if (active_port < 0)
		return -1;

	/* Initialise that port */
	if (ahci_port_init(active_port) < 0)
		return -1;

	/* Register IRQ notification */
	if (ahci_irq > 0 && ahci_irq < 16) {
		kr = device_intr_register(master_device, ahci_irq, irq_port,
					  MACH_MSG_TYPE_MAKE_SEND);
		if (kr == KERN_SUCCESS)
			printf("ahci: IRQ %u registered\n", ahci_irq);
	}

	return 0;
}

/* ================================================================
 * Device server — MIG server stubs for device.defs (subsystem 2800)
 *
 * The ahci_driver acts as a userspace device server: clients (e.g.
 * ext2_server) obtain our service port via netname_look_up and send
 * device_read / device_get_status RPCs.  The MIG-generated
 * device_server() demux calls these ds_* implementations.
 * ================================================================ */

#define SECTOR_SIZE	512u

kern_return_t
ds_device_open(mach_port_t master, mach_port_t reply,
	       mach_msg_type_name_t reply_poly,
	       mach_port_t ledger, dev_mode_t mode,
	       security_token_t sec_token, dev_name_t name,
	       mach_port_t *device)
{
	if (active_port < 0)
		return D_NO_SUCH_DEVICE;
	*device = ahci_service_port;
	return KERN_SUCCESS;
}

kern_return_t
ds_device_close(mach_port_t device)
{
	return KERN_SUCCESS;
}

kern_return_t
ds_device_read(mach_port_t device, mach_port_t reply,
	       mach_msg_type_name_t reply_poly,
	       dev_mode_t mode, recnum_t recnum,
	       io_buf_len_t bytes_wanted,
	       io_buf_ptr_t *data, mach_msg_type_number_t *data_count)
{
	kern_return_t kr;
	vm_offset_t buf;
	unsigned int total, nsectors, lba;

	if (active_port < 0)
		return D_NO_SUCH_DEVICE;
	if (bytes_wanted <= 0)
		return D_INVALID_SIZE;

	/* Round up to sector boundary */
	total = (unsigned int)bytes_wanted;
	if (total % SECTOR_SIZE)
		total = (total + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
	nsectors = total / SECTOR_SIZE;
	lba = recnum;

	kr = vm_allocate(mach_task_self(), &buf, total, TRUE);
	if (kr != KERN_SUCCESS)
		return D_NO_MEMORY;

	/*
	 * Check readahead cache: if the requested range [lba, lba+nsectors)
	 * is fully contained in the cache, copy from there.
	 */
	if (ra_cache.buf != 0 &&
	    lba >= ra_cache.lba_start &&
	    lba + nsectors <= ra_cache.lba_start + ra_cache.lba_count) {
		unsigned int cache_off =
			(lba - ra_cache.lba_start) * SECTOR_SIZE;
		memcpy((void *)buf,
		       (void *)(ra_cache.buf + cache_off), total);
		*data = (io_buf_ptr_t)buf;
		*data_count = (mach_msg_type_number_t)bytes_wanted;
		return KERN_SUCCESS;
	}

	/*
	 * Cache miss — read from disk.
	 * If the request is sequential (lba == end of last read) and
	 * small enough, read extra sectors for readahead.
	 */
	{
		unsigned int read_sects = nsectors;
		unsigned int ra_extra = 0;
		unsigned int offset, chunk;

		/* Sequential detection: prefetch up to ra_sectors total */
		if (ra_cache.buf != 0 &&
		    lba == ra_cache.lba_start + ra_cache.lba_count &&
		    nsectors <= ra_sectors / 2 &&
		    lba + ra_sectors <= disk_sectors) {
			read_sects = ra_sectors;
			ra_extra = read_sects - nsectors;
		}

		/* Read in batches of up to batch_data_size */
		unsigned int read_bytes = read_sects * SECTOR_SIZE;
		unsigned int ra_buf_needed = ra_extra * SECTOR_SIZE;
		vm_offset_t ra_buf = 0;

		/* Allocate readahead buffer if needed */
		if (ra_extra > 0) {
			kr = vm_allocate(mach_task_self(), &ra_buf,
					 ra_buf_needed, TRUE);
			if (kr != KERN_SUCCESS) {
				/* Fall back to no readahead */
				read_sects = nsectors;
				read_bytes = total;
				ra_extra = 0;
			}
		}

		for (offset = 0; offset < read_bytes; offset += chunk) {
			unsigned int batch_sects;

			chunk = read_bytes - offset;
			if (chunk > batch_data_size)
				chunk = batch_data_size;
			batch_sects = chunk / SECTOR_SIZE;

			if (ahci_submit_batch(active_port,
					      lba + offset / SECTOR_SIZE,
					      batch_sects, 0) < 0) {
				vm_deallocate(mach_task_self(), buf, total);
				if (ra_buf)
					vm_deallocate(mach_task_self(),
						      ra_buf, ra_buf_needed);
				return D_IO_ERROR;
			}

			/*
			 * Copy data: the first 'total' bytes go to the
			 * client buffer, any extra goes to readahead.
			 */
			if (offset < total) {
				unsigned int client_chunk = total - offset;
				if (client_chunk > chunk)
					client_chunk = chunk;
				memcpy((void *)(buf + offset),
				       (void *)data_uva, client_chunk);

				if (client_chunk < chunk && ra_buf) {
					memcpy((void *)ra_buf,
					       (void *)(data_uva + client_chunk),
					       chunk - client_chunk);
				}
			} else if (ra_buf) {
				unsigned int ra_off = offset - total;
				memcpy((void *)(ra_buf + ra_off),
				       (void *)data_uva, chunk);
			}
		}

		/* Update readahead cache */
		if (ra_cache.buf != 0)
			vm_deallocate(mach_task_self(),
				      ra_cache.buf, ra_cache.buf_size);

		if (ra_extra > 0 && ra_buf != 0) {
			ra_cache.lba_start = lba + nsectors;
			ra_cache.lba_count = ra_extra;
			ra_cache.buf       = ra_buf;
			ra_cache.buf_size  = ra_buf_needed;
		} else {
			/* No readahead — just record position for
			 * sequential detection on next call */
			ra_cache.lba_start = lba;
			ra_cache.lba_count = nsectors;
			ra_cache.buf       = 0;
			ra_cache.buf_size  = 0;
		}
	}

	*data = (io_buf_ptr_t)buf;
	*data_count = (mach_msg_type_number_t)bytes_wanted;
	return KERN_SUCCESS;
}

kern_return_t
ds_device_read_inband(mach_port_t device, mach_port_t reply,
		      mach_msg_type_name_t reply_poly,
		      dev_mode_t mode, recnum_t recnum,
		      io_buf_len_t bytes_wanted,
		      io_buf_ptr_inband_t data,
		      mach_msg_type_number_t *data_count)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_device_write(mach_port_t device, mach_port_t reply,
		mach_msg_type_name_t reply_poly,
		dev_mode_t mode, recnum_t recnum,
		io_buf_ptr_t data, mach_msg_type_number_t data_count,
		io_buf_len_t *bytes_written)
{
	unsigned int total, offset, lba, chunk;

	if (active_port < 0)
		return D_NO_SUCH_DEVICE;
	if (data_count <= 0)
		return D_INVALID_SIZE;

	/* Invalidate readahead cache — disk content is changing */
	if (ra_cache.buf != 0) {
		vm_deallocate(mach_task_self(),
			      ra_cache.buf, ra_cache.buf_size);
		ra_cache.buf = 0;
		ra_cache.lba_count = 0;
	}

	/* Round up to sector boundary */
	total = (unsigned int)data_count;
	if (total % SECTOR_SIZE)
		total = (total + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);

	/* Write in batches of up to batch_data_size */
	lba = recnum;
	for (offset = 0; offset < total; offset += chunk) {
		unsigned int batch_sects;

		chunk = total - offset;
		if (chunk > batch_data_size)
			chunk = batch_data_size;
		batch_sects = chunk / SECTOR_SIZE;

		memcpy((void *)data_uva, (void *)((vm_offset_t)data + offset),
		       chunk);
		if (ahci_submit_batch(active_port, lba, batch_sects, 1) < 0) {
			/* Deallocate OOL data from sender */
			vm_deallocate(mach_task_self(),
				      (vm_offset_t)data, data_count);
			return D_IO_ERROR;
		}
		lba += batch_sects;
	}

	/* Deallocate OOL data from sender */
	vm_deallocate(mach_task_self(), (vm_offset_t)data, data_count);

	*bytes_written = (io_buf_len_t)data_count;
	return KERN_SUCCESS;
}

kern_return_t
ds_device_write_inband(mach_port_t device, mach_port_t reply,
		       mach_msg_type_name_t reply_poly,
		       dev_mode_t mode, recnum_t recnum,
		       io_buf_ptr_inband_t data,
		       mach_msg_type_number_t data_count,
		       io_buf_len_t *bytes_written)
{
	return D_INVALID_OPERATION;
}

/* ================================================================
 * Batch write: multiple non-contiguous blocks in one IPC message
 * ================================================================ */

kern_return_t
ds_device_write_batch(mach_port_t device, mach_port_t reply,
		      mach_msg_type_name_t reply_poly,
		      dev_mode_t mode,
		      recnum_t *recnums,
		      mach_msg_type_number_t recnumsCnt,
		      unsigned int *sizes,
		      mach_msg_type_number_t sizesCnt,
		      io_buf_ptr_t data,
		      mach_msg_type_number_t data_count,
		      io_buf_len_t *bytes_written)
{
	unsigned int i, data_off = 0, total_written = 0;

	if (active_port < 0)
		return D_NO_SUCH_DEVICE;
	if (recnumsCnt != sizesCnt || recnumsCnt == 0)
		return D_INVALID_SIZE;

	/* Invalidate readahead cache — disk content is changing */
	if (ra_cache.buf != 0) {
		vm_deallocate(mach_task_self(),
			      ra_cache.buf, ra_cache.buf_size);
		ra_cache.buf = 0;
		ra_cache.lba_count = 0;
	}

	for (i = 0; i < recnumsCnt; i++) {
		unsigned int sz = sizes[i];
		unsigned int total, lba, off, chunk;

		if (sz == 0)
			continue;
		if (data_off + sz > data_count)
			goto bad;

		/* Round up to sector boundary */
		total = (sz + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
		lba = recnums[i];

		for (off = 0; off < total; off += chunk) {
			unsigned int sects, cpy;

			chunk = total - off;
			if (chunk > batch_data_size)
				chunk = batch_data_size;
			sects = chunk / SECTOR_SIZE;

			cpy = (off + chunk <= sz) ? chunk
						  : (sz > off ? sz - off : 0);
			if (cpy < chunk)
				memset((void *)data_uva, 0, chunk);
			if (cpy > 0)
				memcpy((void *)data_uva,
				       (void *)((vm_offset_t)data +
						data_off + off),
				       cpy);
			if (ahci_submit_batch(active_port, lba,
					      sects, 1) < 0)
				goto io_err;
			lba += sects;
		}

		data_off += sz;
		total_written += sz;
	}

	vm_deallocate(mach_task_self(), (vm_offset_t)data, data_count);
	*bytes_written = (io_buf_len_t)total_written;
	return KERN_SUCCESS;

bad:
	vm_deallocate(mach_task_self(), (vm_offset_t)data, data_count);
	return D_INVALID_SIZE;
io_err:
	vm_deallocate(mach_task_self(), (vm_offset_t)data, data_count);
	return D_IO_ERROR;
}

kern_return_t
ds_device_get_status(mach_port_t device, dev_flavor_t flavor,
		     dev_status_t status,
		     mach_msg_type_number_t *status_count)
{
	if (flavor == DEV_GET_SIZE) {
		status[DEV_GET_SIZE_DEVICE_SIZE] = (int)disk_sectors;
		status[DEV_GET_SIZE_RECORD_SIZE] = (int)SECTOR_SIZE;
		*status_count = DEV_GET_SIZE_COUNT;
		return KERN_SUCCESS;
	}
	return D_INVALID_OPERATION;
}

kern_return_t
ds_device_set_status(mach_port_t device, dev_flavor_t flavor,
		     dev_status_t status,
		     mach_msg_type_number_t status_count)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_device_map(mach_port_t device, vm_prot_t prot,
	      vm_offset_t offset, vm_size_t size,
	      mach_port_t *pager, boolean_t unmap)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_device_set_filter(mach_port_t device, mach_port_t receive_port,
		     int priority, filter_array_t filter,
		     mach_msg_type_number_t filter_count)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_io_done_queue_create(mach_port_t host, mach_port_t *queue)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_io_done_queue_terminate(mach_port_t queue)
{
	return D_INVALID_OPERATION;
}

/* ================================================================
 * Combined demux: device RPC + IRQ notifications
 * ================================================================ */

static boolean_t
ahci_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
	/* Try MIG device server demux first */
	if (device_server(in, out))
		return TRUE;

	/* Try batch write subsystem */
	if (ahci_batch_server(in, out))
		return TRUE;

	/* IRQ notification — acknowledge, no reply */
	if (in->msgh_id >= IRQ_NOTIFY_MSGH_BASE) {
		ahci_write(AHCI_IS, ~0u);
		port_write(active_port, PORT_IS, ~0u);
		((mig_reply_error_t *)out)->RetCode = MIG_NO_REPLY;
		((mig_reply_error_t *)out)->Head.msgh_size =
			sizeof(mig_reply_error_t);
		return TRUE;
	}

	return FALSE;
}

/* ================================================================
 * Main entry point
 * ================================================================ */

int
main(int argc, char **argv)
{
	kern_return_t kr;

	kr = bootstrap_ports(bootstrap_port,
			     &host_port, &device_port,
			     &root_ledger_wired, &root_ledger_paged,
			     &security_port);
	if (kr != KERN_SUCCESS)
		_exit(1);

	printf_init(device_port);
	panic_init(host_port);
	master_device = device_port;

	printf("\n=== AHCI userspace driver (real I/O) ===\n");

	/* IRQ notification port */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &irq_port);
	if (kr != KERN_SUCCESS) {
		printf("ahci: irq port alloc failed\n");
		return 1;
	}
	kr = mach_port_insert_right(mach_task_self(), irq_port, irq_port,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("ahci: irq port right failed\n");
		return 1;
	}

	/* Service port for device RPC (device_read, etc.) */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &ahci_service_port);
	if (kr != KERN_SUCCESS) {
		printf("ahci: service port alloc failed\n");
		return 1;
	}
	kr = mach_port_insert_right(mach_task_self(),
		ahci_service_port, ahci_service_port,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("ahci: service port right failed\n");
		return 1;
	}

	/* Port set: receive on both IRQ and RPC ports */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_PORT_SET, &port_set);
	if (kr != KERN_SUCCESS) {
		printf("ahci: port set alloc failed\n");
		return 1;
	}
	mach_port_move_member(mach_task_self(), irq_port, port_set);
	mach_port_move_member(mach_task_self(), ahci_service_port, port_set);

	/* Find and initialise the controller */
	if (!pci_scan_ahci()) {
		printf("ahci: no controller, exiting\n");
		return 1;
	}
	if (ahci_init() < 0) {
		printf("ahci: init failed\n");
		return 1;
	}

	/* Identify the attached disk and detect slot count */
	ahci_identify(active_port);

	/* Reallocate DMA buffers for detected batch_slots */
	if (ahci_realloc_batch_buffers() < 0) {
		printf("ahci: DMA realloc failed, falling back to 1 slot\n");
		batch_slots = 1;
		batch_data_size = SLOT_DATA_SIZE;
		ra_sectors = SECTORS_PER_SLOT;
	}

	/* Run sequential read benchmark */
	ahci_benchmark(active_port);

	/* Register with the name server so other tasks can find us */
	kr = netname_check_in(name_server_port, "ahci_driver",
			      mach_task_self(), ahci_service_port);
	if (kr != KERN_SUCCESS)
		printf("ahci: netname_check_in failed (kr=%d) "
		       "— continuing without registration\n", kr);
	else
		printf("ahci: registered as 'ahci_driver' with name server\n");

	printf("ahci: init complete, entering message loop\n");

	/*
	 * Message loop: combined demux handles both device RPCs
	 * (device_read, device_get_status, etc.) and IRQ notifications.
	 */
	mach_msg_server(ahci_demux, 8192, port_set,
			MACH_MSG_OPTION_NONE);

	/* mach_msg_server never returns on success */
	printf("ahci: mach_msg_server exited unexpectedly\n");
	return 1;
}
