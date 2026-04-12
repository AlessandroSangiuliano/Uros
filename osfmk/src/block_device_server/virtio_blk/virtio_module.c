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
 * virtio_module.c — Virtio-blk driver module
 *
 * Implements block_driver_ops for virtio-blk PCI legacy transport.
 * All virtio HW logic from the original virtio_blk.c lives here.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <sa_mach.h>
#include <device/device.h>
#include <device/device_types.h>
#include <stdio.h>
#include "../block_server.h"
#include "virtio_module.h"
#include "device_master.h"

/* PCI configuration registers */
#define PCI_COMMAND		0x04
#define PCI_BAR0		0x10
#define PCI_INTERRUPT_LINE	0x3C
#define PCI_CMD_IO_ENABLE	(1 << 0)
#define PCI_CMD_BUS_MASTER	(1 << 2)

/* Static state — one virtio-blk controller */
static struct virtio_state virtio_st;

/* ================================================================
 * I/O port accessors
 * ================================================================ */

static inline uint32_t
vio_read32(struct virtio_state *st, unsigned int off)
{
	unsigned int val;
	device_io_port_read(st->master_device, st->iobase + off, 4, &val);
	return val;
}

static inline uint16_t
vio_read16(struct virtio_state *st, unsigned int off)
{
	unsigned int val;
	device_io_port_read(st->master_device, st->iobase + off, 2, &val);
	return (uint16_t)val;
}

static inline uint8_t
vio_read8(struct virtio_state *st, unsigned int off)
{
	unsigned int val;
	device_io_port_read(st->master_device, st->iobase + off, 1, &val);
	return (uint8_t)val;
}

static inline void
vio_write32(struct virtio_state *st, unsigned int off, uint32_t val)
{
	device_io_port_write(st->master_device, st->iobase + off, 4, val);
}

static inline void
vio_write16(struct virtio_state *st, unsigned int off, uint16_t val)
{
	device_io_port_write(st->master_device, st->iobase + off, 2, val);
}

static inline void
vio_write8(struct virtio_state *st, unsigned int off, uint8_t val)
{
	device_io_port_write(st->master_device, st->iobase + off, 1, val);
}

/* ================================================================
 * PCI match — virtio-blk (vendor 0x1AF4, device 0x1001 or 0x1042)
 * ================================================================ */

static int
virtio_match(unsigned int vendor_device, unsigned int class_rev)
{
	unsigned int vendor = vendor_device & 0xFFFF;
	unsigned int devid  = (vendor_device >> 16) & 0xFFFF;

	(void)class_rev;
	return vendor == VIRTIO_PCI_VENDOR &&
	       (devid == VIRTIO_PCI_DEVICE_BLK_TRANS ||
		devid == VIRTIO_PCI_DEVICE_BLK_MODERN);
}

/* ================================================================
 * Virtqueue setup
 * ================================================================ */

static int
virtqueue_setup(struct virtio_state *st)
{
	kern_return_t kr;
	unsigned int used_off;

	vio_write16(st, VIRTIO_PCI_QUEUE_SEL, 0);

	st->vq_size = vio_read16(st, VIRTIO_PCI_QUEUE_SIZE);
	if (st->vq_size == 0) {
		printf("virtio: queue 0 size is 0\n");
		return -1;
	}
	printf("virtio: queue 0 size = %u\n", st->vq_size);

	st->vq_alloc_size = VRING_TOTAL_SIZE(st->vq_size);
	st->vq_alloc_size = (st->vq_alloc_size + 4095u) & ~4095u;

	kr = device_dma_alloc(st->master_device, st->vq_alloc_size,
			      &st->vq_kva, &st->vq_pa);
	if (kr != KERN_SUCCESS) {
		printf("virtio: vq alloc failed (%u bytes)\n",
		       st->vq_alloc_size);
		return -1;
	}
	kr = device_dma_map_user(st->master_device, st->vq_kva,
				 st->vq_alloc_size,
				 mach_task_self(), &st->vq_uva);
	if (kr != KERN_SUCCESS) {
		printf("virtio: vq map failed\n");
		return -1;
	}

	memset((void *)st->vq_uva, 0, st->vq_alloc_size);

	st->vq_desc  = (struct vring_desc *)st->vq_uva;
	st->vq_avail = (struct vring_avail *)(st->vq_uva +
			VRING_AVAIL_OFFSET(st->vq_size));
	used_off = VRING_USED_OFFSET(st->vq_size);
	st->vq_used  = (struct vring_used *)(st->vq_uva + used_off);

	printf("virtio: vq pa=0x%08X  desc=+0  avail=+0x%X  used=+0x%X\n",
	       st->vq_pa, VRING_AVAIL_OFFSET(st->vq_size), used_off);

	vio_write32(st, VIRTIO_PCI_QUEUE_PFN, st->vq_pa / VRING_ALIGN);

	/* Request header + status DMA buffer (1 page) */
	kr = device_dma_alloc(st->master_device, 4096,
			      &st->req_kva, &st->req_pa);
	if (kr != KERN_SUCCESS) {
		printf("virtio: req alloc failed\n");
		return -1;
	}
	kr = device_dma_map_user(st->master_device, st->req_kva, 4096,
				 mach_task_self(), &st->req_uva);
	if (kr != KERN_SUCCESS) {
		printf("virtio: req map failed\n");
		return -1;
	}

	/* Data DMA buffer */
	kr = device_dma_alloc(st->master_device, DATA_BUF_SIZE,
			      &st->data_kva, &st->data_pa);
	if (kr != KERN_SUCCESS) {
		printf("virtio: data alloc failed\n");
		return -1;
	}
	kr = device_dma_map_user(st->master_device, st->data_kva,
				 DATA_BUF_SIZE,
				 mach_task_self(), &st->data_uva);
	if (kr != KERN_SUCCESS) {
		printf("virtio: data map failed\n");
		return -1;
	}

	st->last_used_idx = 0;
	return 0;
}

