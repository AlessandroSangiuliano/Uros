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

/*
 * 🔴 THE ADDRESSES ARE vm_address_t (#520), for the reason spelled out in
 * virtio_blk/virtio_module.h: on i386 vm_offset_t and natural_t are the same
 * type and `unsigned int' was right by accident; on x86-64 they are
 * deliberately different and it silently loses the top half of every one.
 *
 * ⚠️ AHCI's own registers are split in halves for exactly this reason --
 * PxCLB/PxCLBU, PxFB/PxFBU, and the PRDT's DBA/DBAU -- so the controller has
 * always been able to reach above 4 GiB and this driver was the only thing
 * saying otherwise.
 */
struct ahci_port_info {
	int		hba_port;
	uint32_t	disk_sectors;
	int		ncq_supported;
	unsigned int	ncq_depth;
	vm_address_t	clb_pa, clb_uva;
	vm_address_t	fb_pa,  fb_uva;
	vm_address_t	dma_kva;
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
	vm_address_t	ct_kva, ct_uva, ct_pa;

	/* Shared data buffer (scatter-gather) */
	vm_address_t	data_kva, data_uva, data_pa;

	/*
	 * 🔴 natural_t, AND THAT IS THE WIRE'S WIDTH RATHER THAN A CHOICE.
	 * device_dma_alloc_sg returns `dma_sg_addr_t = array[*:4096] of
	 * natural_t', and natural_t is 32 bits on both targets on purpose --
	 * mach_port_t is one.  So this array cannot hold a physical address
	 * above 4 GiB no matter what type is written here, and widening it
	 * would only move the truncation to the stub.
	 *
	 * Spelled natural_t rather than `unsigned int' so that it says which
	 * of the two it is: the same eight characters used to mean both, and
	 * the whole of this port is the difference between them.
	 *
	 * ⚠️ The limit itself is #417's, and it is REAL here and not
	 * theoretical -- see the note on the scatter-gather path in
	 * ahci_io.c.
	 */
	natural_t	data_pa_list[1024];
	unsigned int	data_n_pages;

	/* Batching parameters (set after IDENTIFY) */
	unsigned int	batch_slots;
	unsigned int	batch_data_size;
	unsigned int	ra_sectors;
};

/*
 * The two halves AHCI splits every address into.
 *
 * ⚠️ VIA uint64_t, AND THAT IS NOT DECORATION.  `pa >> 32' on a vm_address_t
 * is undefined behaviour on i386, where the type is exactly 32 bits wide --
 * shifting by the width of the type is not "zero", it is whatever the compiler
 * decides, and it decides at -O2.  Widened first, the answer is zero there
 * because there is genuinely no upper half, and the real one here.
 */
static inline uint32_t ahci_pa_lo(vm_address_t pa)
{
	return (uint32_t)(uint64_t)pa;
}

static inline uint32_t ahci_pa_hi(vm_address_t pa)
{
	return (uint32_t)((uint64_t)pa >> 32);
}

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
		     vm_address_t buf_pa, vm_size_t buf_size,
		     int write);

int  ahci_read_sectors_hw(struct ahci_state *st, int port_idx,
			   uint32_t lba, uint16_t count);

int  ahci_write_sectors_hw(struct ahci_state *st, int port_idx,
			    uint32_t lba, uint16_t count);

int  ahci_submit_batch(struct ahci_state *st, int port_idx,
		       uint32_t start_lba, unsigned int nsectors,
		       int write);

/*
 * ⚠️ `caller_pa' is natural_t and not vm_address_t: these addresses arrive
 * from a client over device_read_phys/device_write_phys, whose dma_sg_addr_t
 * is an array of natural_t.  Widening the parameter would claim a reach the
 * interface it is fed from does not have.
 */
int  ahci_submit_phys(struct ahci_state *st, int port_idx,
		      uint32_t start_lba, unsigned int nsectors,
		      int write, natural_t *caller_pa,
		      unsigned int n_pa, unsigned int total_bytes);

#endif /* _AHCI_MODULE_H_ */
