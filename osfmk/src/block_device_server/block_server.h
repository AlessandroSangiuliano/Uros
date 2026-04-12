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
 * block_server.h — Modular block device server framework
 *
 * Defines the module interface (block_driver_ops), controller and
 * partition structures, and framework-level constants and declarations.
 * Each hardware driver (AHCI, virtio-blk, ...) implements a module
 * that plugs into this framework via the ops vtable.
 */

#ifndef _BLOCK_SERVER_H_
#define _BLOCK_SERVER_H_

#include <mach.h>
#include <mach/mach_types.h>
#include <stdint.h>

/* ================================================================
 * Constants
 * ================================================================ */

#define SECTOR_SIZE		512u

#define MAX_CONTROLLERS		8
#define MAX_DISKS_PER_CTRL	4
#define MAX_MBR_PARTS		4
#define MAX_PARTITIONS		32

#define MBR_TYPE_LINUX		0x83

#define IRQ_NOTIFY_MSGH_BASE	3000

/* ================================================================
 * Disk info — returned by module get_disks()
 * ================================================================ */

struct blk_disk_info {
	uint32_t	total_sectors;		/* 512-byte sectors */
	int		ncq_supported;		/* 0/1 */
	unsigned int	ncq_depth;		/* 0 if no NCQ */
	unsigned int	max_transfer_bytes;	/* max single I/O */
};

/* ================================================================
 * Module interface: block_driver_ops
 *
 * Each hardware driver implements this vtable.  The framework calls
 * match() during PCI scan, probe() to initialise hardware, and the
 * I/O functions during normal operation.
 * ================================================================ */

struct block_driver_ops {
	const char	*name;

	/*
	 * PCI matching — return nonzero if this module handles the device
	 * identified by vendor_device (PCI_VENDOR_ID register) and
	 * class_rev (PCI_CLASS_REV register).
	 */
	int  (*match)(unsigned int vendor_device, unsigned int class_rev);

	/*
	 * Probe and initialise the hardware.  Receives PCI bus/device/func,
	 * the kernel device_master port, and a pre-allocated IRQ port.
	 * On success, stores an opaque private handle in *priv and
	 * returns 0.  On failure returns negative.
	 */
	int  (*probe)(unsigned int bus, unsigned int slot, unsigned int func,
		      mach_port_t master_device, mach_port_t irq_port,
		      void **priv);

	/*
	 * Enumerate disks attached to this controller.  Fills up to
	 * max_disks entries in info[] and returns the number of disks found.
	 */
	int  (*get_disks)(void *priv, struct blk_disk_info *info,
			  int max_disks);

	/*
	 * Read sectors from disk 'disk' starting at 'lba'.
	 * Allocates a buffer (vm_allocate) and returns it in *buf.
	 * *buf_size is set to the number of bytes actually read.
	 * Returns 0 on success, negative on error.
	 */
	int  (*read_sectors)(void *priv, int disk, uint32_t lba,
			     unsigned int count,
			     vm_offset_t *buf, unsigned int *buf_size);

	/*
	 * Write sectors to disk 'disk' starting at 'lba'.
	 * Data is at 'buf', 'size' bytes.
	 * Returns 0 on success, negative on error.
	 */
	int  (*write_sectors)(void *priv, int disk, uint32_t lba,
			      unsigned int count,
			      vm_offset_t buf, unsigned int size);

	/*
	 * Acknowledge an IRQ from this controller.
	 */
	void (*irq_handler)(void *priv);

	/*
	 * Optional: read using caller-supplied physical addresses for
	 * zero-copy DMA.  NULL if not supported.
	 */
	int  (*read_sectors_phys)(void *priv, int disk, uint32_t lba,
				  unsigned int count,
				  unsigned int *phys_addrs,
				  unsigned int n_pa,
				  unsigned int total_bytes);

	/*
	 * Optional: write using caller-supplied physical addresses.
	 * NULL if not supported.
	 */
	int  (*write_sectors_phys)(void *priv, int disk, uint32_t lba,
				   unsigned int count,
				   unsigned int *phys_addrs,
				   unsigned int n_pa,
				   unsigned int total_bytes);

	/*
	 * Optional: batch write — multiple non-contiguous blocks in one
	 * operation.  NULL if not supported.
	 */
	int  (*write_batch)(void *priv, int disk,
			    uint32_t *lbas, unsigned int *sizes,
			    unsigned int n,
			    vm_offset_t buf, unsigned int buf_size);
};

/* ================================================================
 * Controller — one per probed PCI device
 * ================================================================ */

struct blk_controller {
	const struct block_driver_ops	*ops;
	void		*priv;
	unsigned int	pci_bus, pci_slot, pci_func;
	mach_port_t	irq_port;
	int		n_disks;
	struct blk_disk_info disks[MAX_DISKS_PER_CTRL];
};

/* ================================================================
 * Partition — one per MBR/GPT partition found on each disk
 * ================================================================ */

struct blk_partition {
	struct blk_controller	*ctrl;
	int		disk_index;
	uint32_t	start_lba;
	uint32_t	num_sectors;
	mach_port_t	recv_port;
	char		name[32];
};

/* ================================================================
 * MBR structures
 * ================================================================ */

#define MBR_BOOTSZ	446
#define MBR_NUMPART	4
#define MBR_MAGIC	0xAA55

struct mbr_part_entry {
	unsigned char	bootid;
	unsigned char	beghead;
	unsigned char	begsect;
	unsigned char	begcyl;
	unsigned char	systid;
	unsigned char	endhead;
	unsigned char	endsect;
	unsigned char	endcyl;
	uint32_t	relsect;
	uint32_t	numsect;
} __attribute__((packed));

struct mbr_block {
	char			bootinst[MBR_BOOTSZ];
	struct mbr_part_entry	parts[MBR_NUMPART];
	uint16_t		signature;
} __attribute__((packed));

/* ================================================================
 * Global state — shared between framework files
 * ================================================================ */

extern struct blk_controller	controllers[];
extern int			n_controllers;

extern struct blk_partition	partitions[];
extern int			n_partitions;

extern mach_port_t	host_port;
extern mach_port_t	device_port;
extern mach_port_t	master_device;
extern mach_port_t	security_port;
extern mach_port_t	root_ledger_wired;
extern mach_port_t	root_ledger_paged;
extern mach_port_t	port_set;

/* ================================================================
 * Framework functions (block_partition.c)
 * ================================================================ */

int  blk_read_mbr(struct blk_controller *ctrl, int disk_idx,
		   const char *prefix);
void blk_register_partitions(void);

/* ================================================================
 * RDTSC helper — shared by benchmark code
 * ================================================================ */

static inline uint64_t
rdtsc(void)
{
	uint64_t tsc;
	__asm__ volatile("rdtsc" : "=A" (tsc));
	return tsc;
}

/* ================================================================
 * Module ops externs
 * ================================================================ */

extern const struct block_driver_ops ahci_module_ops;
extern const struct block_driver_ops virtio_blk_module_ops;

/* Not yet in our exported headers */
extern kern_return_t mach_port_set_protected_payload(
    mach_port_t task, mach_port_t port, unsigned long payload);

#endif /* _BLOCK_SERVER_H_ */
