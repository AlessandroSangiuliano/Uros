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
 * blk.h — Block I/O abstraction layer
 *
 * Decouples filesystem servers from specific block drivers (AHCI,
 * virtio-blk, NVMe, etc.).  The filesystem calls blk_read/blk_write
 * and libblk dispatches to the appropriate driver via MIG RPC.
 *
 * Driver capabilities (batch write, phys-addressed DMA) are probed
 * at open time and used automatically when available.
 */

#ifndef LIBBLK_BLK_H
#define LIBBLK_BLK_H

#include <mach/port.h>
#include <mach/kern_return.h>
#include <device/device_types.h>

/*
 * Opaque block device handle.
 */
struct blk_dev;

/*
 * blk_open — Discover a block driver by name and open the device.
 *
 * Looks up `driver_name` via netname, opens the device, queries
 * sector size and capacity, and probes for extended capabilities
 * (batch write, phys-addressed I/O).
 *
 * Returns NULL on failure.
 */
struct blk_dev *blk_open(mach_port_t name_server_port,
			 const char *driver_name);

/*
 * blk_close — Release all resources associated with a block device.
 */
void blk_close(struct blk_dev *dev);

/*
 * blk_read — Read sectors from the device.
 *
 * Reads `bytes_wanted` bytes starting at sector `recnum`.
 * On success, `*data` points to the received buffer and
 * `*bytes_read` contains the actual byte count.
 *
 * The caller must vm_deallocate the returned buffer.
 */
kern_return_t blk_read(struct blk_dev *dev,
		       recnum_t recnum,
		       io_buf_len_t bytes_wanted,
		       io_buf_ptr_t *data,
		       mach_msg_type_number_t *bytes_read);

/*
 * blk_write — Write data to the device.
 *
 * Writes `data_count` bytes starting at sector `recnum`.
 * Returns the number of bytes actually written in `*bytes_written`.
 */
kern_return_t blk_write(struct blk_dev *dev,
			recnum_t recnum,
			io_buf_ptr_t data,
			mach_msg_type_number_t data_count,
			io_buf_len_t *bytes_written);

/*
 * blk_read_phys — Zero-copy DMA read into caller-supplied physical pages.
 *
 * If the driver supports phys-addressed I/O, DMA goes directly into
 * the pages described by `phys_addrs`.  Otherwise falls back to
 * blk_read + memcpy.
 */
kern_return_t blk_read_phys(struct blk_dev *dev,
			    recnum_t recnum,
			    io_buf_len_t bytes_wanted,
			    unsigned int *phys_addrs,
			    unsigned int n_phys,
			    io_buf_len_t *bytes_read);

/*
 * blk_write_phys — Zero-copy DMA write from caller-supplied physical pages.
 *
 * If the driver supports phys-addressed I/O, DMA reads directly from
 * the pages described by `phys_addrs`.  Otherwise falls back to
 * blk_write with data copy.
 */
kern_return_t blk_write_phys(struct blk_dev *dev,
			     recnum_t recnum,
			     io_buf_len_t bytes_to_write,
			     unsigned int *phys_addrs,
			     unsigned int n_phys,
			     io_buf_len_t *bytes_written);

/*
 * blk_write_batch — Write multiple non-contiguous blocks in one RPC.
 *
 * If the driver supports batch writes, sends all blocks in a single
 * message.  Otherwise falls back to individual blk_write calls.
 *
 * `recnums` and `data_bufs` are parallel arrays of `count` entries.
 */
kern_return_t blk_write_batch(struct blk_dev *dev,
			      recnum_t *recnums,
			      io_buf_ptr_t *data_bufs,
			      mach_msg_type_number_t *data_sizes,
			      unsigned int count,
			      io_buf_len_t *total_written);

/*
 * blk_read_overwrite — Read directly into a caller-provided buffer.
 *
 * Avoids OOL allocation; the kernel writes directly into `buffer`.
 */
kern_return_t blk_read_overwrite(struct blk_dev *dev,
				 recnum_t recnum,
				 io_buf_len_t bytes_wanted,
				 vm_offset_t buffer,
				 mach_msg_type_number_t *bytes_read);

/*
 * Device properties.
 */
mach_port_t	blk_port(struct blk_dev *dev);
unsigned int	blk_rec_size(struct blk_dev *dev);
unsigned int	blk_capacity_sectors(struct blk_dev *dev);
const char     *blk_driver_name(struct blk_dev *dev);
int		blk_has_phys(struct blk_dev *dev);
int		blk_has_batch(struct blk_dev *dev);

#endif /* LIBBLK_BLK_H */
