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
 * block_device.c — MIG device server stubs and readahead cache
 *
 * Implements the ds_device_* functions that the MIG-generated
 * device_server() demux calls.  Protected payload on each partition
 * port provides the struct blk_partition pointer.  All I/O is
 * dispatched through the controller's ops vtable.
 */

#include <mach.h>
#include <mach/mach_port.h>
#include <mach/cap_types.h>
#include <device/device.h>
#include <device/device_types.h>
#include <stdio.h>
#include <string.h>
#include "block_server.h"

/*
 * UrMach capability verify trap (slot 37).  Declared here to keep BDS
 * from having to pull in the full libcap public header — we only need
 * the fast-path syscall, not cap_server RPC.
 */
extern kern_return_t urmach_cap_verify(const struct uros_cap *token,
                                       uint32_t op,
                                       uint64_t resource_id);

/* ================================================================
 * Readahead cache
 *
 * When a sequential read pattern is detected (current LBA ==
 * previous end LBA), we read extra sectors and cache them.
 * The next ds_device_read for the continuation is served from
 * the cache without hitting the disk.
 * ================================================================ */

static struct {
	uint32_t		lba_start;
	uint32_t		lba_count;
	vm_offset_t		buf;
	unsigned int		buf_size;
	struct blk_partition	*part;
} ra_cache;

static void
ra_invalidate(struct blk_partition *part)
{
	if (ra_cache.buf != 0 && ra_cache.part == part) {
		vm_deallocate(mach_task_self(),
			      ra_cache.buf, ra_cache.buf_size);
		ra_cache.buf = 0;
		ra_cache.lba_count = 0;
	}
}

/* ================================================================
 * ds_device_open / close
 * ================================================================ */

kern_return_t
ds_device_open(mach_port_t master, mach_port_t reply,
	       mach_msg_type_name_t reply_poly,
	       mach_port_t ledger, dev_mode_t mode,
	       security_token_t sec_token, dev_name_t name,
	       mach_port_t *device)
{
	struct blk_partition *part = (struct blk_partition *)master;
	if (part && part->recv_port != MACH_PORT_NULL)
		*device = part->recv_port;
	else
		*device = MACH_PORT_NULL;
	return KERN_SUCCESS;
}

kern_return_t
ds_device_close(mach_port_t device)
{
	return KERN_SUCCESS;
}

/* ================================================================
 * ds_device_open_cap — capability-gated open (Uros Issue C)
 *
 * Verifies the supplied uros_cap token against the partition name via
 * urmach_cap_verify.  On success the partition is marked authenticated
 * and the same partition port is returned, so subsequent device_read /
 * device_write on that port are accepted.  On failure returns
 * KERN_NO_ACCESS and the device port is left MACH_PORT_NULL.
 * ================================================================ */

kern_return_t
ds_device_open_cap(mach_port_t master, mach_port_t reply,
		   mach_msg_type_name_t reply_poly,
		   mach_port_t ledger, dev_mode_t mode,
		   security_token_t sec_token, dev_name_t name,
		   char *token_blob, mach_msg_type_number_t token_len,
		   mach_port_t *device)
{
	struct blk_partition *part = (struct blk_partition *)master;
	if (!part || part->recv_port == MACH_PORT_NULL) {
		*device = MACH_PORT_NULL;
		return D_NO_SUCH_DEVICE;
	}

	if (token_len != sizeof(struct uros_cap)) {
		*device = MACH_PORT_NULL;
		return KERN_NO_ACCESS;
	}

	struct uros_cap token;
	memcpy(&token, token_blob, sizeof(token));

	uint64_t resource_id = cap_name_hash(part->name);
	uint32_t op = CAP_OP_BLK_READ | CAP_OP_BLK_WRITE;

	kern_return_t kr = urmach_cap_verify(&token, op, resource_id);
	if (kr != KERN_SUCCESS) {
		printf("blk: cap verify FAIL for %s (op=0x%x id=0x%llx): kr=%d\n",
		       part->name, op, (unsigned long long)resource_id, kr);
		*device = MACH_PORT_NULL;
		return KERN_NO_ACCESS;
	}

	part->cap_authenticated = 1;
	printf("blk: validated cap %llu op=0x%x for %s\n",
	       (unsigned long long)token.cap_id, op, part->name);

	*device = part->recv_port;
	return KERN_SUCCESS;
}

