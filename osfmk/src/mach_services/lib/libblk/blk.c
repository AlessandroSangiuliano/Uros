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
 * blk.c — Block I/O abstraction layer implementation
 *
 * Provides a driver-agnostic block I/O interface for filesystem
 * servers.  At blk_open() time, the driver is discovered via netname,
 * the device is opened, and extended capabilities (batch write,
 * phys-addressed DMA) are probed via weak symbols.
 *
 * All I/O methods transparently fall back to basic device_read/write
 * if extended capabilities are not available.
 */

#include <mach.h>
#include <mach/mach_port.h>
#include <device/device.h>
#include <device/device_types.h>
#include <sa_mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * netname_notify is provided by libnetname.  Declared weak so that
 * binaries which link libblk but never call blk_open() (e.g. bootstrap)
 * are not forced to link libnetname.
 */
typedef struct {
	mach_msg_header_t		head;
	mach_msg_body_t			body;
	mach_msg_port_descriptor_t	service;
	mach_msg_trailer_t		trailer;
} netname_notify_msg_t;

extern kern_return_t netname_notify(
	mach_port_t serv, const char *name,
	mach_port_t port) __attribute__((weak));

extern kern_return_t netname_look_up(
	mach_port_t serv, const char *host,
	const char *name, mach_port_t *port) __attribute__((weak));

#include "blk.h"

/* ================================================================
 * Extended driver capabilities (MIG stubs from ahci_batch.defs).
 *
 * Declared weak: resolve to NULL when the binary does not link the
 * corresponding MIG user stubs.  blk_open() checks these pointers
 * and records which capabilities are available.
 * ================================================================ */

extern kern_return_t device_write_batch(
	mach_port_t device,
	dev_mode_t mode,
	recnum_t *recnums, mach_msg_type_number_t recnumsCnt,
	unsigned int *sizes, mach_msg_type_number_t sizesCnt,
	io_buf_ptr_t data, mach_msg_type_number_t dataCnt,
	io_buf_len_t *bytes_written) __attribute__((weak));

extern kern_return_t device_read_phys(
	mach_port_t device,
	dev_mode_t mode, recnum_t recnum,
	io_buf_len_t bytes_wanted,
	unsigned int *phys_addrs, mach_msg_type_number_t phys_addrsCnt,
	io_buf_len_t *bytes_read) __attribute__((weak));

extern kern_return_t device_write_phys(
	mach_port_t device,
	dev_mode_t mode, recnum_t recnum,
	io_buf_len_t bytes_to_write,
	unsigned int *phys_addrs, mach_msg_type_number_t phys_addrsCnt,
	io_buf_len_t *bytes_written) __attribute__((weak));

extern kern_return_t device_read_overwrite(
	mach_port_t device,
	dev_mode_t mode, recnum_t recnum,
	io_buf_len_t bytes_wanted,
	vm_offset_t buffer,
	mach_msg_type_number_t *bytes_read) __attribute__((weak));

/* ================================================================
 * Block device structure
 * ================================================================ */

#define BLK_NAME_MAX	64

struct blk_dev {
	mach_port_t	bd_port;		/* driver service port */
	unsigned int	bd_rec_size;		/* sector size (bytes) */
	unsigned int	bd_capacity;		/* total sectors */
	char		bd_name[BLK_NAME_MAX];	/* driver name */

	/* Capability flags (set at open time) */
	int		bd_has_batch;
	int		bd_has_phys;
	int		bd_has_overwrite;
};

/* ================================================================
 * blk_open
 * ================================================================ */

