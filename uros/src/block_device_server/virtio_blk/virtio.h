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
 * virtio.h — Virtio PCI legacy transport and virtio-blk definitions
 *
 * Reference: Virtual I/O Device (VIRTIO) Version 1.2, Section 2 & 5.2
 *            Legacy interface: Virtio 0.9.5
 */

#ifndef _VIRTIO_H_
#define _VIRTIO_H_

#include <stdint.h>

/* ================================================================
 * PCI identification
 * ================================================================ */

#define VIRTIO_PCI_VENDOR		0x1AF4
#define VIRTIO_PCI_DEVICE_BLK_TRANS	0x1001	/* transitional block device */
#define VIRTIO_PCI_DEVICE_BLK_MODERN	0x1042	/* modern block device */

/* ================================================================
 * Legacy PCI I/O port registers (BAR0)
 *
 * Offsets from the BAR0 I/O port base.
 * ================================================================ */

#define VIRTIO_PCI_HOST_FEATURES	0x00	/* 32-bit, device features */
#define VIRTIO_PCI_GUEST_FEATURES	0x04	/* 32-bit, driver features */
#define VIRTIO_PCI_QUEUE_PFN		0x08	/* 32-bit, queue phys page */
#define VIRTIO_PCI_QUEUE_SIZE		0x0C	/* 16-bit, queue size */
#define VIRTIO_PCI_QUEUE_SEL		0x0E	/* 16-bit, queue select */
#define VIRTIO_PCI_QUEUE_NOTIFY		0x10	/* 16-bit, queue notify */
#define VIRTIO_PCI_STATUS		0x12	/* 8-bit, device status */
#define VIRTIO_PCI_ISR			0x13	/* 8-bit, ISR status */

/* Device-specific config starts at offset 0x14 (legacy, no MSI-X) */
#define VIRTIO_PCI_CONFIG		0x14

/* ================================================================
 * Device status bits
 * ================================================================ */

#define VIRTIO_STATUS_ACKNOWLEDGE	(1u << 0)
#define VIRTIO_STATUS_DRIVER		(1u << 1)
#define VIRTIO_STATUS_DRIVER_OK		(1u << 2)
#define VIRTIO_STATUS_FEATURES_OK	(1u << 3)	/* modern only */
#define VIRTIO_STATUS_FAILED		(1u << 7)

/* ================================================================
 * Virtqueue: split virtqueue layout
 *
 * A split virtqueue consists of three contiguous areas:
 *   1. Descriptor table  (16 bytes per entry)
 *   2. Available ring    (2 + 2 + 2*qsz + 2 bytes)
 *   3. Used ring         (2 + 2 + 8*qsz + 2 bytes, page-aligned)
 * ================================================================ */

/* Descriptor flags */
#define VRING_DESC_F_NEXT	(1u << 0)	/* next field is valid */
#define VRING_DESC_F_WRITE	(1u << 1)	/* device writes (vs reads) */

struct vring_desc {
	uint64_t	addr;		/* physical address */
	uint32_t	len;		/* length in bytes */
	uint16_t	flags;
	uint16_t	next;		/* next descriptor index */
} __attribute__((packed));

struct vring_avail {
	uint16_t	flags;
	uint16_t	idx;
	uint16_t	ring[];		/* variable-length */
} __attribute__((packed));

struct vring_used_elem {
	uint32_t	id;		/* descriptor chain head */
	uint32_t	len;		/* bytes written by device */
} __attribute__((packed));

struct vring_used {
	uint16_t	flags;
	uint16_t	idx;
	struct vring_used_elem	ring[];
} __attribute__((packed));

/*
 * Calculate the byte offsets of avail and used rings for a given
 * queue size, following the legacy virtio alignment rules.
 *
 * Descriptor table:  offset 0, size = qsz * 16
 * Available ring:    offset = qsz * 16,   size = 6 + 2 * qsz
 * Used ring:         next page boundary after avail ring end
 */
#define VRING_DESC_SIZE(qsz)	((qsz) * 16u)
#define VRING_AVAIL_SIZE(qsz)	(6u + 2u * (qsz))
#define VRING_USED_SIZE(qsz)	(6u + 8u * (qsz))

/* Align to page (4096) for used ring */
#define VRING_ALIGN		4096u
#define VRING_AVAIL_OFFSET(qsz)	VRING_DESC_SIZE(qsz)
#define VRING_USED_OFFSET(qsz)	\
	(((VRING_AVAIL_OFFSET(qsz) + VRING_AVAIL_SIZE(qsz)) + \
	  VRING_ALIGN - 1) & ~(VRING_ALIGN - 1))

/* Total virtqueue allocation size */
#define VRING_TOTAL_SIZE(qsz)	\
	(VRING_USED_OFFSET(qsz) + VRING_USED_SIZE(qsz))

/* ================================================================
 * Virtio-blk device configuration (at VIRTIO_PCI_CONFIG offset)
 *
 * Legacy layout: capacity is at config offset 0 (8 bytes).
 * ================================================================ */

struct virtio_blk_config {
	uint64_t	capacity;	/* disk size in 512-byte sectors */
	uint32_t	size_max;	/* max segment size */
	uint32_t	seg_max;	/* max segments per request */
	/* ... more fields we don't use */
} __attribute__((packed));

/* ================================================================
 * Virtio-blk request header
 * ================================================================ */

#define VIRTIO_BLK_T_IN		0	/* read */
#define VIRTIO_BLK_T_OUT	1	/* write */
#define VIRTIO_BLK_T_FLUSH	4	/* flush */

struct virtio_blk_req_hdr {
	uint32_t	type;		/* VIRTIO_BLK_T_IN/OUT/FLUSH */
	uint32_t	reserved;
	uint64_t	sector;		/* start sector (512-byte) */
} __attribute__((packed));

/* Status byte values (written by device at end of request) */
#define VIRTIO_BLK_S_OK		0
#define VIRTIO_BLK_S_IOERR	1
#define VIRTIO_BLK_S_UNSUPP	2

/* ================================================================
 * Feature bits
 * ================================================================ */

#define VIRTIO_BLK_F_SIZE_MAX	(1u << 1)
#define VIRTIO_BLK_F_SEG_MAX	(1u << 2)
#define VIRTIO_BLK_F_GEOMETRY	(1u << 4)
#define VIRTIO_BLK_F_RO		(1u << 5)
#define VIRTIO_BLK_F_BLK_SIZE	(1u << 6)
#define VIRTIO_BLK_F_FLUSH	(1u << 9)

#endif /* _VIRTIO_H_ */
