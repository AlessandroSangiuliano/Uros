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
 * virtio_blk.c — Userspace virtio-blk driver for Uros
 *
 * Uses the virtio PCI legacy transport (BAR0 I/O ports) to
 * communicate with a QEMU virtio-blk-pci device.  Much faster
 * than emulated AHCI because the hypervisor handles requests
 * directly without simulating hardware registers.
 *
 * Boot flow:
 *   bootstrap → main() → pci_scan() → virtio_init()
 *   → virtqueue_setup() → read capacity → message loop
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
#include "virtio.h"

/* msgh_id base for IRQ notifications */
#define IRQ_NOTIFY_MSGH_BASE	3000

/* PCI configuration registers */
#define PCI_VENDOR_ID		0x00
#define PCI_COMMAND		0x04
#define PCI_CLASS_REV		0x08
#define PCI_BAR0		0x10
#define PCI_INTERRUPT_LINE	0x3C
#define PCI_CMD_IO_ENABLE	(1 << 0)
#define PCI_CMD_BUS_MASTER	(1 << 2)

#define SECTOR_SIZE		512u

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
static mach_port_t	vblk_service_port;
static mach_port_t	port_set;

/* PCI location */
static unsigned int	vblk_bus, vblk_slot, vblk_func;
static unsigned int	vblk_irq;
static unsigned int	vblk_iobase;	/* BAR0 I/O port base */

/* Disk info */
static uint32_t		disk_sectors;	/* low 32 bits of capacity */

/* ================================================================
 * Virtqueue state
 *
 * We use a single virtqueue (queue 0 = requestq).
 * Each I/O request uses a 3-descriptor chain:
 *   desc 0: virtio_blk_req_hdr  (device-readable)
 *   desc 1: data buffer          (device-readable or writable)
 *   desc 2: status byte           (device-writable)
 * ================================================================ */

static unsigned int	vq_size;	/* number of descriptors */

/* DMA buffers for the virtqueue (physically contiguous) */
static unsigned int	vq_kva, vq_uva, vq_pa;
static unsigned int	vq_alloc_size;

/* Pointers into the mapped virtqueue */
static struct vring_desc  *vq_desc;
static struct vring_avail *vq_avail;
static struct vring_used  *vq_used;

/* Request header + status DMA buffer (one page, reused per request) */
static unsigned int	req_kva, req_uva, req_pa;

/* Data DMA buffer */
static unsigned int	data_kva, data_uva, data_pa;
#define DATA_BUF_SIZE	(128u * 1024u)	/* 128 KB data buffer */

/* Free descriptor tracking */
static uint16_t		last_used_idx;

/* ================================================================
 * I/O port helpers — wrap device_master RPCs
 * ================================================================ */

static inline uint32_t
vio_read32(unsigned int off)
{
	unsigned int val;
	device_io_port_read(master_device, vblk_iobase + off, 4, &val);
	return val;
}

static inline uint16_t
vio_read16(unsigned int off)
{
	unsigned int val;
	device_io_port_read(master_device, vblk_iobase + off, 2, &val);
	return (uint16_t)val;
}

static inline uint8_t
vio_read8(unsigned int off)
{
	unsigned int val;
	device_io_port_read(master_device, vblk_iobase + off, 1, &val);
	return (uint8_t)val;
}

static inline void
vio_write32(unsigned int off, uint32_t val)
{
	device_io_port_write(master_device, vblk_iobase + off, 4, val);
}

static inline void
vio_write16(unsigned int off, uint16_t val)
{
	device_io_port_write(master_device, vblk_iobase + off, 2, val);
}

static inline void
vio_write8(unsigned int off, uint8_t val)
{
	device_io_port_write(master_device, vblk_iobase + off, 1, val);
}

/* ================================================================
 * RDTSC for benchmarking
 * ================================================================ */

static inline uint64_t
rdtsc(void)
{
	uint64_t tsc;
	__asm__ volatile("rdtsc" : "=A" (tsc));
	return tsc;
}

/* ================================================================
 * PCI scan — find virtio-blk device
 * ================================================================ */