/* ================================================================
 * Submit a single virtio-blk request (polling)
 * ================================================================ */

static int
virtio_blk_request(struct virtio_state *st, uint32_t type,
		   uint64_t sector, unsigned int data_offset,
		   unsigned int data_len)
{
	struct virtio_blk_req_hdr *hdr;
	uint8_t *status_ptr;
	int timeout;

	hdr = (struct virtio_blk_req_hdr *)st->req_uva;
	hdr->type = type;
	hdr->reserved = 0;
	hdr->sector = sector;

	status_ptr = (uint8_t *)(st->req_uva + sizeof(*hdr));
	*status_ptr = 0xFF;

	/* Descriptor 0: header */
	st->vq_desc[0].addr  = (uint64_t)st->req_pa;
	st->vq_desc[0].len   = sizeof(struct virtio_blk_req_hdr);
	st->vq_desc[0].flags = VRING_DESC_F_NEXT;
	st->vq_desc[0].next  = 1;

	/* Descriptor 1: data */
	st->vq_desc[1].addr  = (uint64_t)(st->data_pa + data_offset);
	st->vq_desc[1].len   = data_len;
	st->vq_desc[1].flags = VRING_DESC_F_NEXT;
	if (type == VIRTIO_BLK_T_IN)
		st->vq_desc[1].flags |= VRING_DESC_F_WRITE;
	st->vq_desc[1].next  = 2;

	/* Descriptor 2: status */
	st->vq_desc[2].addr  = (uint64_t)(st->req_pa + sizeof(*hdr));
	st->vq_desc[2].len   = 1;
	st->vq_desc[2].flags = VRING_DESC_F_WRITE;
	st->vq_desc[2].next  = 0;

	st->vq_avail->ring[st->vq_avail->idx % st->vq_size] = 0;
	__asm__ volatile("" ::: "memory");
	st->vq_avail->idx++;

	vio_write16(st, VIRTIO_PCI_QUEUE_NOTIFY, 0);

	for (timeout = 0; timeout < 10000000; timeout++) {
		__asm__ volatile("pause" ::: "memory");
		if (((volatile struct vring_used *)st->vq_used)->idx
		    != st->last_used_idx) {
			vio_read8(st, VIRTIO_PCI_ISR);
			st->last_used_idx = st->vq_used->idx;
			if (*status_ptr != VIRTIO_BLK_S_OK) {
				printf("virtio: request failed, "
				       "status=%u\n", *status_ptr);
				return -1;
			}
			return 0;
		}
	}

	printf("virtio: request timeout\n");
	return -1;
}

/* ================================================================
 * Probe — full virtio-blk initialisation
 * ================================================================ */

