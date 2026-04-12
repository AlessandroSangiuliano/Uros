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
 * ahci_module.c — AHCI driver module: match, probe, HBA init, IDENTIFY
 *
 * Implements the block_driver_ops interface for AHCI (SATA) controllers.
 * All AHCI HW logic from the original ahci_driver.c is here and in
 * ahci_io.c.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <sa_mach.h>
#include <device/device.h>
#include <device/device_types.h>
#include <stdio.h>
#include "../block_server.h"
#include "ahci_module.h"
#include "device_master.h"

/* PCI configuration registers */
#define PCI_COMMAND		0x04
#define PCI_BAR5		0x24
#define PCI_INTERRUPT_LINE	0x3C
#define PCI_CMD_MEM_ENABLE	(1 << 1)
#define PCI_CMD_BUS_MASTER	(1 << 2)

/* Static state — we support one AHCI controller for now */
static struct ahci_state ahci_st;

/* ================================================================
 * PCI match — AHCI SATA controller (class 01:06:01)
 * ================================================================ */

static int
ahci_match(unsigned int vendor_device, unsigned int class_rev)
{
	return ((class_rev >> 24) & 0xFF) == PCI_CLASS_STORAGE &&
	       ((class_rev >> 16) & 0xFF) == PCI_SUBCLASS_SATA &&
	       ((class_rev >>  8) & 0xFF) == PCI_PROGIF_AHCI;
}

/* ================================================================
 * Port start / stop helpers
 * ================================================================ */

static void
ahci_port_stop(struct ahci_state *st, int port)
{
	int i;
	uint32_t cmd;

	cmd = port_read(st, port, PORT_CMD);
	port_write(st, port, PORT_CMD, cmd & ~PORT_CMD_ST);
	for (i = 0; i < 500; i++)
		if (!(port_read(st, port, PORT_CMD) & PORT_CMD_CR))
			break;

	cmd = port_read(st, port, PORT_CMD);
	port_write(st, port, PORT_CMD, cmd & ~PORT_CMD_FRE);
	for (i = 0; i < 500; i++)
		if (!(port_read(st, port, PORT_CMD) & PORT_CMD_FR))
			break;
}

static void
ahci_port_start(struct ahci_state *st, int port)
{
	uint32_t cmd;
	int i;

	cmd = port_read(st, port, PORT_CMD);
	port_write(st, port, PORT_CMD, cmd | PORT_CMD_FRE | PORT_CMD_SUD);

	for (i = 0; i < 500000; i++)
		if (!(port_read(st, port, PORT_TFD) &
		      (PORT_TFD_STS_BSY | PORT_TFD_STS_DRQ)))
			break;

	cmd = port_read(st, port, PORT_CMD);
	port_write(st, port, PORT_CMD, cmd | PORT_CMD_ST);
}

/* ================================================================
 * HBA reset
 * ================================================================ */

static int
ahci_hba_reset(struct ahci_state *st)
{
	int i;

	if (ahci_read(st, AHCI_CAP2) & CAP2_BOH) {
		ahci_write(st, AHCI_BOHC,
			   ahci_read(st, AHCI_BOHC) | BOHC_OOS);
		for (i = 0; i < 25000; i++)
			if (!(ahci_read(st, AHCI_BOHC) & BOHC_BOS))
				break;
	}

	ahci_write(st, AHCI_GHC, GHC_AE);

	ahci_write(st, AHCI_GHC, ahci_read(st, AHCI_GHC) | GHC_HR);
	for (i = 0; i < 1000000; i++)
		if (!(ahci_read(st, AHCI_GHC) & GHC_HR))
			break;
	if (ahci_read(st, AHCI_GHC) & GHC_HR) {
		printf("ahci: HBA reset timed out\n");
		return -1;
	}

	ahci_write(st, AHCI_GHC, GHC_AE);

	printf("ahci: HBA reset OK  version=0x%08X  cap=0x%08X\n",
	       ahci_read(st, AHCI_VS), ahci_read(st, AHCI_CAP));
	return 0;
}