/* ================================================================
 * ds_device_read — main read path with readahead cache
 * ================================================================ */

kern_return_t
ds_device_read(mach_port_t device, mach_port_t reply,
	       mach_msg_type_name_t reply_poly,
	       dev_mode_t mode, recnum_t recnum,
	       io_buf_len_t bytes_wanted,
	       io_buf_ptr_t *data, mach_msg_type_number_t *data_count)
{
	struct blk_partition *part = (struct blk_partition *)device;
	struct blk_controller *ctrl = part->ctrl;
	kern_return_t kr;
	vm_offset_t buf, read_buf;
	unsigned int total, nsectors, lba;
	unsigned int read_buf_size;
	unsigned int max_xfer;

	if (!part->cap_authenticated)
		return KERN_NO_ACCESS;

	if (bytes_wanted <= 0)
		return D_INVALID_SIZE;

	total = (unsigned int)bytes_wanted;
	if (total % SECTOR_SIZE)
		total = (total + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
	nsectors = total / SECTOR_SIZE;

	if (recnum + nsectors > part->num_sectors)
		return D_INVALID_SIZE;

	lba = part->start_lba + recnum;

	kr = vm_allocate(mach_task_self(), &buf, total, TRUE);
	if (kr != KERN_SUCCESS)
		return D_NO_MEMORY;

	/* Check readahead cache */
	if (ra_cache.buf != 0 &&
	    ra_cache.part == part &&
	    lba >= ra_cache.lba_start &&
	    lba + nsectors <= ra_cache.lba_start + ra_cache.lba_count) {
		unsigned int cache_off =
			(lba - ra_cache.lba_start) * SECTOR_SIZE;
		memcpy((void *)buf,
		       (void *)(ra_cache.buf + cache_off), total);
		*data = (io_buf_ptr_t)buf;
		*data_count = (mach_msg_type_number_t)bytes_wanted;
		return KERN_SUCCESS;
	}

	/*
	 * Cache miss — read from disk.
	 * Determine readahead: if sequential and small, prefetch extra.
	 */
	max_xfer = ctrl->disks[part->disk_index].max_transfer_bytes;
	if (max_xfer == 0)
		max_xfer = 128u * 1024u;

	{
		unsigned int ra_max_sectors = max_xfer / SECTOR_SIZE;
		unsigned int read_sects = nsectors;
		unsigned int ra_extra = 0;
		uint32_t part_end = part->start_lba + part->num_sectors;
		unsigned int offset, chunk;

		/* Sequential detection: prefetch up to ra_max_sectors */
		if (ra_cache.buf != 0 &&
		    ra_cache.part == part &&
		    lba == ra_cache.lba_start + ra_cache.lba_count &&
		    nsectors <= ra_max_sectors / 2 &&
		    lba + ra_max_sectors <= part_end) {
			read_sects = ra_max_sectors;
			ra_extra = read_sects - nsectors;
		}

		unsigned int read_bytes = read_sects * SECTOR_SIZE;
		unsigned int ra_buf_needed = ra_extra * SECTOR_SIZE;
		vm_offset_t ra_buf = 0;

		if (ra_extra > 0) {
			kr = vm_allocate(mach_task_self(), &ra_buf,
					 ra_buf_needed, TRUE);
			if (kr != KERN_SUCCESS) {
				read_sects = nsectors;
				read_bytes = total;
				ra_extra = 0;
			}
		}

		/* Read in chunks of max_xfer */
		for (offset = 0; offset < read_bytes; offset += chunk) {
			unsigned int batch_sects;

			chunk = read_bytes - offset;
			if (chunk > max_xfer)
				chunk = max_xfer;
			batch_sects = chunk / SECTOR_SIZE;

			if (ctrl->ops->read_sectors(ctrl->priv,
						    part->disk_index,
						    lba + offset / SECTOR_SIZE,
						    batch_sects,
						    &read_buf,
						    &read_buf_size) < 0) {
				vm_deallocate(mach_task_self(), buf, total);
				if (ra_buf)
					vm_deallocate(mach_task_self(),
						      ra_buf, ra_buf_needed);
				return D_IO_ERROR;
			}

			/* Copy: first 'total' bytes → client, rest → RA */
			if (offset < total) {
				unsigned int client_chunk = total - offset;
				if (client_chunk > chunk)
					client_chunk = chunk;
				memcpy((void *)(buf + offset),
				       (void *)read_buf, client_chunk);
				if (client_chunk < chunk && ra_buf) {
					memcpy((void *)ra_buf,
					       (void *)(read_buf + client_chunk),
					       chunk - client_chunk);
				}
			} else if (ra_buf) {
				unsigned int ra_off = offset - total;
				memcpy((void *)(ra_buf + ra_off),
				       (void *)read_buf, chunk);
			}

			vm_deallocate(mach_task_self(),
				      read_buf, read_buf_size);
		}

		/* Update readahead cache */
		if (ra_cache.buf != 0)
			vm_deallocate(mach_task_self(),
				      ra_cache.buf, ra_cache.buf_size);

		if (ra_extra > 0 && ra_buf != 0) {
			ra_cache.lba_start = lba + nsectors;
			ra_cache.lba_count = ra_extra;
			ra_cache.buf       = ra_buf;
			ra_cache.buf_size  = ra_buf_needed;
			ra_cache.part      = part;
		} else {
			ra_cache.lba_start = lba;
			ra_cache.lba_count = nsectors;
			ra_cache.buf       = 0;
			ra_cache.buf_size  = 0;
			ra_cache.part      = part;
		}
	}

	*data = (io_buf_ptr_t)buf;
	*data_count = (mach_msg_type_number_t)bytes_wanted;
	return KERN_SUCCESS;
}

/* ================================================================
 * ds_device_write
 * ================================================================ */

kern_return_t
ds_device_write(mach_port_t device, mach_port_t reply,
		mach_msg_type_name_t reply_poly,
		dev_mode_t mode, recnum_t recnum,
		io_buf_ptr_t data, mach_msg_type_number_t data_count,
		io_buf_len_t *bytes_written)
{
	struct blk_partition *part = (struct blk_partition *)device;
	struct blk_controller *ctrl = part->ctrl;
	unsigned int total, nsectors, lba;
	unsigned int offset, chunk, max_xfer;

	if (!part->cap_authenticated) {
		vm_deallocate(mach_task_self(), (vm_offset_t)data, data_count);
		return KERN_NO_ACCESS;
	}

	if (data_count <= 0)
		return D_INVALID_SIZE;

	total = (unsigned int)data_count;
	if (total % SECTOR_SIZE)
		total = (total + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
	nsectors = total / SECTOR_SIZE;

	if (recnum + nsectors > part->num_sectors) {
		vm_deallocate(mach_task_self(), (vm_offset_t)data, data_count);
		return D_INVALID_SIZE;
	}

	ra_invalidate(part);

	lba = part->start_lba + recnum;
	max_xfer = ctrl->disks[part->disk_index].max_transfer_bytes;
	if (max_xfer == 0)
		max_xfer = 128u * 1024u;

	for (offset = 0; offset < total; offset += chunk) {
		unsigned int batch_sects;

		chunk = total - offset;
		if (chunk > max_xfer)
			chunk = max_xfer;
		batch_sects = chunk / SECTOR_SIZE;

		if (ctrl->ops->write_sectors(ctrl->priv,
					     part->disk_index,
					     lba + offset / SECTOR_SIZE,
					     batch_sects,
					     (vm_offset_t)data + offset,
					     chunk) < 0) {
			vm_deallocate(mach_task_self(),
				      (vm_offset_t)data, data_count);
			return D_IO_ERROR;
		}
	}

	vm_deallocate(mach_task_self(), (vm_offset_t)data, data_count);
	*bytes_written = (io_buf_len_t)data_count;
	return KERN_SUCCESS;
}

/* ================================================================
 * ds_device_write_batch — multiple non-contiguous blocks
 * ================================================================ */

kern_return_t
ds_device_write_batch(mach_port_t device, mach_port_t reply,
		      mach_msg_type_name_t reply_poly,
		      dev_mode_t mode,
		      recnum_t *recnums,
		      mach_msg_type_number_t recnumsCnt,
		      unsigned int *sizes,
		      mach_msg_type_number_t sizesCnt,
		      io_buf_ptr_t data,
		      mach_msg_type_number_t data_count,
		      io_buf_len_t *bytes_written)
{
	struct blk_partition *part = (struct blk_partition *)device;
	struct blk_controller *ctrl = part->ctrl;

	if (!part->cap_authenticated) {
		vm_deallocate(mach_task_self(), (vm_offset_t)data, data_count);
		return KERN_NO_ACCESS;
	}

	if (recnumsCnt != sizesCnt || recnumsCnt == 0)
		return D_INVALID_SIZE;

	ra_invalidate(part);

	/* If module supports batch write, delegate */
	if (ctrl->ops->write_batch) {
		uint32_t lbas[16];
		unsigned int i;

		for (i = 0; i < recnumsCnt && i < 16; i++)
			lbas[i] = part->start_lba + recnums[i];

		if (ctrl->ops->write_batch(ctrl->priv,
					   part->disk_index,
					   lbas, sizes, recnumsCnt,
					   (vm_offset_t)data,
					   data_count) < 0) {
			vm_deallocate(mach_task_self(),
				      (vm_offset_t)data, data_count);
			return D_IO_ERROR;
		}

		/* Sum sizes for bytes_written */
		{
			unsigned int tw = 0;
			for (i = 0; i < recnumsCnt; i++)
				tw += sizes[i];
			*bytes_written = (io_buf_len_t)tw;
		}
		vm_deallocate(mach_task_self(),
			      (vm_offset_t)data, data_count);
		return KERN_SUCCESS;
	}

	/* Fallback: write each block individually */
	{
		unsigned int i, data_off = 0, total_written = 0;

		for (i = 0; i < recnumsCnt; i++) {
			unsigned int sz = sizes[i];
			unsigned int wr_total, lba, off, chunk;
			unsigned int max_xfer;

			if (sz == 0)
				continue;
			if (data_off + sz > data_count) {
				vm_deallocate(mach_task_self(),
					      (vm_offset_t)data, data_count);
				return D_INVALID_SIZE;
			}

			wr_total = (sz + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
			lba = part->start_lba + recnums[i];
			max_xfer = ctrl->disks[part->disk_index].max_transfer_bytes;
			if (max_xfer == 0)
				max_xfer = 128u * 1024u;

			for (off = 0; off < wr_total; off += chunk) {
				unsigned int sects;
				chunk = wr_total - off;
				if (chunk > max_xfer)
					chunk = max_xfer;
				sects = chunk / SECTOR_SIZE;

				if (ctrl->ops->write_sectors(
					ctrl->priv, part->disk_index,
					lba + off / SECTOR_SIZE,
					sects,
					(vm_offset_t)data + data_off + off,
					chunk) < 0) {
					vm_deallocate(mach_task_self(),
						      (vm_offset_t)data,
						      data_count);
					return D_IO_ERROR;
				}
			}
			data_off += sz;
			total_written += sz;
		}

		vm_deallocate(mach_task_self(),
			      (vm_offset_t)data, data_count);
		*bytes_written = (io_buf_len_t)total_written;
		return KERN_SUCCESS;
	}
}

/* ================================================================
 * Zero-copy physical DMA read/write
 * ================================================================ */

kern_return_t
ds_device_read_phys(mach_port_t device, mach_port_t reply,
		    mach_msg_type_name_t reply_poly,
		    dev_mode_t mode, recnum_t recnum,
		    io_buf_len_t bytes_wanted,
		    unsigned int *phys_addrs,
		    mach_msg_type_number_t phys_addrsCnt,
		    io_buf_len_t *bytes_read)
{
	struct blk_partition *part = (struct blk_partition *)device;
	struct blk_controller *ctrl = part->ctrl;
	unsigned int total, nsectors;

	if (!part->cap_authenticated)
		return KERN_NO_ACCESS;

	if (!ctrl->ops->read_sectors_phys)
		return D_INVALID_OPERATION;

	if (bytes_wanted == 0 || phys_addrsCnt == 0)
		return D_INVALID_SIZE;

	total = (bytes_wanted + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
	nsectors = total / SECTOR_SIZE;

	if (recnum + nsectors > part->num_sectors)
		return D_INVALID_SIZE;

	if (ctrl->ops->read_sectors_phys(ctrl->priv,
					  part->disk_index,
					  part->start_lba + recnum,
					  nsectors,
					  phys_addrs, phys_addrsCnt,
					  total) < 0)
		return D_IO_ERROR;

	*bytes_read = bytes_wanted;
	return KERN_SUCCESS;
}

kern_return_t
ds_device_write_phys(mach_port_t device, mach_port_t reply,
		     mach_msg_type_name_t reply_poly,
		     dev_mode_t mode, recnum_t recnum,
		     io_buf_len_t bytes_to_write,
		     unsigned int *phys_addrs,
		     mach_msg_type_number_t phys_addrsCnt,
		     io_buf_len_t *bytes_written)
{
	struct blk_partition *part = (struct blk_partition *)device;
	struct blk_controller *ctrl = part->ctrl;
	unsigned int total, nsectors;

	if (!part->cap_authenticated)
		return KERN_NO_ACCESS;

	if (!ctrl->ops->write_sectors_phys)
		return D_INVALID_OPERATION;

	if (bytes_to_write == 0 || phys_addrsCnt == 0)
		return D_INVALID_SIZE;

	total = (bytes_to_write + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
	nsectors = total / SECTOR_SIZE;

	if (recnum + nsectors > part->num_sectors)
		return D_INVALID_SIZE;

	ra_invalidate(part);

	if (ctrl->ops->write_sectors_phys(ctrl->priv,
					   part->disk_index,
					   part->start_lba + recnum,
					   nsectors,
					   phys_addrs, phys_addrsCnt,
					   total) < 0)
		return D_IO_ERROR;

	*bytes_written = bytes_to_write;
	return KERN_SUCCESS;
}

/* ================================================================
 * ds_device_get_status — DEV_GET_SIZE returns partition info
 * ================================================================ */

kern_return_t
ds_device_get_status(mach_port_t device, dev_flavor_t flavor,
		     dev_status_t status,
		     mach_msg_type_number_t *status_count)
{
	struct blk_partition *part = (struct blk_partition *)device;

	if (flavor == DEV_GET_SIZE) {
		status[DEV_GET_SIZE_DEVICE_SIZE] = (int)part->num_sectors;
		status[DEV_GET_SIZE_RECORD_SIZE] = (int)SECTOR_SIZE;
		*status_count = DEV_GET_SIZE_COUNT;
		return KERN_SUCCESS;
	}
	return D_INVALID_OPERATION;
}

/* ================================================================
 * Stubs for unimplemented device operations
 * ================================================================ */

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
