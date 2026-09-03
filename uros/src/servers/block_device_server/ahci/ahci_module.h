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

/*
 * ⚠️ Included here and not left to the .c file's include order, because the
 * _Static_assert below needs MAX_DISKS_PER_CTRL and an assertion that
 * silently does not compile is not an assertion.
 */
#include "../block_server.h"

/* ================================================================
 * Per-port state
 * ================================================================ */

/*
 * 🔴 THIRTY-TWO, BECAUSE THAT IS WHAT THE HARDWARE CAN HAVE.
 *
 * PxPI -- the Ports Implemented register -- is a 32-bit mask with one bit per
 * port, so an AHCI controller has at most 32 and QEMU's ich9 already reports
 * six (PI=0x3f).  This was 4, and nothing anywhere said why: `git log -S'
 * finds it introduced by #396, a commit that MOVED the file.  It is an
 * inherited number, not a decision, and a driver that stops looking after the
 * fourth disk on a controller that has six is wrong on hardware we already
 * boot on.
 *
 * ⚠️ The cost is the struct array and nothing else.  A port's command list
 * and FIS area are allocated when the port is found to have a device, so
 * ports that do not exist cost their ahci_port_info and no DMA.
 */
#define MAX_AHCI_PORTS		32

/*
 * 🔑 AND THE FRAMEWORK HAS TO BE ABLE TO HOLD THEM.
 *
 * get_disks() writes into an array the framework owns, bounded by
 * MAX_DISKS_PER_CTRL, and these two numbers were equal by coincidence: they
 * live in different files, neither mentions the other, and nothing tied
 * them.  Raising this one alone -- the natural thing to do after reading the
 * specification -- would have written past the end of ctrl->disks[].
 *
 * So the relationship is stated where it can fail at compile time rather
 * than remembered.
 */
_Static_assert(MAX_AHCI_PORTS <= MAX_DISKS_PER_CTRL,
	       "get_disks() fills an array of MAX_DISKS_PER_CTRL and no more");

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
	vm_address_t	clb_dma, clb_uva;
	vm_address_t	fb_dma,  fb_uva;
	vm_address_t	dma_kva;
};

/* ================================================================
 * Module-private state — one per probed AHCI controller
 * ================================================================ */

/*
 * Scatter-gather: each slot uses PRDT_PER_SLOT pages (128 KB),
 * allowing multi-entry PRDTs with non-contiguous physical pages.
 */
/*
 * How many pages one scatter-gather allocation describes.  Four megabytes,
 * which is the buffer this driver keeps for batched I/O.
 *
 * ⚠️ NOT the same number as PRDT_PER_SLOT, and they are easy to confuse: this
 * bounds the ALLOCATION, that one bounds how many of its pages a single
 * command table can name.
 */
#define AHCI_MAX_SG_PAGES	1024u

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
	vm_address_t	ct_kva, ct_uva, ct_dma;

	/* Shared data buffer (scatter-gather) */
	vm_address_t	data_kva, data_uva, data_dma;

	/*
	 * 🔴 vm_address_t since #520 moved dma_sg_addr_t out of line and
	 * widened it.  It was natural_t, which was the WIRE's width and not a
	 * choice: the reply used to be an inline array of 32-bit elements, so
	 * no type written here could have held a page above four gigabytes.
	 *
	 * ⚠️ 8 KiB in BSS rather than 4, and it is BSS -- struct ahci_state is
	 * static.  The same widening on the stack is what made the inline
	 * array unaffordable in the first place.
	 */
	vm_address_t	data_dma_list[AHCI_MAX_SG_PAGES];
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
 * `caller_pa' arrives from a client over device_read_phys/device_write_phys,
 * whose dma_sg_addr_t is now an inline array of vm_address_t (#520) -- so the
 * parameter's reach and the interface's are the same again, which is the only
 * arrangement in which neither has to be remembered.
 */
int  ahci_submit_phys(struct ahci_state *st, int port_idx,
		      uint32_t start_lba, unsigned int nsectors,
		      int write, vm_address_t *caller_pa,
		      unsigned int n_pa, unsigned int total_bytes);


/*
 * The bus/device/function this controller sits at, packed the way
 * device_master.defs wants it: (bus << 8) | (dev << 3) | func.  A DMA
 * buffer says which device is going to read it (#432).
 */
#define	AHCI_BDF(st)	(((st)->pci_bus << 8) \
			 | ((st)->pci_slot << 3) \
			 | (st)->pci_func)

#endif /* _AHCI_MODULE_H_ */