/* ================================================================
 * Port discovery
 * ================================================================ */

static int
ahci_port_scan(struct ahci_state *st)
{
	uint32_t pi = ahci_read(st, AHCI_PI);
	int port;

	st->n_ports = 0;
	for (port = 0; port < AHCI_MAX_PORTS; port++) {
		if (!(pi & (1u << port)))
			continue;
		if ((port_read(st, port, PORT_SSTS) & SSTS_DET_MASK)
		    != SSTS_DET_PRESENT)
			continue;
		if (st->n_ports >= MAX_AHCI_PORTS)
			break;

		printf("ahci: device on port %d  sig=0x%08X\n",
		       port, port_read(st, port, PORT_SIG));
		st->ports[st->n_ports].hba_port = port;
		st->n_ports++;
	}

	if (st->n_ports == 0) {
		printf("ahci: no device found (PI=0x%08X)\n", pi);
		return -1;
	}
	printf("ahci: found %d port(s)\n", st->n_ports);
	return 0;
}

/* ================================================================
 * Port initialisation
 * ================================================================ */

static int
ahci_port_init(struct ahci_state *st, int port_idx)
{
	struct ahci_port_info *pi = &st->ports[port_idx];
	int hba_port = pi->hba_port;
	kern_return_t kr;
	unsigned int dma_kva, dma_uva, dma_pa;

	kr = device_dma_alloc(st->master_device, 4096, &dma_kva, &dma_pa);
	if (kr != KERN_SUCCESS) {
		printf("ahci: port %d CLB/FB alloc failed\n", hba_port);
		return -1;
	}
	kr = device_dma_map_user(st->master_device, dma_kva, 4096,
				 mach_task_self(), &dma_uva);
	if (kr != KERN_SUCCESS) {
		printf("ahci: port %d CLB/FB map failed\n", hba_port);
		return -1;
	}

	pi->clb_pa  = dma_pa;
	pi->clb_uva = dma_uva;
	pi->fb_pa   = dma_pa  + 1024;
	pi->fb_uva  = dma_uva + 1024;
	pi->dma_kva = dma_kva;

	memset((void *)dma_uva, 0, 4096);

	ahci_port_stop(st, hba_port);

	port_write(st, hba_port, PORT_CLB,  pi->clb_pa);
	port_write(st, hba_port, PORT_CLBU, 0);
	port_write(st, hba_port, PORT_FB,   pi->fb_pa);
	port_write(st, hba_port, PORT_FBU,  0);

	port_write(st, hba_port, PORT_SERR, ~0u);
	port_write(st, hba_port, PORT_IS,   ~0u);

	port_write(st, hba_port, PORT_IE,
		   PORT_IE_DHRE | PORT_IE_SDBE | PORT_IE_TFEE);

	ahci_port_start(st, hba_port);

	printf("ahci: port %d initialised  cmd=0x%08X  tfd=0x%08X\n",
	       hba_port,
	       port_read(st, hba_port, PORT_CMD),
	       port_read(st, hba_port, PORT_TFD));
	return 0;
}

/* ================================================================
 * IDENTIFY DEVICE
 * ================================================================ */

