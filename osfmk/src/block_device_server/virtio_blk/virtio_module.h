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
 * virtio_module.h — Virtio-blk module private state
 */

#ifndef _VIRTIO_MODULE_H_
#define _VIRTIO_MODULE_H_

#include <stdint.h>
#include <mach.h>
#include "virtio.h"

#define DATA_BUF_SIZE	(128u * 1024u)

struct virtio_state {
	mach_port_t	master_device;
	unsigned int	pci_bus, pci_slot, pci_func;
	unsigned int	irq;
	unsigned int	iobase;		/* BAR0 I/O port base */

	uint32_t	disk_sectors;

	/* Virtqueue */
	unsigned int	vq_size;
	unsigned int	vq_kva, vq_uva, vq_pa;
	unsigned int	vq_alloc_size;

	struct vring_desc  *vq_desc;
	struct vring_avail *vq_avail;
	struct vring_used  *vq_used;

	/* Request header + status DMA buffer */
	unsigned int	req_kva, req_uva, req_pa;

	/* Data DMA buffer */
	unsigned int	data_kva, data_uva, data_pa;

	uint16_t	last_used_idx;
};

#endif /* _VIRTIO_MODULE_H_ */