static int
pci_scan_virtio(void)
{
	unsigned int bus, slot, func;
	unsigned int vendor_device;
	kern_return_t kr;

	printf("virtio: scanning PCI bus...\n");

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

				if ((vendor_device & 0xFFFF) == VIRTIO_PCI_VENDOR &&
				    (((vendor_device >> 16) & 0xFFFF) ==
				     VIRTIO_PCI_DEVICE_BLK_TRANS ||
				     ((vendor_device >> 16) & 0xFFFF) ==
				     VIRTIO_PCI_DEVICE_BLK_MODERN)) {
					vblk_bus  = bus;
					vblk_slot = slot;
					vblk_func = func;
					printf("virtio: found virtio-blk at "
					       "PCI %u:%u.%u  id=0x%08X\n",
					       bus, slot, func, vendor_device);
					return 1;
				}
			}
		}
	}

	printf("virtio: no virtio-blk device found\n");
	return 0;
}

/* ================================================================
 * Virtqueue setup
 * ================================================================ */

static int
virtqueue_setup(void)
{
	kern_return_t kr;
	unsigned int used_off;

	/* Select queue 0 (requestq) */
	vio_write16(VIRTIO_PCI_QUEUE_SEL, 0);

	/* Read queue size */
	vq_size = vio_read16(VIRTIO_PCI_QUEUE_SIZE);
	if (vq_size == 0) {
		printf("virtio: queue 0 size is 0\n");
		return -1;
	}
	printf("virtio: queue 0 size = %u\n", vq_size);

	/* Allocate virtqueue DMA memory */
	vq_alloc_size = VRING_TOTAL_SIZE(vq_size);
	/* Round up to page */
	vq_alloc_size = (vq_alloc_size + 4095u) & ~4095u;

	kr = device_dma_alloc(master_device, vq_alloc_size, &vq_kva, &vq_pa);
	if (kr != KERN_SUCCESS) {
		printf("virtio: vq alloc failed (%u bytes)\n", vq_alloc_size);
		return -1;
	}
	kr = device_dma_map_user(master_device, vq_kva, vq_alloc_size,
				 mach_task_self(), &vq_uva);
	if (kr != KERN_SUCCESS) {
		printf("virtio: vq map failed\n");
		return -1;
	}

	/* Zero the entire virtqueue */
	memset((void *)vq_uva, 0, vq_alloc_size);

	/* Set up pointers into the mapped memory */
	vq_desc  = (struct vring_desc *)vq_uva;
	vq_avail = (struct vring_avail *)(vq_uva + VRING_AVAIL_OFFSET(vq_size));
	used_off = VRING_USED_OFFSET(vq_size);
	vq_used  = (struct vring_used *)(vq_uva + used_off);

	printf("virtio: vq pa=0x%08X  desc=+0  avail=+0x%X  used=+0x%X\n",
	       vq_pa, VRING_AVAIL_OFFSET(vq_size), used_off);

	/* Tell device the queue physical page frame number */
	vio_write32(VIRTIO_PCI_QUEUE_PFN, vq_pa / VRING_ALIGN);

	/* Allocate request header + status DMA buffer (1 page) */
	kr = device_dma_alloc(master_device, 4096, &req_kva, &req_pa);
	if (kr != KERN_SUCCESS) {
		printf("virtio: req alloc failed\n");
		return -1;
	}
	kr = device_dma_map_user(master_device, req_kva, 4096,
				 mach_task_self(), &req_uva);
	if (kr != KERN_SUCCESS) {
		printf("virtio: req map failed\n");
		return -1;
	}

	/* Allocate data DMA buffer */
	kr = device_dma_alloc(master_device, DATA_BUF_SIZE, &data_kva, &data_pa);
	if (kr != KERN_SUCCESS) {
		printf("virtio: data alloc failed\n");
		return -1;
	}
	kr = device_dma_map_user(master_device, data_kva, DATA_BUF_SIZE,
				 mach_task_self(), &data_uva);
	if (kr != KERN_SUCCESS) {
		printf("virtio: data map failed\n");
		return -1;
	}

	last_used_idx = 0;
	return 0;
}

/* ================================================================
 * Submit a single virtio-blk request and poll for completion
 *
 * Uses descriptors 0-2 as a fixed 3-descriptor chain:
 *   0: request header  (device-readable)
 *   1: data buffer     (device-readable for write, writable for read)
 *   2: status byte     (device-writable)
 * ================================================================ */