static int
ahci_identify(struct ahci_state *st, int port_idx)
{
	struct ahci_port_info *pi = &st->ports[port_idx];
	struct ata_fis_h2d fis;
	uint16_t *buf = (uint16_t *)st->data_uva;
	char model[41];
	uint32_t lba28;
	unsigned int i;

	memset(&fis, 0, sizeof(fis));
	fis.fis_type = FIS_TYPE_H2D;
	fis.flags    = FIS_H2D_FLAG_CMD;
	fis.command  = ATA_CMD_IDENTIFY;
	fis.device   = 0;

	if (ahci_submit_cmd(st, port_idx, &fis, st->data_pa, 512, 0) < 0) {
		printf("ahci: IDENTIFY failed on port %d\n", pi->hba_port);
		return -1;
	}

	for (i = 0; i < 20; i++) {
		uint16_t w = buf[27 + i];
		model[i * 2]     = (char)((w >> 8) & 0xFF);
		model[i * 2 + 1] = (char)(w & 0xFF);
	}
	model[40] = '\0';
	for (i = 39; i > 0 && model[i] == ' '; i--)
		model[i] = '\0';

	lba28 = ((uint32_t)buf[61] << 16) | buf[60];
	pi->disk_sectors = lba28;

	printf("ahci: port %d model: %s\n", pi->hba_port, model);
	printf("ahci: port %d sectors: %u  capacity: ~%u MB\n",
	       pi->hba_port, lba28, lba28 / 2048);

	{
		uint32_t cap = ahci_read(st, AHCI_CAP);
		int hba_ncq = (cap & CAP_SNCQ) != 0;
		unsigned int hba_slots =
			((cap >> CAP_NCS_SHIFT) & CAP_NCS_MASK) + 1;
		int dev_ncq = (buf[ATA_ID_SATA_CAP] & ATA_ID_SATA_CAP_NCQ) != 0;
		unsigned int dev_qdepth = (buf[ATA_ID_QUEUE_DEPTH] & 0x1F) + 1;

		if (hba_ncq && dev_ncq) {
			pi->ncq_supported = 1;
			pi->ncq_depth = hba_slots < dev_qdepth
				    ? hba_slots : dev_qdepth;
			printf("ahci: port %d NCQ depth=%u\n",
			       pi->hba_port, pi->ncq_depth);
		} else {
			pi->ncq_supported = 0;
			pi->ncq_depth = 0;
		}

		if (hba_slots > st->batch_slots)
			st->batch_slots = hba_slots;
	}

	return 0;
}

/* ================================================================
 * Reallocate DMA buffers for detected batch_slots
 * ================================================================ */

static int
ahci_realloc_batch_buffers(struct ahci_state *st)
{
	kern_return_t kr;
	unsigned int ct_size;
	unsigned int n_pages;
	mach_msg_type_number_t pa_count = 1024;

	if (st->batch_slots <= 1)
		return 0;

	ct_size = st->batch_slots * CT_STRIDE;
	ct_size = (ct_size + 4095u) & ~4095u;

	/* Release old user mapping before freeing kernel DMA buffer */
	vm_deallocate(mach_task_self(), st->ct_uva, 4096);
	device_dma_free(st->master_device, st->ct_kva, 4096);

	kr = device_dma_alloc(st->master_device, ct_size,
			      &st->ct_kva, &st->ct_pa);
	if (kr != KERN_SUCCESS) {
		printf("ahci: CT realloc (%u bytes) failed\n", ct_size);
		return -1;
	}
	kr = device_dma_map_user(st->master_device, st->ct_kva, ct_size,
				 mach_task_self(), &st->ct_uva);
	if (kr != KERN_SUCCESS) {
		printf("ahci: CT remap failed\n");
		return -1;
	}

	/* Release old user mapping before freeing kernel DMA buffer */
	vm_deallocate(mach_task_self(), st->data_uva, 4096);
	device_dma_free(st->master_device, st->data_kva, 4096);

	n_pages = st->batch_slots * PRDT_PER_SLOT;
	if (n_pages > 1024)
		n_pages = 1024;

	kr = device_dma_alloc_sg(st->master_device, n_pages,
				 mach_task_self(),
				 &st->data_kva, &st->data_uva,
				 st->data_pa_list, &pa_count);
	if (kr != KERN_SUCCESS) {
		printf("ahci: scatter-gather alloc (%u pages) "
		       "failed (kr=%d)\n", n_pages, kr);
		return -1;
	}
	st->data_n_pages = n_pages;
	st->data_pa = st->data_pa_list[0];

	st->batch_data_size = st->batch_slots * SLOT_DATA_SIZE;
	st->ra_sectors = st->batch_slots * SECTORS_PER_SLOT;

	printf("ahci: DMA reallocated: ct=%u bytes  data=%u pages "
	       "(%u KB, scatter-gather, %u slots)\n",
	       ct_size, n_pages, n_pages * 4, st->batch_slots);

	return 0;
}

