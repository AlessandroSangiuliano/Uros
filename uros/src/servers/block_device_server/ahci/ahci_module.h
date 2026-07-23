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
 * ahci_module.h — AHCI module private state and internal declarations
 */

#ifndef _AHCI_MODULE_H_
#define _AHCI_MODULE_H_

#include <stdint.h>
#include <mach.h>
#include "ahci.h"

/* ================================================================
 * Per-port state
 * ================================================================ */

#define MAX_AHCI_PORTS		4

struct ahci_port_info {
	int		hba_port;
	uint32_t	disk_sectors;
	int		ncq_supported;
	unsigned int	ncq_depth;
	unsigned int	clb_pa, clb_uva;
	unsigned int	fb_pa,  fb_uva;
	unsigned int	dma_kva;
};

/* ================================================================
 * Module-private state — one per probed AHCI controller
 * ================================================================ */

/*
 * Scatter-gather: each slot uses PRDT_PER_SLOT pages (128 KB),
 * allowing multi-entry PRDTs with non-contiguous physical pages.
 */
#define SLOT_DATA_SIZE		(PRDT_PER_SLOT * 4096u)
#define CT_STRIDE		640u
#define SECTORS_PER_SLOT	(SLOT_DATA_SIZE / 512u)

struct ahci_state {
	volatile uint32_t	*abar;

	unsigned int	pci_bus, pci_slot, pci_func;
	unsigned int	ahci_irq;
	mach_port_t	master_device;

	struct ahci_port_info	ports[MAX_AHCI_PORTS];
	int			n_ports;

	/* Shared CT buffer */
	unsigned int	ct_kva, ct_uva, ct_pa;

	/* Shared data buffer (scatter-gather) */
	unsigned int	data_kva, data_uva, data_pa;
	unsigned int	data_pa_list[1024];
	unsigned int	data_n_pages;

	/* Batching parameters (set after IDENTIFY) */
	unsigned int	batch_slots;
	unsigned int	batch_data_size;
	unsigned int	ra_sectors;
};

/* ================================================================
 * MMIO accessors
 * ================================================================ */

static inline uint32_t
ahci_read(struct ahci_state *st, unsigned int reg)
{
	return *(volatile uint32_t *)((uint8_t *)st->abar + reg);
}

static inline void
ahci_write(struct ahci_state *st, unsigned int reg, uint32_t val)
{
	*(volatile uint32_t *)((uint8_t *)st->abar + reg) = val;
}

static inline uint32_t
port_read(struct ahci_state *st, int port, unsigned int reg)
{
	return ahci_read(st, AHCI_PORT_BASE + port * AHCI_PORT_SIZE + reg);
}

static inline void
port_write(struct ahci_state *st, int port, unsigned int reg, uint32_t val)
{
	ahci_write(st, AHCI_PORT_BASE + port * AHCI_PORT_SIZE + reg, val);
}

/* ================================================================
 * Internal functions (ahci_io.c)
 * ================================================================ */

int  ahci_submit_cmd(struct ahci_state *st, int port_idx,
		     struct ata_fis_h2d *fis,
		     unsigned int buf_pa, unsigned int buf_size,
		     int write);

int  ahci_read_sectors_hw(struct ahci_state *st, int port_idx,
			   uint32_t lba, uint16_t count);

int  ahci_write_sectors_hw(struct ahci_state *st, int port_idx,
			    uint32_t lba, uint16_t count);

int  ahci_submit_batch(struct ahci_state *st, int port_idx,
		       uint32_t start_lba, unsigned int nsectors,
		       int write);

int  ahci_submit_phys(struct ahci_state *st, int port_idx,
		      uint32_t start_lba, unsigned int nsectors,
		      int write, unsigned int *caller_pa,
		      unsigned int n_pa, unsigned int total_bytes);

#endif /* _AHCI_MODULE_H_ */