struct blk_dev *
blk_open(mach_port_t name_server_port, const char *driver_name)
{
	struct blk_dev		*dev;
	mach_port_t		drv_port;
	mach_port_t		notify_recv;
	netname_notify_msg_t	nmsg;
	kern_return_t		kr;
	unsigned int		size_info[2];
	mach_msg_type_number_t	info_cnt;

	/* Discover the driver via netname.
	 * Use netname_notify to wait if the driver hasn't registered yet. */
	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &notify_recv);
	if (kr != KERN_SUCCESS) {
		printf("blk_open: port alloc failed: %d\n", kr);
		return NULL;
	}

	kr = netname_notify(name_server_port, (char *)driver_name,
			    notify_recv);
	if (kr != KERN_SUCCESS) {
		printf("blk_open: netname_notify(%s) failed: %d\n",
		       driver_name, kr);
		mach_port_destroy(mach_task_self(), notify_recv);
		return NULL;
	}

	kr = mach_msg(&nmsg.head, MACH_RCV_MSG,
		      0, sizeof(nmsg), notify_recv,
		      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (kr != KERN_SUCCESS) {
		printf("blk_open: notification recv failed: %d\n", kr);
		mach_port_destroy(mach_task_self(), notify_recv);
		return NULL;
	}

	drv_port = nmsg.service.name;
	mach_port_destroy(mach_task_self(), notify_recv);

	/* Query device size */
	info_cnt = 2;
	kr = device_get_status(drv_port, DEV_GET_SIZE,
			       (dev_status_t)size_info, &info_cnt);

	/* Allocate and fill the blk_dev structure */
	dev = (struct blk_dev *)malloc(sizeof(struct blk_dev));
	if (!dev) {
		printf("blk_open: malloc failed\n");
		return NULL;
	}

	dev->bd_port = drv_port;
	strncpy(dev->bd_name, driver_name, BLK_NAME_MAX - 1);
	dev->bd_name[BLK_NAME_MAX - 1] = '\0';

	if (kr == KERN_SUCCESS && info_cnt >= 2) {
		dev->bd_rec_size = size_info[1];
		dev->bd_capacity = size_info[0] / size_info[1];
	} else {
		/* Fallback: assume 512-byte sectors, unknown capacity */
		dev->bd_rec_size = 512;
		dev->bd_capacity = 0;
	}

	/* Probe capabilities */
	dev->bd_has_batch = (device_write_batch != NULL);
	dev->bd_has_phys = (device_read_phys != NULL &&
			    device_write_phys != NULL);
	dev->bd_has_overwrite = (device_read_overwrite != NULL);

	printf("blk: opened '%s' (port=%d, rec=%u, sectors=%u, "
	       "batch=%d, phys=%d, overwrite=%d)\n",
	       dev->bd_name, dev->bd_port, dev->bd_rec_size,
	       dev->bd_capacity, dev->bd_has_batch,
	       dev->bd_has_phys, dev->bd_has_overwrite);

	return dev;
}

/* ================================================================
 * blk_open_try — non-blocking variant of blk_open
 *
 * Uses netname_look_up (returns immediately) instead of
 * netname_notify (blocks until the name appears).
 * Returns NULL if the driver is not yet registered.
 * ================================================================ */

struct blk_dev *
blk_open_try(mach_port_t name_server_port, const char *driver_name)
{
	struct blk_dev		*dev;
	mach_port_t		drv_port;
	kern_return_t		kr;
	unsigned int		size_info[2];
	mach_msg_type_number_t	info_cnt;

	kr = netname_look_up(name_server_port, "",
			     (char *)driver_name, &drv_port);
	if (kr != KERN_SUCCESS)
		return NULL;

	/* Query device size */
	info_cnt = 2;
	kr = device_get_status(drv_port, DEV_GET_SIZE,
			       (dev_status_t)size_info, &info_cnt);

	dev = (struct blk_dev *)malloc(sizeof(struct blk_dev));
	if (!dev)
		return NULL;

	dev->bd_port = drv_port;
	strncpy(dev->bd_name, driver_name, BLK_NAME_MAX - 1);
	dev->bd_name[BLK_NAME_MAX - 1] = '\0';

	if (kr == KERN_SUCCESS && info_cnt >= 2) {
		dev->bd_rec_size = size_info[1];
		dev->bd_capacity = size_info[0] / size_info[1];
	} else {
		dev->bd_rec_size = 512;
		dev->bd_capacity = 0;
	}

	dev->bd_has_batch = (device_write_batch != NULL);
	dev->bd_has_phys = (device_read_phys != NULL &&
			    device_write_phys != NULL);
	dev->bd_has_overwrite = (device_read_overwrite != NULL);

	printf("blk: opened '%s' (port=%d, rec=%u, sectors=%u, "
	       "batch=%d, phys=%d, overwrite=%d)\n",
	       dev->bd_name, dev->bd_port, dev->bd_rec_size,
	       dev->bd_capacity, dev->bd_has_batch,
	       dev->bd_has_phys, dev->bd_has_overwrite);

	return dev;
}

/* ================================================================
 * blk_close
 * ================================================================ */

void
blk_close(struct blk_dev *dev)
{
	if (!dev)
		return;
	mach_port_deallocate(mach_task_self(), dev->bd_port);
	free(dev);
}

/* ================================================================
 * Basic I/O
 * ================================================================ */

kern_return_t
blk_read(struct blk_dev *dev,
	 recnum_t recnum,
	 io_buf_len_t bytes_wanted,
	 io_buf_ptr_t *data,
	 mach_msg_type_number_t *bytes_read)
{
	return device_read(dev->bd_port, 0, recnum,
			   (int)bytes_wanted,
			   data, bytes_read);
}

kern_return_t
blk_write(struct blk_dev *dev,
	  recnum_t recnum,
	  io_buf_ptr_t data,
	  mach_msg_type_number_t data_count,
	  io_buf_len_t *bytes_written)
{
	return device_write(dev->bd_port, 0, recnum,
			    data, data_count, (int *)bytes_written);
}