/* ================================================================
 * Probe — full AHCI controller initialisation
 * ================================================================ */

static int
ahci_probe(unsigned int bus, unsigned int slot, unsigned int func,
	   mach_port_t master_dev, mach_port_t irq,
	   void **priv)
{
	struct ahci_state *st = &ahci_st;
	kern_return_t kr;
	unsigned int bar5, cmd_reg, irq_reg;
	int i;

	st->pci_bus  = bus;
	st->pci_slot = slot;
	st->pci_func = func;
	st->master_device = master_dev;
	st->batch_slots = 1;

	/* Read BAR5 (ABAR) */
	kr = device_pci_config_read(master_dev, bus, slot, func,
				    PCI_BAR5, &bar5);
	if (kr != KERN_SUCCESS) {
		printf("ahci: failed to read BAR5\n");
		return -1;
	}
	printf("ahci: ABAR phys = 0x%08X\n", bar5 & ~0xFu);

	/* Enable PCI bus master + memory space */
	kr = device_pci_config_read(master_dev, bus, slot, func,
				    PCI_COMMAND, &cmd_reg);
	if (kr == KERN_SUCCESS) {
		cmd_reg |= PCI_CMD_MEM_ENABLE | PCI_CMD_BUS_MASTER;
		device_pci_config_write(master_dev, bus, slot, func,
					PCI_COMMAND, cmd_reg);
	}

	/* Read IRQ */
	kr = device_pci_config_read(master_dev, bus, slot, func,
				    PCI_INTERRUPT_LINE, &irq_reg);
	if (kr == KERN_SUCCESS) {
		st->ahci_irq = irq_reg & 0xFF;
		printf("ahci: IRQ = %u\n", st->ahci_irq);
	}

	/* Map ABAR */
	kr = device_mmio_map(master_dev, bar5 & ~0xFu, AHCI_ABAR_SIZE,
			     mach_task_self(), (unsigned int *)&st->abar);
	if (kr != KERN_SUCCESS) {
		printf("ahci: device_mmio_map failed (kr=%d)\n", kr);
		return -1;
	}
	printf("ahci: ABAR mapped at uva=0x%08X\n", (unsigned int)st->abar);

	/* Initial CT buffer (1 page) */
	kr = device_dma_alloc(master_dev, 4096, &st->ct_kva, &st->ct_pa);
	if (kr != KERN_SUCCESS) { printf("ahci: CT alloc failed\n"); return -1; }
	kr = device_dma_map_user(master_dev, st->ct_kva, 4096,
				 mach_task_self(), &st->ct_uva);
	if (kr != KERN_SUCCESS) { printf("ahci: CT map failed\n"); return -1; }

	/* Initial data buffer (1 page) */
	kr = device_dma_alloc(master_dev, 4096, &st->data_kva, &st->data_pa);
	if (kr != KERN_SUCCESS) { printf("ahci: data alloc failed\n"); return -1; }
	kr = device_dma_map_user(master_dev, st->data_kva, 4096,
				 mach_task_self(), &st->data_uva);
	if (kr != KERN_SUCCESS) { printf("ahci: data map failed\n"); return -1; }

	st->data_pa_list[0] = st->data_pa;
	st->data_n_pages = 1;

	printf("ahci: DMA: ct pa=0x%08X data pa=0x%08X\n",
	       st->ct_pa, st->data_pa);

	/* HBA reset + AHCI enable */
	if (ahci_hba_reset(st) < 0)
		return -1;

	/* Find and init ports */
	if (ahci_port_scan(st) < 0)
		return -1;

	for (i = 0; i < st->n_ports; i++) {
		if (ahci_port_init(st, i) < 0)
			return -1;
	}

	/* Register IRQ */
	if (st->ahci_irq > 0 && st->ahci_irq < 16) {
		kr = device_intr_register(master_dev, st->ahci_irq, irq,
					  MACH_MSG_TYPE_MAKE_SEND);
		if (kr == KERN_SUCCESS)
			printf("ahci: IRQ %u registered\n", st->ahci_irq);
	}

	/* Identify disks and detect batch slots */
	for (i = 0; i < st->n_ports; i++)
		ahci_identify(st, i);

	st->batch_data_size = st->batch_slots * SLOT_DATA_SIZE;
	st->ra_sectors = st->batch_slots * SECTORS_PER_SLOT;
	printf("ahci: batch_slots=%u  data=%u KB  readahead=%u sectors\n",
	       st->batch_slots, st->batch_data_size / 1024, st->ra_sectors);

	if (ahci_realloc_batch_buffers(st) < 0) {
		printf("ahci: DMA realloc failed, falling back to 1 slot\n");
		st->batch_slots = 1;
		st->batch_data_size = SLOT_DATA_SIZE;
		st->ra_sectors = SECTORS_PER_SLOT;
	}

	/*
	 * Note: GHC_IE (global interrupt enable) is NOT set here.
	 * The original ahci_driver never enabled it — IRQ forwarding
	 * works via the kernel's device_intr_register mechanism, not
	 * via AHCI's global interrupt enable bit.  Enabling GHC_IE
	 * causes an immediate IRQ delivery before the message loop
	 * is ready, which deadlocks the task.
	 */

	*priv = st;
	return 0;
}