static int
virtio_blk_request(uint32_t type, uint64_t sector,
		   unsigned int data_offset, unsigned int data_len)
{
	struct virtio_blk_req_hdr *hdr;
	uint8_t *status_ptr;
	int timeout;

	/* Set up request header in the req DMA buffer */
	hdr = (struct virtio_blk_req_hdr *)req_uva;
	hdr->type = type;
	hdr->reserved = 0;
	hdr->sector = sector;

	/* Status byte lives right after the header in the same page */
	status_ptr = (uint8_t *)(req_uva + sizeof(*hdr));
	*status_ptr = 0xFF;	/* sentinel */

	/* Descriptor 0: header (device reads) */
	vq_desc[0].addr  = (uint64_t)req_pa;
	vq_desc[0].len   = sizeof(struct virtio_blk_req_hdr);
	vq_desc[0].flags = VRING_DESC_F_NEXT;
	vq_desc[0].next  = 1;

	/* Descriptor 1: data buffer */
	vq_desc[1].addr  = (uint64_t)(data_pa + data_offset);
	vq_desc[1].len   = data_len;
	vq_desc[1].flags = VRING_DESC_F_NEXT;
	if (type == VIRTIO_BLK_T_IN)
		vq_desc[1].flags |= VRING_DESC_F_WRITE;	/* device writes */
	vq_desc[1].next  = 2;

	/* Descriptor 2: status byte (device writes) */
	vq_desc[2].addr  = (uint64_t)(req_pa + sizeof(*hdr));
	vq_desc[2].len   = 1;
	vq_desc[2].flags = VRING_DESC_F_WRITE;
	vq_desc[2].next  = 0;

	/* Add to available ring */
	vq_avail->ring[vq_avail->idx % vq_size] = 0;	/* chain head = desc 0 */

	/* Memory barrier before updating idx */
	__asm__ volatile("" ::: "memory");

	vq_avail->idx++;

	/* Notify device */
	vio_write16(VIRTIO_PCI_QUEUE_NOTIFY, 0);

	/*
	 * Poll for completion.  The hypervisor processes the request
	 * and updates the used ring.  Under KVM this typically
	 * completes within a few hundred iterations.
	 */
	for (timeout = 0; timeout < 10000000; timeout++) {
		__asm__ volatile("pause" ::: "memory");
		if (((volatile struct vring_used *)vq_used)->idx
		    != last_used_idx) {
			vio_read8(VIRTIO_PCI_ISR);	/* ack interrupt */
			last_used_idx = vq_used->idx;
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
 * Full virtio-blk initialisation
 * ================================================================ */

static int
virtio_init(void)
{
	kern_return_t kr;
	unsigned int bar0, cmd_reg, irq_reg;
	uint32_t host_features;
	uint32_t cap_lo, cap_hi;

	/* Read BAR0 (I/O port base) */
	kr = device_pci_config_read(master_device,
		vblk_bus, vblk_slot, vblk_func, PCI_BAR0, &bar0);
	if (kr != KERN_SUCCESS || !(bar0 & 1u)) {
		printf("virtio: BAR0 is not I/O space (0x%08X)\n", bar0);
		return -1;
	}
	vblk_iobase = bar0 & ~3u;
	printf("virtio: I/O base = 0x%04X\n", vblk_iobase);

	/* Enable PCI I/O space + bus master */
	kr = device_pci_config_read(master_device,
		vblk_bus, vblk_slot, vblk_func, PCI_COMMAND, &cmd_reg);
	if (kr == KERN_SUCCESS) {
		cmd_reg |= PCI_CMD_IO_ENABLE | PCI_CMD_BUS_MASTER;
		device_pci_config_write(master_device,
			vblk_bus, vblk_slot, vblk_func, PCI_COMMAND, cmd_reg);
	}

	/* Read IRQ */
	kr = device_pci_config_read(master_device,
		vblk_bus, vblk_slot, vblk_func, PCI_INTERRUPT_LINE, &irq_reg);
	if (kr == KERN_SUCCESS) {
		vblk_irq = irq_reg & 0xFF;
		printf("virtio: IRQ = %u\n", vblk_irq);
	}

	/*
	 * Legacy virtio device init sequence:
	 *   1. Reset (status = 0)
	 *   2. Acknowledge (status |= ACKNOWLEDGE)
	 *   3. Driver (status |= DRIVER)
	 *   4. Read features, negotiate
	 *   5. Setup virtqueues
	 *   6. Driver OK (status |= DRIVER_OK)
	 */

	/* 1. Reset */
	vio_write8(VIRTIO_PCI_STATUS, 0);

	/* 2. Acknowledge */
	vio_write8(VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

	/* 3. Driver */
	vio_write8(VIRTIO_PCI_STATUS,
		   VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

	/* 4. Feature negotiation — accept no optional features */
	host_features = vio_read32(VIRTIO_PCI_HOST_FEATURES);
	printf("virtio: host features = 0x%08X\n", host_features);
	vio_write32(VIRTIO_PCI_GUEST_FEATURES, 0);

	/* 5. Setup virtqueue */
	if (virtqueue_setup() < 0) {
		vio_write8(VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
		return -1;
	}

	/* Register IRQ */
	if (vblk_irq > 0 && vblk_irq < 16) {
		kr = device_intr_register(master_device, vblk_irq, irq_port,
					  MACH_MSG_TYPE_MAKE_SEND);
		if (kr == KERN_SUCCESS)
			printf("virtio: IRQ %u registered\n", vblk_irq);
	}

	/* 6. Driver OK */
	vio_write8(VIRTIO_PCI_STATUS,
		   VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
		   VIRTIO_STATUS_DRIVER_OK);

	/* Read disk capacity from device config (offset 0x14, 8 bytes) */
	cap_lo = vio_read32(VIRTIO_PCI_CONFIG + 0);
	cap_hi = vio_read32(VIRTIO_PCI_CONFIG + 4);
	disk_sectors = cap_lo;	/* 32-bit is enough for < 2 TB */

	printf("virtio: capacity = %u sectors (%u MB)\n",
	       disk_sectors, disk_sectors / 2048);
	printf("virtio: status = 0x%02X\n",
	       vio_read8(VIRTIO_PCI_STATUS));

	return 0;
}

/* ================================================================
 * Sequential read benchmark
 * ================================================================ */

#define BENCH_SECTORS		256u	/* 128 KB total */

static void
virtio_benchmark(void)
{
	uint64_t t0, t1;
	unsigned int bytes = BENCH_SECTORS * SECTOR_SIZE;
	unsigned int offset, chunk_sects, chunk_bytes;

	printf("virtio: benchmark: %u sectors (%u KB) sequential read\n",
	       BENCH_SECTORS, BENCH_SECTORS / 2);

	/* Warm up */
	if (virtio_blk_request(VIRTIO_BLK_T_IN, 0, 0, SECTOR_SIZE) < 0) {
		printf("virtio: benchmark warm-up failed\n");
		return;
	}

	t0 = rdtsc();

	offset = 0;
	while (offset < bytes) {
		chunk_bytes = bytes - offset;
		if (chunk_bytes > DATA_BUF_SIZE)
			chunk_bytes = DATA_BUF_SIZE;
		chunk_sects = chunk_bytes / SECTOR_SIZE;

		if (virtio_blk_request(VIRTIO_BLK_T_IN,
				       offset / SECTOR_SIZE,
				       0, chunk_bytes) < 0) {
			printf("virtio: benchmark read error\n");
			return;
		}
		offset += chunk_bytes;
	}

	t1 = rdtsc();

	{
		unsigned int cyc = (unsigned int)((t1 - t0) & 0xFFFFFFFFu);

		printf("virtio: %u sectors (%u KB) in %u TSC cycles\n",
		       BENCH_SECTORS, BENCH_SECTORS / 2, cyc);
		printf("virtio: cycles/sector : %u\n",
		       cyc / BENCH_SECTORS);
		if (cyc > 0)
			printf("virtio: ~%u MB/s  (assumes 1 GHz TSC)\n",
			       bytes * 1000u / cyc);
	}
}

/* ================================================================
 * Device server — MIG server stubs for device.defs
 * ================================================================ */

kern_return_t
ds_device_open(mach_port_t master, mach_port_t reply,
	       mach_msg_type_name_t reply_poly,
	       mach_port_t ledger, dev_mode_t mode,
	       security_token_t sec_token, dev_name_t name,
	       mach_port_t *device)
{
	*device = vblk_service_port;
	return KERN_SUCCESS;
}

kern_return_t
ds_device_close(mach_port_t device)
{
	return KERN_SUCCESS;
}

/*
 * ds_device_open_cap stub: see ahci_driver comment — this standalone
 * binary is not a capability-gated entry point in v1.
 */
kern_return_t
ds_device_open_cap(mach_port_t master, mach_port_t reply,
		   mach_msg_type_name_t reply_poly,
		   mach_port_t ledger, dev_mode_t mode,
		   security_token_t sec_token, dev_name_t name,
		   char *token, mach_msg_type_number_t token_len,
		   mach_port_t *device)
{
	*device = MACH_PORT_NULL;
	return D_INVALID_OPERATION;
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
	unsigned int total, offset, chunk, lba;

	if (bytes_wanted <= 0)
		return D_INVALID_SIZE;

	total = (unsigned int)bytes_wanted;
	if (total % SECTOR_SIZE)
		total = (total + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);

	kr = vm_allocate(mach_task_self(), &buf, total, TRUE);
	if (kr != KERN_SUCCESS)
		return D_NO_MEMORY;

	lba = recnum;
	for (offset = 0; offset < total; offset += chunk) {
		chunk = total - offset;
		if (chunk > DATA_BUF_SIZE)
			chunk = DATA_BUF_SIZE;

		if (virtio_blk_request(VIRTIO_BLK_T_IN,
				       lba + offset / SECTOR_SIZE,
				       0, chunk) < 0) {
			vm_deallocate(mach_task_self(), buf, total);
			return D_IO_ERROR;
		}
		memcpy((void *)(buf + offset), (void *)data_uva, chunk);
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

	if (data_count <= 0)
		return D_INVALID_SIZE;

	total = (unsigned int)data_count;
	if (total % SECTOR_SIZE)
		total = (total + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);

	lba = recnum;
	for (offset = 0; offset < total; offset += chunk) {
		chunk = total - offset;
		if (chunk > DATA_BUF_SIZE)
			chunk = DATA_BUF_SIZE;

		memcpy((void *)data_uva,
		       (void *)((vm_offset_t)data + offset), chunk);
		if (virtio_blk_request(VIRTIO_BLK_T_OUT,
				       lba + offset / SECTOR_SIZE,
				       0, chunk) < 0) {
			vm_deallocate(mach_task_self(),
				      (vm_offset_t)data, data_count);
			return D_IO_ERROR;
		}
		lba += chunk / SECTOR_SIZE;
	}

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
 * Combined demux
 * ================================================================ */

static boolean_t
virtio_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
	if (device_server(in, out))
		return TRUE;

	/* IRQ notification */
	if (in->msgh_id >= IRQ_NOTIFY_MSGH_BASE) {
		/* Read ISR to acknowledge (clears interrupt) */
		vio_read8(VIRTIO_PCI_ISR);
		((mig_reply_error_t *)out)->RetCode = MIG_NO_REPLY;
		((mig_reply_error_t *)out)->Head.msgh_size =
			sizeof(mig_reply_error_t);
		return TRUE;
	}

	return FALSE;
}

/* ================================================================
 * Main
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

	printf("\n=== Virtio-blk userspace driver ===\n");

	/* IRQ notification port */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &irq_port);
	if (kr != KERN_SUCCESS)
		return 1;
	kr = mach_port_insert_right(mach_task_self(), irq_port, irq_port,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS)
		return 1;

	/* Service port */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &vblk_service_port);
	if (kr != KERN_SUCCESS)
		return 1;
	kr = mach_port_insert_right(mach_task_self(),
		vblk_service_port, vblk_service_port,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS)
		return 1;

	/* Port set */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_PORT_SET, &port_set);
	if (kr != KERN_SUCCESS)
		return 1;
	mach_port_move_member(mach_task_self(), irq_port, port_set);
	mach_port_move_member(mach_task_self(), vblk_service_port, port_set);

	/* Find and initialise */
	if (!pci_scan_virtio()) {
		printf("virtio: no device, exiting\n");
		return 1;
	}
	if (virtio_init() < 0) {
		printf("virtio: init failed\n");
		return 1;
	}

	/* Benchmark */
	virtio_benchmark();

	/* Register with name server */
	kr = netname_check_in(name_server_port, "virtio_blk",
			      mach_task_self(), vblk_service_port);
	if (kr != KERN_SUCCESS)
		printf("virtio: netname_check_in failed (kr=%d)\n", kr);
	else
		printf("virtio: registered as 'virtio_blk'\n");

	printf("virtio: entering message loop\n");

	mach_msg_server(virtio_demux, 8192, port_set,
			MACH_MSG_OPTION_NONE);

	printf("virtio: mach_msg_server exited unexpectedly\n");
	return 1;
}