static int
virtio_probe(unsigned int bus, unsigned int slot, unsigned int func,
	     mach_port_t master_dev, mach_port_t irq,
	     void **priv)
{
	struct virtio_state *st = &virtio_st;
	kern_return_t kr;
	unsigned int bar0, cmd_reg, irq_reg;
	uint32_t host_features, cap_lo;

	st->pci_bus  = bus;
	st->pci_slot = slot;
	st->pci_func = func;
	st->master_device = master_dev;

	/* Read BAR0 */
	kr = device_pci_config_read(master_dev, bus, slot, func,
				    PCI_BAR0, &bar0);
	if (kr != KERN_SUCCESS || !(bar0 & 1u)) {
		printf("virtio: BAR0 is not I/O space (0x%08X)\n", bar0);
		return -1;
	}
	st->iobase = bar0 & ~3u;
	printf("virtio: I/O base = 0x%04X\n", st->iobase);

	/* Enable I/O space + bus master */
	kr = device_pci_config_read(master_dev, bus, slot, func,
				    PCI_COMMAND, &cmd_reg);
	if (kr == KERN_SUCCESS) {
		cmd_reg |= PCI_CMD_IO_ENABLE | PCI_CMD_BUS_MASTER;
		device_pci_config_write(master_dev, bus, slot, func,
					PCI_COMMAND, cmd_reg);
	}

	/* Read IRQ */
	kr = device_pci_config_read(master_dev, bus, slot, func,
				    PCI_INTERRUPT_LINE, &irq_reg);
	if (kr == KERN_SUCCESS) {
		st->irq = irq_reg & 0xFF;
		printf("virtio: IRQ = %u\n", st->irq);
	}

	/* Device init sequence */
	vio_write8(st, VIRTIO_PCI_STATUS, 0);
	vio_write8(st, VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
	vio_write8(st, VIRTIO_PCI_STATUS,
		   VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

	host_features = vio_read32(st, VIRTIO_PCI_HOST_FEATURES);
	printf("virtio: host features = 0x%08X\n", host_features);
	vio_write32(st, VIRTIO_PCI_GUEST_FEATURES, 0);

	if (virtqueue_setup(st) < 0) {
		vio_write8(st, VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
		return -1;
	}

	/* Register IRQ */
	if (st->irq > 0 && st->irq < 16) {
		kr = device_intr_register(master_dev, st->irq, irq,
					  MACH_MSG_TYPE_MAKE_SEND);
		if (kr == KERN_SUCCESS)
			printf("virtio: IRQ %u registered\n", st->irq);
	}

	/* Driver OK */
	vio_write8(st, VIRTIO_PCI_STATUS,
		   VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
		   VIRTIO_STATUS_DRIVER_OK);

	/* Read capacity */
	cap_lo = vio_read32(st, VIRTIO_PCI_CONFIG + 0);
	st->disk_sectors = cap_lo;

	printf("virtio: capacity = %u sectors (%u MB)\n",
	       st->disk_sectors, st->disk_sectors / 2048);
	printf("virtio: status = 0x%02X\n",
	       vio_read8(st, VIRTIO_PCI_STATUS));

	*priv = st;
	return 0;
}

/* ================================================================
 * get_disks — virtio-blk has exactly 1 disk
 * ================================================================ */

static int
virtio_get_disks(void *priv, struct blk_disk_info *info, int max_disks)
{
	struct virtio_state *st = (struct virtio_state *)priv;

	if (max_disks < 1)
		return 0;

	info[0].total_sectors    = st->disk_sectors;
	info[0].ncq_supported    = 0;
	info[0].ncq_depth        = 0;
	info[0].max_transfer_bytes = DATA_BUF_SIZE;

	return 1;
}

/* ================================================================
 * Module ops callbacks
 * ================================================================ */

static int
virtio_mod_read_sectors(void *priv, int disk, uint32_t lba,
			unsigned int count,
			vm_offset_t *buf, unsigned int *buf_size)
{
	struct virtio_state *st = (struct virtio_state *)priv;
	unsigned int total = count * SECTOR_SIZE;
	unsigned int offset, chunk;
	kern_return_t kr;
	vm_offset_t out;

	(void)disk;

	kr = vm_allocate(mach_task_self(), &out, total, TRUE);
	if (kr != KERN_SUCCESS)
		return -1;

	for (offset = 0; offset < total; offset += chunk) {
		chunk = total - offset;
		if (chunk > DATA_BUF_SIZE)
			chunk = DATA_BUF_SIZE;

		if (virtio_blk_request(st, VIRTIO_BLK_T_IN,
				       lba + offset / SECTOR_SIZE,
				       0, chunk) < 0) {
			vm_deallocate(mach_task_self(), out, total);
			return -1;
		}
		memcpy((void *)(out + offset), (void *)st->data_uva, chunk);
	}

	*buf = out;
	*buf_size = total;
	return 0;
}

static int
virtio_mod_write_sectors(void *priv, int disk, uint32_t lba,
			 unsigned int count,
			 vm_offset_t buf, unsigned int size)
{
	struct virtio_state *st = (struct virtio_state *)priv;
	unsigned int total = count * SECTOR_SIZE;
	unsigned int offset, chunk;

	(void)disk;

	for (offset = 0; offset < total; offset += chunk) {
		chunk = total - offset;
		if (chunk > DATA_BUF_SIZE)
			chunk = DATA_BUF_SIZE;

		memcpy((void *)st->data_uva,
		       (void *)(buf + offset), chunk);
		if (virtio_blk_request(st, VIRTIO_BLK_T_OUT,
				       lba + offset / SECTOR_SIZE,
				       0, chunk) < 0)
			return -1;
	}
	return 0;
}

static void
virtio_mod_irq_handler(void *priv)
{
	struct virtio_state *st = (struct virtio_state *)priv;
	vio_read8(st, VIRTIO_PCI_ISR);
}

/* ================================================================
 * Module ops vtable
 * ================================================================ */

const struct block_driver_ops virtio_blk_module_ops = {
	.name			= "virtio_blk",
	.match			= virtio_match,
	.probe			= virtio_probe,
	.get_disks		= virtio_get_disks,
	.read_sectors		= virtio_mod_read_sectors,
	.write_sectors		= virtio_mod_write_sectors,
	.irq_handler		= virtio_mod_irq_handler,
	.read_sectors_phys	= NULL,
	.write_sectors_phys	= NULL,
	.write_batch		= NULL,
};