/* ================================================================
 * get_disks — return disk info for each port
 * ================================================================ */

static int
ahci_get_disks(void *priv, struct blk_disk_info *info, int max_disks)
{
	struct ahci_state *st = (struct ahci_state *)priv;
	int i, count;

	count = st->n_ports;
	if (count > max_disks)
		count = max_disks;

	for (i = 0; i < count; i++) {
		info[i].total_sectors    = st->ports[i].disk_sectors;
		info[i].ncq_supported    = st->ports[i].ncq_supported;
		info[i].ncq_depth        = st->ports[i].ncq_depth;
		info[i].max_transfer_bytes = st->batch_data_size;
	}

	return count;
}

/* ================================================================
 * Module ops callbacks — delegate to ahci_io.c
 * ================================================================ */

static int
ahci_mod_read_sectors(void *priv, int disk, uint32_t lba,
		      unsigned int count,
		      vm_offset_t *buf, unsigned int *buf_size)
{
	struct ahci_state *st = (struct ahci_state *)priv;
	unsigned int total = count * SECTOR_SIZE;
	unsigned int offset, chunk;

	for (offset = 0; offset < total; offset += chunk) {
		unsigned int batch_sects;

		chunk = total - offset;
		if (chunk > st->batch_data_size)
			chunk = st->batch_data_size;
		batch_sects = chunk / SECTOR_SIZE;

		if (ahci_submit_batch(st, disk,
				      lba + offset / SECTOR_SIZE,
				      batch_sects, 0) < 0)
			return -1;
	}

	/*
	 * Data is in the DMA buffer (st->data_uva).
	 * Allocate a user buffer and copy it out.
	 */
	{
		kern_return_t kr;
		vm_offset_t out;

		kr = vm_allocate(mach_task_self(), &out, total, TRUE);
		if (kr != KERN_SUCCESS)
			return -1;
		memcpy((void *)out, (void *)st->data_uva, total);
		*buf = out;
		*buf_size = total;
	}
	return 0;
}

