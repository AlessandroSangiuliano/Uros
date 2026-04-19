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
 * ahci_io.c — AHCI I/O operations: command submission, read/write,
 *             batched scatter-gather, and physical-address DMA.
 */

#include <mach.h>
#include <sa_mach.h>
#include <stdio.h>
#include "ahci_module.h"

/* ================================================================
 * Single command submission (polling, slot 0)
 * ================================================================ */

int
ahci_submit_cmd(struct ahci_state *st, int port_idx,
		struct ata_fis_h2d *fis,
		unsigned int buf_pa, unsigned int buf_size,
		int write)
{
	int port = st->ports[port_idx].hba_port;
	struct ahci_cmd_hdr   *hdr =
		(struct ahci_cmd_hdr *)st->ports[port_idx].clb_uva;
	struct ahci_cmd_table *tbl = (struct ahci_cmd_table *)st->ct_uva;
	int i;

	for (i = 0; i < 1000000; i++)
		if (!(port_read(st, port, PORT_TFD) &
		      (PORT_TFD_STS_BSY | PORT_TFD_STS_DRQ)))
			break;

	port_write(st, port, PORT_IS, ~0u);

	hdr[0].opts  = CMD_HDR_CFL(5);
	if (write)
		hdr[0].opts |= CMD_HDR_W;
	hdr[0].prdtl = (buf_size > 0) ? 1 : 0;
	hdr[0].prdbc = 0;
	hdr[0].ctba  = st->ct_pa;
	hdr[0].ctbau = 0;
	for (i = 0; i < 4; i++)
		hdr[0].rsvd[i] = 0;

	memset(tbl, 0, sizeof(struct ahci_cmd_table));
	memcpy(tbl->cfis, fis, 20);

	if (buf_size > 0) {
		tbl->prdt[0].dba  = buf_pa;
		tbl->prdt[0].dbau = 0;
		tbl->prdt[0].rsvd = 0;
		tbl->prdt[0].dbc  = PRDT_DBC(buf_size) | PRDT_IOC;
	}

	port_write(st, port, PORT_CI, 1u);

	for (i = 0; i < 5000000; i++) {
		uint32_t is = port_read(st, port, PORT_IS);
		if (is & PORT_IS_TFES) {
			printf("ahci: task file error  IS=0x%08X  TFD=0x%08X\n",
			       is, port_read(st, port, PORT_TFD));
			port_write(st, port, PORT_IS, PORT_IS_TFES);
			return -1;
		}
		if (!(port_read(st, port, PORT_CI) & 1u))
			return 0;
	}

	printf("ahci: command timed out  CI=0x%08X\n",
	       port_read(st, port, PORT_CI));
	return -1;
}

/* ================================================================
 * READ DMA EXT (LBA48) — single slot
 * ================================================================ */

int
ahci_read_sectors_hw(struct ahci_state *st, int port_idx,
		     uint32_t lba, uint16_t count)
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
	fis.sector_count     = count & 0xFF;
	fis.sector_count_exp = (count >> 8) & 0xFF;

	return ahci_submit_cmd(st, port_idx, &fis,
			       st->data_pa, (unsigned int)count * 512, 0);
}

/* ================================================================
 * WRITE DMA EXT (LBA48) — single slot
 * ================================================================ */

int
ahci_write_sectors_hw(struct ahci_state *st, int port_idx,
		      uint32_t lba, uint16_t count)
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
	fis.sector_count     = count & 0xFF;
	fis.sector_count_exp = (count >> 8) & 0xFF;

	return ahci_submit_cmd(st, port_idx, &fis,
			       st->data_pa, (unsigned int)count * 512, 1);
}

/* ================================================================
 * Batched command submission (scatter-gather, multi-slot)
 * ================================================================ */