kern_return_t
blk_read_overwrite(struct blk_dev *dev,
		   recnum_t recnum,
		   io_buf_len_t bytes_wanted,
		   vm_offset_t buffer,
		   mach_msg_type_number_t *bytes_read)
{
	if (dev->bd_has_overwrite) {
		return device_read_overwrite(dev->bd_port, 0, recnum,
					     bytes_wanted, buffer,
					     bytes_read);
	}

	/* Fallback: regular read + memcpy */
	{
		io_buf_ptr_t tmp;
		mach_msg_type_number_t cnt;
		kern_return_t kr;

		kr = device_read(dev->bd_port, 0, recnum,
				 (int)bytes_wanted, &tmp, &cnt);
		if (kr != KERN_SUCCESS)
			return kr;

		memcpy((void *)buffer, (void *)tmp, cnt);
		vm_deallocate(mach_task_self(),
			      (vm_offset_t)tmp, cnt);
		*bytes_read = cnt;
		return KERN_SUCCESS;
	}
}

/* ================================================================
 * Phys-addressed I/O (zero-copy DMA)
 * ================================================================ */

kern_return_t
blk_read_phys(struct blk_dev *dev,
	      recnum_t recnum,
	      io_buf_len_t bytes_wanted,
	      unsigned int *phys_addrs,
	      unsigned int n_phys,
	      io_buf_len_t *bytes_read)
{
	if (dev->bd_has_phys) {
		return device_read_phys(dev->bd_port, 0, recnum,
					bytes_wanted,
					phys_addrs, n_phys,
					bytes_read);
	}

	/* Fallback: not possible without a destination VA.
	 * Caller must handle this case. */
	return KERN_INVALID_ARGUMENT;
}

kern_return_t
blk_write_phys(struct blk_dev *dev,
	       recnum_t recnum,
	       io_buf_len_t bytes_to_write,
	       unsigned int *phys_addrs,
	       unsigned int n_phys,
	       io_buf_len_t *bytes_written)
{
	if (dev->bd_has_phys) {
		return device_write_phys(dev->bd_port, 0, recnum,
					 bytes_to_write,
					 phys_addrs, n_phys,
					 bytes_written);
	}

	return KERN_INVALID_ARGUMENT;
}

/* ================================================================
 * Batch write
 * ================================================================ */

kern_return_t
blk_write_batch(struct blk_dev *dev,
		recnum_t *recnums,
		io_buf_ptr_t *data_bufs,
		mach_msg_type_number_t *data_sizes,
		unsigned int count,
		io_buf_len_t *total_written)
{
	if (dev->bd_has_batch && count > 1) {
		/* Build concatenated buffer and size array for the
		 * batch RPC.  The MIG stub expects:
		 *   recnums[]  — array of sector numbers
		 *   sizes[]    — array of per-block sizes (unsigned int)
		 *   data       — concatenated block data
		 */
		vm_size_t total_size = 0;
		unsigned int i;
		unsigned int sizes[count];
		vm_offset_t concat;
		vm_offset_t off;
		kern_return_t kr;

		for (i = 0; i < count; i++) {
			sizes[i] = data_sizes[i];
			total_size += data_sizes[i];
		}

		kr = vm_allocate(mach_task_self(), &concat,
				 total_size, TRUE);
		if (kr != KERN_SUCCESS)
			goto fallback;

		off = 0;
		for (i = 0; i < count; i++) {
			memcpy((void *)(concat + off),
			       (void *)data_bufs[i], data_sizes[i]);
			off += data_sizes[i];
		}

		kr = device_write_batch(dev->bd_port, 0,
					recnums, count,
					sizes, count,
					(io_buf_ptr_t)concat,
					(mach_msg_type_number_t)total_size,
					total_written);

		vm_deallocate(mach_task_self(), concat, total_size);
		return kr;
	}

fallback:
	/* Fallback: individual writes */
	{
		unsigned int i;
		io_buf_len_t written;
		io_buf_len_t total = 0;
		kern_return_t kr;

		for (i = 0; i < count; i++) {
			kr = device_write(dev->bd_port, 0, recnums[i],
					  data_bufs[i], data_sizes[i],
					  (int *)&written);
			if (kr != KERN_SUCCESS)
				return kr;
			total += written;
		}
		*total_written = total;
		return KERN_SUCCESS;
	}
}

/* ================================================================
 * Property accessors
 * ================================================================ */

mach_port_t
blk_port(struct blk_dev *dev)
{
	return dev->bd_port;
}

void
blk_set_port(struct blk_dev *dev, mach_port_t new_port)
{
	if (!dev || new_port == MACH_PORT_NULL || new_port == dev->bd_port)
		return;
	mach_port_deallocate(mach_task_self(), dev->bd_port);
	dev->bd_port = new_port;
}

unsigned int
blk_rec_size(struct blk_dev *dev)
{
	return dev->bd_rec_size;
}

unsigned int
blk_capacity_sectors(struct blk_dev *dev)
{
	return dev->bd_capacity;
}

const char *
blk_driver_name(struct blk_dev *dev)
{
	return dev->bd_name;
}

int
blk_has_phys(struct blk_dev *dev)
{
	return dev->bd_has_phys;
}

int
blk_has_batch(struct blk_dev *dev)
{
	return dev->bd_has_batch;
}
