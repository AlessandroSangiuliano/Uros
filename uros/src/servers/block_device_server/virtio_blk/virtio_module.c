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
#include <device/pci.h>
#include "device_master.h"

/* PCI configuration registers */
/* The configuration header comes from <device/pci.h> (#427). */

/* Static state — one virtio-blk controller */
/*
 * 🔴 ONE STATE PER CONTROLLER, and it used to be ONE STATE.
 *
 * `probe' hands the framework a pointer through *priv, and the framework
 * keeps it in ctrl->priv for the life of that controller.  A single static
 * instance meant every controller was handed the SAME pointer, so the second
 * probe overwrote the first controller's ABAR, its port table, its DMA
 * addresses and its disk geometry -- while the first controller's partitions
 * were already registered and pointing at it.
 *
 * ⚠️ Invisible on i386 and on the default board, where there is exactly one
 * virtio controller and a singleton is indistinguishable from a correct
 * allocation.  q35 is what exposed it: that chipset has its own AHCI at
 * 0:31.2 besides the one the command line adds, so a boot there probes twice.
 *
 * 🔑 And the way it announced itself was not a crash.  The second probe found
 * no drive, failed, and left the shared state describing a port with nothing
 * on it -- so the FIRST controller's next read timed out with SSTS=0 and the
 * command engine stopped, on a port that had reported a disk and a running
 * engine minutes earlier.  The register dump is what named it: hardware that
 * says "no device" about a device it had just identified is not hardware
 * that changed its mind.
 *
 * The slot is taken but not committed until probe succeeds, so a controller
 * that fails to come up does not consume one.  Probes are sequential.
 */
static struct virtio_state virtio_st[MAX_CONTROLLERS];
static unsigned virtio_n_states;


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
 * Where the device configuration begins
 * ================================================================ */

/*
 * 🔴 ASKED OF THE DEVICE, not assumed (#520).
 *
 * The legacy virtio-pci header grows two 16-bit MSI-X vector fields at offset
 * 0x14 when MSI-X is enabled on the function, pushing the device-specific
 * configuration from 0x14 to 0x18.  This driver assumed 0x14, which was right
 * on i386 -- where there is no MSI-X to enable -- and wrong here.
 *
 * ⚠️ And "enabled" is not "present".  Every virtio device QEMU builds OFFERS
 * MSI-X; what moves the registers is the enable bit in the capability's
 * control word, which some other task may have set.  It was irq_claim_test:
 * it walks every device on bus 0 offering each one a vector, ten tasks before
 * this driver starts.  So this reads the bit rather than reasoning about who
 * might have set it.
 */
static unsigned int
virtio_config_offset(struct virtio_state *st)
{
	unsigned int	cap, hdr, reg;

	/* Capabilities are optional; without the list there is no MSI-X. */
	if (device_pci_config_read(st->master_device, st->pci_bus,
				   st->pci_slot, st->pci_func,
				   PCI_COMMAND, &reg) != KERN_SUCCESS
	    || !(reg & (PCI_STATUS_CAP_LIST << 16)))
		return VIRTIO_PCI_CONFIG;

	if (device_pci_config_read(st->master_device, st->pci_bus,
				   st->pci_slot, st->pci_func,
				   PCI_CAP_POINTER, &reg) != KERN_SUCCESS)
		return VIRTIO_PCI_CONFIG;

	cap = reg & 0xFC;

	/*
	 * ⚠️ Bounded, and not by trusting the list to end.  A device whose
	 * next-pointer loops would spin here forever; forty-eight is more
	 * capabilities than a function can hold in the 192 bytes available to
	 * them, so reaching it means the list is malformed.
	 */
	for (hdr = 0; cap >= 0x40 && hdr < 48; hdr++) {
		if (device_pci_config_read(st->master_device, st->pci_bus,
					   st->pci_slot, st->pci_func,
					   cap, &reg) != KERN_SUCCESS)
			break;

		if ((reg & 0xFF) == PCI_CAP_ID_MSIX)
			return (reg & (PCI_MSIX_CTL_ENABLE << 16))
				? VIRTIO_PCI_CONFIG_MSIX
				: VIRTIO_PCI_CONFIG;

		cap = (reg >> 8) & 0xFC;
	}

	return VIRTIO_PCI_CONFIG;
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
	     const struct pci_bar_region *bars, unsigned int n_bars,
	     void **priv)
{
	struct virtio_state *st;

	if (virtio_n_states >= MAX_CONTROLLERS) {
		printf("virtio: %u controllers already, and the framework holds "
		       "no more — refusing to probe %u:%u.%u\n",
		       virtio_n_states, bus, slot, func);
		return -1;
	}
	st = &virtio_st[virtio_n_states];
	memset(st, 0, sizeof(*st));
	kern_return_t kr;
	const struct pci_bar_region *io_region;
	unsigned int cmd_reg, irq_reg;
	int i;
	uint32_t host_features, cap_lo, cap_hi;

	st->pci_bus  = bus;
	st->pci_slot = slot;
	st->pci_func = func;
	st->master_device = master_dev;

	/*
	 * The I/O region, from what the HAL decoded (#427).
	 *
	 * 🔑 The check that mattered here is kept, not dropped: legacy virtio
	 * lives in I/O space, and a region that is not I/O is not this
	 * device's.  It used to be `bar0 & 1' on the raw value -- which was
	 * right, and is the reason the ~3 mask below it was right too, since
	 * an I/O BAR keeps two address bits a memory BAR does not.  The decode
	 * now answers the same question by flag, and applied the correct mask
	 * when it did.
	 */
	io_region = NULL;
	for (i = 0; i < (int)n_bars; i++)
		if (bars[i].slot == 0 && (bars[i].flags & PCI_REGION_IO))
			io_region = &bars[i];

	if (io_region == NULL) {
		printf("virtio: BAR slot 0 is not I/O space among %u "
		       "region(s)\n", n_bars);
		return -1;
	}
	st->iobase = (unsigned int)io_region->base;
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

	/*
	 * 🔴 WHERE the device config is, ASKED rather than assumed (#520).  See
	 * virtio_config_offset(): a legacy virtio-pci device puts it at 20 when
	 * MSI-X is disabled and at 24 when it is enabled, and this driver read
	 * 20 unconditionally.
	 */
	st->config_off = virtio_config_offset(st);

	/*
	 * ⚠️ Both halves.  Capacity is 64 bits of 512-byte sectors and only the
	 * low one used to be read -- which is a 2 TiB ceiling that nothing here
	 * would have reported, since the number that came back was a perfectly
	 * plausible size.
	 */
	cap_lo = vio_read32(st, st->config_off + 0);
	cap_hi = vio_read32(st, st->config_off + 4);
	if (cap_hi != 0) {
		printf("virtio: capacity %u:%08X sectors exceeds what this "
		       "driver addresses — refusing the disk\n",
		       cap_hi, cap_lo);
		return -1;
	}
	st->disk_sectors = cap_lo;

	printf("virtio: config at +0x%02X (msi-x %s), capacity = %u sectors "
	       "(%u MB)\n", st->config_off,
	       st->config_off == VIRTIO_PCI_CONFIG_MSIX ? "on" : "off",
	       st->disk_sectors, st->disk_sectors / 2048);
	printf("virtio: status = 0x%02X\n",
	       vio_read8(st, VIRTIO_PCI_STATUS));

	virtio_n_states++;		/* committed: this controller came up */

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
	(void)device_intr_enable(st->master_device, st->irq);
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