int
ahci_submit_batch(struct ahci_state *st, int port_idx,
		  uint32_t start_lba, unsigned int nsectors,
		  int write)
{
	int port = st->ports[port_idx].hba_port;
	int port_ncq = st->ports[port_idx].ncq_supported;
	struct ahci_cmd_hdr *hdr =
		(struct ahci_cmd_hdr *)st->ports[port_idx].clb_uva;
	unsigned int nslots, slot, sects_per_slot;
	unsigned int ci_mask;
	int i;

	nslots = (nsectors + SECTORS_PER_SLOT - 1) / SECTORS_PER_SLOT;
	if (nslots > st->batch_slots)
		nslots = st->batch_slots;

	for (i = 0; i < 1000000; i++)
		if (!(port_read(st, port, PORT_TFD) &
		      (PORT_TFD_STS_BSY | PORT_TFD_STS_DRQ)))
			break;

	port_write(st, port, PORT_IS, ~0u);

	for (slot = 0; slot < nslots; slot++) {
		struct ahci_cmd_table *tbl;
		struct ata_fis_h2d *fis;
		uint32_t lba;
		unsigned int slot_bytes, n_prdt, p;

		sects_per_slot = nsectors > SECTORS_PER_SLOT
			? SECTORS_PER_SLOT : nsectors;
		slot_bytes = sects_per_slot * 512u;
		lba = start_lba + slot * SECTORS_PER_SLOT;

		n_prdt = (slot_bytes + 4095u) / 4096u;
		if (n_prdt > PRDT_PER_SLOT)
			n_prdt = PRDT_PER_SLOT;

		hdr[slot].opts  = CMD_HDR_CFL(5);
		if (write)
			hdr[slot].opts |= CMD_HDR_W;
		hdr[slot].prdtl = n_prdt;
		hdr[slot].prdbc = 0;
		hdr[slot].ctba  = st->ct_pa + slot * CT_STRIDE;
		hdr[slot].ctbau = 0;
		hdr[slot].rsvd[0] = 0;
		hdr[slot].rsvd[1] = 0;
		hdr[slot].rsvd[2] = 0;
		hdr[slot].rsvd[3] = 0;

		tbl = (struct ahci_cmd_table *)(st->ct_uva + slot * CT_STRIDE);
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

		if (port_ncq) {
			fis->command      = write
				? ATA_CMD_WRITE_FPDMA_QUEUED
				: ATA_CMD_READ_FPDMA_QUEUED;
			fis->features     = sects_per_slot & 0xFF;
			fis->features_exp = (sects_per_slot >> 8) & 0xFF;
			fis->sector_count     = (slot << 3);
			fis->sector_count_exp = 0;
		} else {
			fis->command      = write
				? ATA_CMD_WRITE_DMA_EXT
				: ATA_CMD_READ_DMA_EXT;
			fis->sector_count     = sects_per_slot & 0xFF;
			fis->sector_count_exp = (sects_per_slot >> 8) & 0xFF;
		}

		for (p = 0; p < n_prdt; p++) {
			unsigned int page_idx = slot * PRDT_PER_SLOT + p;
			unsigned int page_bytes = 4096u;

			if (p == n_prdt - 1) {
				unsigned int rem = slot_bytes - p * 4096u;
				if (rem < 4096u)
					page_bytes = rem;
			}

			tbl->prdt[p].dba  = st->data_pa_list[page_idx];
			tbl->prdt[p].dbau = 0;
			tbl->prdt[p].rsvd = 0;
			tbl->prdt[p].dbc  = PRDT_DBC(page_bytes);
			if (p == n_prdt - 1)
				tbl->prdt[p].dbc |= PRDT_IOC;
		}

		nsectors -= sects_per_slot;
	}

	ci_mask = (1u << nslots) - 1;
	if (port_ncq)
		port_write(st, port, PORT_SACT, ci_mask);
	port_write(st, port, PORT_CI, ci_mask);

	for (i = 0; i < 5000000; i++) {
		uint32_t is = port_read(st, port, PORT_IS);
		if (is & PORT_IS_TFES) {
			printf("ahci: batch task file error  IS=0x%08X  "
			       "TFD=0x%08X\n",
			       is, port_read(st, port, PORT_TFD));
			port_write(st, port, PORT_IS, PORT_IS_TFES);
			return -1;
		}
		if (port_ncq) {
			if (!(port_read(st, port, PORT_SACT) & ci_mask))
				return 0;
		} else {
			if (!(port_read(st, port, PORT_CI) & ci_mask))
				return 0;
		}
	}

	printf("ahci: batch timed out  CI=0x%08X  SACT=0x%08X\n",
	       port_read(st, port, PORT_CI),
	       port_read(st, port, PORT_SACT));
	return -1;
}