static int
ahci_mod_write_sectors(void *priv, int disk, uint32_t lba,
		       unsigned int count,
		       vm_offset_t buf, unsigned int size)
{
	struct ahci_state *st = (struct ahci_state *)priv;
	unsigned int total = count * SECTOR_SIZE;
	unsigned int offset, chunk;

	for (offset = 0; offset < total; offset += chunk) {
		unsigned int batch_sects;

		chunk = total - offset;
		if (chunk > st->batch_data_size)
			chunk = st->batch_data_size;
		batch_sects = chunk / SECTOR_SIZE;

		memcpy((void *)st->data_uva,
		       (void *)(buf + offset), chunk);
		if (ahci_submit_batch(st, disk,
				      lba + offset / SECTOR_SIZE,
				      batch_sects, 1) < 0)
			return -1;
	}
	return 0;
}

static void
ahci_mod_irq_handler(void *priv)
{
	struct ahci_state *st = (struct ahci_state *)priv;
	int i;

	ahci_write(st, AHCI_IS, ~0u);
	for (i = 0; i < st->n_ports; i++)
		port_write(st, st->ports[i].hba_port, PORT_IS, ~0u);
}

static int
ahci_mod_read_phys(void *priv, int disk, uint32_t lba,
		   unsigned int count,
		   unsigned int *phys_addrs, unsigned int n_pa,
		   unsigned int total_bytes)
{
	struct ahci_state *st = (struct ahci_state *)priv;
	return ahci_submit_phys(st, disk, lba, count, 0,
				phys_addrs, n_pa, total_bytes);
}

static int
ahci_mod_write_phys(void *priv, int disk, uint32_t lba,
		    unsigned int count,
		    unsigned int *phys_addrs, unsigned int n_pa,
		    unsigned int total_bytes)
{
	struct ahci_state *st = (struct ahci_state *)priv;
	return ahci_submit_phys(st, disk, lba, count, 1,
				phys_addrs, n_pa, total_bytes);
}

static int
ahci_mod_write_batch(void *priv, int disk,
		     uint32_t *lbas, unsigned int *sizes, unsigned int n,
		     vm_offset_t buf, unsigned int buf_size)
{
	struct ahci_state *st = (struct ahci_state *)priv;
	unsigned int i, data_off = 0;

	for (i = 0; i < n; i++) {
		unsigned int sz = sizes[i];
		unsigned int wr_total, lba, off, chunk;

		if (sz == 0)
			continue;
		if (data_off + sz > buf_size)
			return -1;

		wr_total = (sz + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
		lba = lbas[i];

		for (off = 0; off < wr_total; off += chunk) {
			unsigned int sects, cpy;

			chunk = wr_total - off;
			if (chunk > st->batch_data_size)
				chunk = st->batch_data_size;
			sects = chunk / SECTOR_SIZE;

			cpy = (off + chunk <= sz) ? chunk
						  : (sz > off ? sz - off : 0);
			if (cpy < chunk)
				memset((void *)st->data_uva, 0, chunk);
			if (cpy > 0)
				memcpy((void *)st->data_uva,
				       (void *)(buf + data_off + off), cpy);
			if (ahci_submit_batch(st, disk, lba,
					      sects, 1) < 0)
				return -1;
			lba += sects;
		}
		data_off += sz;
	}
	return 0;
}

/* ================================================================
 * Module ops vtable
 * ================================================================ */

const struct block_driver_ops ahci_module_ops = {
	.name			= "ahci",
	.match			= ahci_match,
	.probe			= ahci_probe,
	.get_disks		= ahci_get_disks,
	.read_sectors		= ahci_mod_read_sectors,
	.write_sectors		= ahci_mod_write_sectors,
	.irq_handler		= ahci_mod_irq_handler,
	.read_sectors_phys	= ahci_mod_read_phys,
	.write_sectors_phys	= ahci_mod_write_phys,
	.write_batch		= ahci_mod_write_batch,
};