/* ================================================================
 * Physical-address DMA (zero-copy)
 * ================================================================ */

int
ahci_submit_phys(struct ahci_state *st, int port_idx,
		 uint32_t start_lba, unsigned int nsectors,
		 int write, unsigned int *caller_pa,
		 unsigned int n_pa, unsigned int total_bytes)
{
	int port = st->ports[port_idx].hba_port;
	struct ahci_cmd_hdr   *hdr =
		(struct ahci_cmd_hdr *)st->ports[port_idx].clb_uva;
	struct ahci_cmd_table *tbl;
	struct ata_fis_h2d    *fis;
	unsigned int n_prdt, p, bytes_left;
	int i;

	if (nsectors == 0 || n_pa == 0)
		return -1;

	for (i = 0; i < 1000000; i++)
		if (!(port_read(st, port, PORT_TFD) &
		      (PORT_TFD_STS_BSY | PORT_TFD_STS_DRQ)))
			break;

	port_write(st, port, PORT_IS, ~0u);

	n_prdt = n_pa;
	if (n_prdt > PRDT_PER_SLOT)
		n_prdt = PRDT_PER_SLOT;

	hdr[0].opts  = CMD_HDR_CFL(5);
	if (write)
		hdr[0].opts |= CMD_HDR_W;
	hdr[0].prdtl = n_prdt;
	hdr[0].prdbc = 0;
	hdr[0].ctba  = st->ct_pa;
	hdr[0].ctbau = 0;
	hdr[0].rsvd[0] = 0;
	hdr[0].rsvd[1] = 0;
	hdr[0].rsvd[2] = 0;
	hdr[0].rsvd[3] = 0;

	tbl = (struct ahci_cmd_table *)st->ct_uva;
	memset(tbl, 0, CT_STRIDE);

	fis = (struct ata_fis_h2d *)tbl->cfis;
	fis->fis_type         = FIS_TYPE_H2D;
	fis->flags            = FIS_H2D_FLAG_CMD;
	fis->device           = ATA_DEV_LBA;
	fis->lba_lo           = (start_lba >>  0) & 0xFF;
	fis->lba_mid          = (start_lba >>  8) & 0xFF;
	fis->lba_hi           = (start_lba >> 16) & 0xFF;
	fis->lba_lo_exp       = (start_lba >> 24) & 0xFF;
	fis->command          = write
		? ATA_CMD_WRITE_DMA_EXT
		: ATA_CMD_READ_DMA_EXT;
	fis->sector_count     = nsectors & 0xFF;
	fis->sector_count_exp = (nsectors >> 8) & 0xFF;

	bytes_left = total_bytes;
	for (p = 0; p < n_prdt; p++) {
		unsigned int chunk = bytes_left > 4096u ? 4096u : bytes_left;

		tbl->prdt[p].dba  = caller_pa[p];
		tbl->prdt[p].dbau = 0;
		tbl->prdt[p].rsvd = 0;
		tbl->prdt[p].dbc  = PRDT_DBC(chunk);
		if (p == n_prdt - 1)
			tbl->prdt[p].dbc |= PRDT_IOC;
		bytes_left -= chunk;
	}

	port_write(st, port, PORT_CI, 1);

	for (i = 0; i < 5000000; i++) {
		uint32_t is = port_read(st, port, PORT_IS);
		if (is & PORT_IS_TFES) {
			printf("ahci: phys task file error  IS=0x%08X  "
			       "TFD=0x%08X\n",
			       is, port_read(st, port, PORT_TFD));
			port_write(st, port, PORT_IS, PORT_IS_TFES);
			return -1;
		}
		if (!(port_read(st, port, PORT_CI) & 1))
			return 0;
	}

	printf("ahci: phys timed out  CI=0x%08X\n",
	       port_read(st, port, PORT_CI));
	return -1;
}
