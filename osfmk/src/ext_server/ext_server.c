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
 * ext_server/ext_server.c — Userspace ext2/ext3/ext4 filesystem server for Uros
 *
 * Architecture:
 *   - Block I/O via ahci_driver (netname_look_up + device_read RPC)
 *   - ext2 parsing provided by libsa_fs (ext2fs.c, unchanged)
 *   - MIG server interface: ext2fs_server.defs (subsystem 2920)
 *   - Per-mount ports registered via netname_check_in("ext_server")
 *   - Multiple partitions supported via mount_context array
 *
 * Clients:
 *   netname_look_up(name_server_port, "", "ext_server", &fs_port);
 *   ext2_open(fs_port, "/path", &fid);
 *   ext2_stat(fs_port, fid, &size);
 *   ext2_read(fs_port, fid, offset, count, &data, &data_count);
 *   ext2_close(fs_port, fid);
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/bootstrap.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <sa_mach.h>
#include <device/device.h>
#include <stdio.h>
#include <stdlib.h>
#include <servers/netname.h>
#include <servers/netname_defs.h>

#include <cthreads.h>
#include <ext2fs/ext2fs.h>
#include <ext2fs/defs.h>
#include <file_system.h>
#include <page_cache.h>
#include <blk.h>

#include "ext2fs_server_server.h"
#include "ahci_batch.h"
#include "device_master.h"

/* MIG stub — generated from mach_port.defs */
extern kern_return_t mach_port_set_protected_payload(
	mach_port_t task, mach_port_t name, unsigned payload);

/* ================================================================
 * Global Mach ports
 * ================================================================ */

static mach_port_t	host_port;
static mach_port_t	device_port;
static mach_port_t	security_port;
static mach_port_t	root_ledger_wired;
static mach_port_t	root_ledger_paged;

/* ================================================================
 * Per-mount context — one per mounted partition
 * ================================================================ */

#define MAX_OPEN_FILES	16
#define MAX_MOUNTS	4

struct open_file {
	int		in_use;
	fs_private_t	private;	/* opaque ext2fs state */
	struct ext2fs_file file_data;	/* pre-allocated (object pool) */
	char		path[256];	/* path for clone matching */
	int		on_dirty_list;	/* non-zero if linked in dirty_head */
	int		dirty_next;	/* index of next dirty, -1 = end */
	int		dirty_prev;	/* index of prev dirty, -1 = head */
};

/*
 * Context for the page cache writeback callback.
 */
struct writeback_ctx {
	struct device	*dev;		/* device with blk handle */
	unsigned int	blk_to_sec;	/* EXT2_BLOCK_SIZE / DEV_BSIZE */
};

/*
 * Mount context: all per-partition state.
 * Protected payload on each mount port points here, so MIG handlers
 * receive it as fs_port_arg and can access the right partition.
 */
struct mount_context {
	int		active;			/* non-zero if mounted */
	struct device	dev;			/* block device */
	struct writeback_ctx wb;		/* writeback context */
	struct open_file open_files[MAX_OPEN_FILES];
	int		dirty_head;		/* dirty list head (-1 = empty) */
	mach_port_t	port;			/* receive port for this mount */
	char		driver_name[64];	/* block driver name */
	char		service_name[64];	/* netname registration */
};

static struct mount_context	mounts[MAX_MOUNTS];
static int			n_mounts;
static mach_port_t		port_set;	/* port set for all mounts */

static void
dirty_list_add(struct mount_context *mnt, int idx)
{
	if (mnt->open_files[idx].on_dirty_list)
		return;
	mnt->open_files[idx].on_dirty_list = 1;
	mnt->open_files[idx].dirty_prev = -1;
	mnt->open_files[idx].dirty_next = mnt->dirty_head;
	if (mnt->dirty_head >= 0)
		mnt->open_files[mnt->dirty_head].dirty_prev = idx;
	mnt->dirty_head = idx;
}

static void
dirty_list_remove(struct mount_context *mnt, int idx)
{
	if (!mnt->open_files[idx].on_dirty_list)
		return;
	mnt->open_files[idx].on_dirty_list = 0;
	if (mnt->open_files[idx].dirty_prev >= 0)
		mnt->open_files[mnt->open_files[idx].dirty_prev].dirty_next =
		    mnt->open_files[idx].dirty_next;
	else
		mnt->dirty_head = mnt->open_files[idx].dirty_next;
	if (mnt->open_files[idx].dirty_next >= 0)
		mnt->open_files[mnt->open_files[idx].dirty_next].dirty_prev =
		    mnt->open_files[idx].dirty_prev;
	mnt->open_files[idx].dirty_next = -1;
	mnt->open_files[idx].dirty_prev = -1;
}

/* ================================================================
 * Page cache writeback
 * ================================================================ */

/*
 * Flush a dirty page cache block to disk via libblk.
 * If phys != 0, use zero-copy DMA write.
 * Otherwise fall back to regular write with data copy.
 */
static int
ext2_writeback(void *ctx, daddr_t block, vm_offset_t data, vm_size_t size,
	       vm_offset_t phys)
{
	struct writeback_ctx *wb = (struct writeback_ctx *)ctx;
	io_buf_len_t bytes_written;
	recnum_t recnum = (recnum_t)(block * wb->blk_to_sec *
				     DEV_BSIZE / wb->dev->rec_size);
	kern_return_t rc;

	if (phys && blk_has_phys(wb->dev->blk)) {
		unsigned int pa = (unsigned int)phys;
		rc = blk_write_phys(wb->dev->blk, recnum,
				    (io_buf_len_t)size,
				    &pa, 1, &bytes_written);
	} else {
		rc = blk_write(wb->dev->blk, recnum,
			       (io_buf_ptr_t)data,
			       (mach_msg_type_number_t)size,
			       &bytes_written);
	}
	if (rc != KERN_SUCCESS) {
		printf("ext2: writeback block %ld failed: %d\n",
		       (long)block, rc);
		return -1;
	}
	return 0;
}

/* ================================================================
 * Background writeback thread
 * ================================================================ */

#define WRITEBACK_INTERVAL_MS	5000	/* flush dirty pages every 5 seconds */

/*
 * Background writeback thread.  Periodically flushes dirty metadata
 * and page cache blocks to disk.  Uses mach_msg with timeout as sleep.
 */
static void *
writeback_thread(void *arg)
{
	mach_port_t	sleep_port;
	kern_return_t	kr;
	mach_msg_header_t msg;
	int		mi;

	(void)arg;

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &sleep_port);
	if (kr != KERN_SUCCESS) {
		printf("ext2: writeback thread: port alloc failed\n");
		return NULL;
	}

	for (;;) {
		(void)mach_msg(&msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
			       0, sizeof(msg), sleep_port,
			       WRITEBACK_INTERVAL_MS, MACH_PORT_NULL);

		for (mi = 0; mi < n_mounts; mi++) {
			struct mount_context *mnt = &mounts[mi];
			if (!mnt->active)
				continue;

			/* Flush dirty metadata */
			{
				int i = mnt->dirty_head;
				while (i >= 0) {
					int next =
					    mnt->open_files[i].dirty_next;
					ext2fs_flush_metadata(
					    mnt->open_files[i].private);
					if (!ext2fs_is_dirty(
					    mnt->open_files[i].private))
						dirty_list_remove(mnt, i);
					i = next;
				}
			}

			/* Flush dirty page cache blocks */
			if (mnt->dev.cache)
				page_cache_sync(mnt->dev.cache);
		}
	}

	/* NOTREACHED */
	return NULL;
}

/* ================================================================
 * MIG server routines  (ds_ prefix from ext2fs_server.defs)
 * ================================================================ */

kern_return_t
ds_ext2_open(
	mach_port_t		fs_port_arg,
	ext2_path_t		path,
	natural_t		*fid_out)
{
	struct mount_context *mnt = (struct mount_context *)fs_port_arg;
	fs_private_t priv;
	int fid, rc, i, donor;

	/* Find a free slot (pool allocation) */
	for (fid = 0; fid < MAX_OPEN_FILES; fid++)
		if (!mnt->open_files[fid].in_use)
			break;
	if (fid == MAX_OPEN_FILES)
		return KERN_RESOURCE_SHORTAGE;

	/* Check for existing open with same path — clone inode data
	 * instead of full path walk + disk I/O.  Each opener gets its
	 * own fid with independent read state. */
	donor = -1;
	for (i = 0; i < MAX_OPEN_FILES; i++) {
		if (mnt->open_files[i].in_use &&
		    strcmp(mnt->open_files[i].path, path) == 0) {
			donor = i;
			break;
		}
	}

	if (donor >= 0) {
		/* Clone: copy inode, skip path walk */
		ext2fs_clone_file(&mnt->open_files[fid].file_data,
				  &mnt->open_files[donor].file_data);
		priv = (fs_private_t)&mnt->open_files[fid].file_data;
		printf("ext2: cloned \"%s\" -> fid=%u (from fid=%u)\n",
		       path, fid + 1, donor + 1);
	} else {
		/* Full open with path walk */
		rc = ext2fs_open_file_into(&mnt->dev, path, &priv,
					   &mnt->open_files[fid].file_data);
		if (rc != 0) {
			printf("ext2: open \"%s\" failed (rc=%d)\n",
			       path, rc);
			return KERN_FAILURE;
		}
		printf("ext2: opened \"%s\" -> fid=%u\n", path, fid + 1);
	}

	mnt->open_files[fid].in_use  = 1;
	mnt->open_files[fid].private = priv;
	strncpy(mnt->open_files[fid].path, path,
		sizeof(mnt->open_files[fid].path) - 1);
	mnt->open_files[fid].path[sizeof(mnt->open_files[fid].path) - 1] =
		'\0';

	*fid_out = (natural_t)(fid + 1);
	return KERN_SUCCESS;
}

kern_return_t
ds_ext2_stat(
	mach_port_t	fs_port_arg,
	natural_t	fid,
	natural_t	*file_size_out)
{
	struct mount_context *mnt = (struct mount_context *)fs_port_arg;
	int idx = (int)fid - 1;

	if (idx < 0 || idx >= MAX_OPEN_FILES || !mnt->open_files[idx].in_use)
		return KERN_INVALID_ARGUMENT;

	*file_size_out =
		(natural_t)ext2fs_file_size(mnt->open_files[idx].private);
	return KERN_SUCCESS;
}

kern_return_t
ds_ext2_read(
	mach_port_t		fs_port_arg,
	natural_t		fid,
	natural_t		offset,
	natural_t		count,
	pointer_t		*data_out,
	mach_msg_type_number_t	*data_count_out)
{
	struct mount_context *mnt = (struct mount_context *)fs_port_arg;
	int idx = (int)fid - 1;
	fs_private_t priv;
	kern_return_t kr;
	vm_offset_t buf;
	size_t fsize;
	int rc;

	if (idx < 0 || idx >= MAX_OPEN_FILES || !mnt->open_files[idx].in_use)
		return KERN_INVALID_ARGUMENT;

	priv  = mnt->open_files[idx].private;
	fsize = ext2fs_file_size(priv);

	/* Clamp to file size */
	if (offset >= fsize || count == 0) {
		*data_out       = (pointer_t)0;
		*data_count_out = 0;
		return KERN_SUCCESS;
	}
	if (offset + count > fsize)
		count = (natural_t)(fsize - offset);

	kr = vm_allocate(mach_task_self(), &buf, (vm_size_t)count, TRUE);
	if (kr != KERN_SUCCESS)
		return kr;

	rc = ext2fs_read_file(priv, (vm_offset_t)offset,
			      buf, (vm_size_t)count);
	if (rc != 0) {
		vm_deallocate(mach_task_self(), buf, (vm_size_t)count);
		return KERN_FAILURE;
	}

	*data_out       = (pointer_t)buf;
	*data_count_out = (mach_msg_type_number_t)count;
	return KERN_SUCCESS;
}

kern_return_t
ds_ext2_close(
	mach_port_t	fs_port_arg,
	natural_t	fid)
{
	struct mount_context *mnt = (struct mount_context *)fs_port_arg;
	int idx = (int)fid - 1;

	if (idx < 0 || idx >= MAX_OPEN_FILES || !mnt->open_files[idx].in_use)
		return KERN_INVALID_ARGUMENT;

	dirty_list_remove(mnt, idx);
	ext2fs_close_file(mnt->open_files[idx].private);

	mnt->open_files[idx].private = NULL;
	mnt->open_files[idx].in_use  = 0;
	mnt->open_files[idx].path[0] = '\0';

	return KERN_SUCCESS;
}

kern_return_t
ds_ext2_write(
	mach_port_t		fs_port_arg,
	natural_t		fid,
	natural_t		offset,
	pointer_t		data,
	mach_msg_type_number_t	data_count)
{
	struct mount_context *mnt = (struct mount_context *)fs_port_arg;
	int idx = (int)fid - 1;
	fs_private_t priv;
	int rc;

	if (idx < 0 || idx >= MAX_OPEN_FILES || !mnt->open_files[idx].in_use)
		return KERN_INVALID_ARGUMENT;

	priv = mnt->open_files[idx].private;

	rc = ext2fs_write_file(priv, (vm_offset_t)offset,
			       (vm_offset_t)data, (vm_size_t)data_count);

	/* MIG OOL data must be deallocated by the server */
	vm_deallocate(mach_task_self(), (vm_offset_t)data,
		      (vm_size_t)data_count);

	if (rc != 0) {
		printf("ext2: write fid=%u offset=%u count=%u failed: %d\n",
		       fid, offset, data_count, rc);
		return KERN_FAILURE;
	}

	/* Write sets dirty flags — track for efficient sync */
	dirty_list_add(mnt, idx);

	return KERN_SUCCESS;
}

kern_return_t
ds_ext2_sync(
	mach_port_t	fs_port_arg)
{
	struct mount_context *mnt = (struct mount_context *)fs_port_arg;
	int rc;

	/* Flush dirty metadata — iterate only dirty files */
	{
		int i = mnt->dirty_head;
		while (i >= 0) {
			int next = mnt->open_files[i].dirty_next;
			rc = ext2fs_flush_metadata(
				mnt->open_files[i].private);
			if (rc != 0)
				return KERN_FAILURE;
			if (!ext2fs_is_dirty(mnt->open_files[i].private))
				dirty_list_remove(mnt, i);
			i = next;
		}
	}

	/* Sync page cache to disk */
	if (mnt->dev.cache) {
		rc = page_cache_sync(mnt->dev.cache);
		if (rc != 0) {
			printf("ext2: sync failed, %d blocks not written\n",
			       rc);
			return KERN_FAILURE;
		}
	}

	return KERN_SUCCESS;
}

kern_return_t
ds_ext2_open_read(
	mach_port_t		fs_port_arg,
	ext2_path_t		path,
	natural_t		offset,
	natural_t		count,
	natural_t		*fid_out,
	pointer_t		*data_out,
	mach_msg_type_number_t	*data_count_out)
{
	kern_return_t kr;
	natural_t fid;

	/* Open */
	kr = ds_ext2_open(fs_port_arg, path, &fid);
	if (kr != KERN_SUCCESS) {
		*data_out       = (pointer_t)0;
		*data_count_out = 0;
		return kr;
	}

	/* Read */
	kr = ds_ext2_read(fs_port_arg, fid, offset, count,
			  data_out, data_count_out);
	if (kr != KERN_SUCCESS) {
		ds_ext2_close(fs_port_arg, fid);
		return kr;
	}

	*fid_out = fid;
	return KERN_SUCCESS;
}

kern_return_t
ds_ext2_read_close(
	mach_port_t		fs_port_arg,
	natural_t		fid,
	natural_t		offset,
	natural_t		count,
	pointer_t		*data_out,
	mach_msg_type_number_t	*data_count_out)
{
	kern_return_t kr;

	/* Read */
	kr = ds_ext2_read(fs_port_arg, fid, offset, count,
			  data_out, data_count_out);

	/* Close regardless of read result */
	ds_ext2_close(fs_port_arg, fid);

	return kr;
}

/* ================================================================
 * Mount a single partition
 * ================================================================ */

/*
 * mount_partition — mount a single partition.
 *
 * If blocking is non-zero, blk_open() waits for the driver to appear
 * (used for the first/mandatory partition).  If blocking is zero,
 * blk_open_try() returns immediately with NULL if the driver hasn't
 * registered — used for optional/extra partitions.
 */
static int
mount_partition(struct mount_context *mnt, const char *driver_name,
		const char *service_name, int blocking)
{
	kern_return_t kr;
	struct blk_dev *bd;

	strncpy(mnt->driver_name, driver_name, sizeof(mnt->driver_name) - 1);
	strncpy(mnt->service_name, service_name,
		sizeof(mnt->service_name) - 1);
	mnt->dirty_head = -1;

	/* Open block device via libblk */
	if (blocking)
		bd = blk_open(name_server_port, driver_name);
	else
		bd = blk_open_try(name_server_port, driver_name);
	if (!bd) {
		if (blocking)
			printf("ext2: blk_open(%s) failed\n", driver_name);
		return -1;
	}
	mnt->dev.blk = bd;
	mnt->dev.dev_port = blk_port(bd);
	mnt->dev.rec_size = blk_rec_size(bd);
	mnt->dev.mount_data = NULL;

	/* Create initial page cache (non-DMA, no writeback yet) */
	mnt->dev.cache = page_cache_create(8192, NULL, NULL);
	if (!mnt->dev.cache)
		printf("ext2: warning: page cache alloc failed, "
		       "running uncached\n");
	else
		printf("ext2: page cache enabled (8192 blocks)\n");

	/* Verify ext2 superblock by opening a known test file.
	 * Also use the first open to discover the block size and
	 * configure the page cache writeback callback. */
	{
		fs_private_t priv;
		int rc = ext2fs_open_file(&mnt->dev, "hello.txt", &priv);
		if (rc == 0) {
			struct ext2fs_file *fp =
				(struct ext2fs_file *)priv;
			int blksz = EXT2_BLOCK_SIZE(fp->f_fs);

			printf("ext2: mounted, /hello.txt size=%u bytes"
			       " (blk=%d)\n",
			       (unsigned int)ext2fs_file_size(priv),
			       blksz);

			/* Set up writeback context */
			mnt->wb.dev = &mnt->dev;
			mnt->wb.blk_to_sec = blksz / DEV_BSIZE;

			/* Upgrade to DMA-backed page cache */
			{
				unsigned int kva, uva;
				unsigned int pa_list[1024];
				mach_msg_type_number_t pa_cnt = 1024;
				unsigned int n_entries = 4096;
				unsigned int n_pages;
				struct page_cache *dma_pc;

				n_pages = (n_entries * blksz + 4095) / 4096;
				if (n_pages > 1024)
					n_pages = 1024;

				kr = device_dma_alloc_sg(
					device_port, n_pages,
					mach_task_self(),
					&kva, &uva,
					pa_list, &pa_cnt);
				printf("ext2: DMA alloc: kr=%d kva=0x%x uva=0x%x n_pages=%u pa_cnt=%u\n",
				       kr, kva, uva, n_pages, pa_cnt);
				if (kr == KERN_SUCCESS) {
					dma_pc = page_cache_create_dma(
						n_entries,
						(vm_size_t)blksz,
						(vm_offset_t)uva,
						pa_list, pa_cnt,
						ext2_writeback,
						&mnt->wb);
					if (dma_pc) {
						page_cache_destroy(
							mnt->dev.cache);
						mnt->dev.cache = dma_pc;
						printf("ext2: DMA page "
						       "cache (%u "
						       "entries, %u "
						       "pages)\n",
						       dma_pc->pc_max_entries,
						       pa_cnt);
					} else {
						printf("ext2: DMA cache "
						       "create failed, "
						       "using non-DMA\n");
						vm_deallocate(
							mach_task_self(),
							(vm_offset_t)uva,
							(vm_size_t)n_pages
								* 4096);
					}
				} else {
					printf("ext2: DMA alloc failed "
					       "(kr=%d), using "
					       "non-DMA cache\n", kr);
				}
			}

			/* Set writeback on non-DMA cache if
			 * DMA upgrade failed */
			if (mnt->dev.cache &&
			    !mnt->dev.cache->pc_dma_pool) {
				mnt->dev.cache->pc_writeback =
					ext2_writeback;
				mnt->dev.cache->pc_writeback_ctx =
					&mnt->wb;
			}
			printf("ext2: writeback enabled\n");

			ext2fs_close_file(priv);
			free(priv);
		} else {
			printf("ext2: mount OK (test file not found, "
			       "rc=%d)\n", rc);
		}
	}

	/* Allocate per-mount receive port */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &mnt->port);
	if (kr != KERN_SUCCESS) {
		printf("ext2: port alloc failed for %s\n", service_name);
		return -1;
	}
	kr = mach_port_insert_right(mach_task_self(), mnt->port, mnt->port,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("ext2: port right failed for %s\n", service_name);
		return -1;
	}

	/* Set protected payload — MIG handlers receive mnt as fs_port_arg */
	kr = mach_port_set_protected_payload(mach_task_self(),
					     mnt->port,
					     (unsigned long)mnt);
	if (kr != KERN_SUCCESS)
		printf("ext2: set_protected_payload failed (kr=%d)\n", kr);
	else
		printf("ext2: protected payload set (mnt=%p)\n",
		       (void *)mnt);

	/* Add to port set */
	kr = mach_port_move_member(mach_task_self(), mnt->port, port_set);
	if (kr != KERN_SUCCESS) {
		printf("ext2: port_move_member failed (kr=%d)\n", kr);
		return -1;
	}

	/* Register with name server */
	kr = netname_check_in(name_server_port, service_name,
			      MACH_PORT_NULL, mnt->port);
	if (kr != KERN_SUCCESS)
		printf("ext2: netname_check_in(%s) failed (kr=%d)\n",
		       service_name, kr);
	else
		printf("ext2: registered as \"%s\"\n", service_name);

	mnt->active = 1;
	return 0;
}

/* ================================================================
 * Main entry point
 * ================================================================ */

int
main(int argc, char **argv)
{
	kern_return_t kr;
	(void)argc; (void)argv;

	kr = bootstrap_ports(bootstrap_port,
			     &host_port, &device_port,
			     &root_ledger_wired, &root_ledger_paged,
			     &security_port);
	if (kr != KERN_SUCCESS)
		_exit(1);

	printf_init(device_port);
	panic_init(host_port);

	printf("\n=== ext2 filesystem server ===\n");

	/* Create port set for all mount ports */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_PORT_SET, &port_set);
	if (kr != KERN_SUCCESS) {
		printf("ext2: port set alloc failed\n");
		return 1;
	}

	/* Mount partitions.
	 * First partition (ahci0a) is mandatory — blocks until available.
	 * Additional partitions are optional — non-blocking lookup. */
	{
		static const struct {
			const char *driver;
			const char *service;
		} part_table[] = {
			{ "ahci0a", "ext_server" },
			{ "ahci0b", "ext_server:1" },
			{ "ahci1a", "ext_server:2" },
			{ "ahci1b", "ext_server:3" },
		};
		int i;

		for (i = 0; i < MAX_MOUNTS; i++) {
			int rc = mount_partition(&mounts[i],
				part_table[i].driver,
				part_table[i].service,
				/*blocking=*/ (i == 0));
			if (rc < 0) {
				if (i == 0)
					return 1;	/* mandatory */
				continue;		/* optional */
			}
			n_mounts++;
		}

		printf("ext2: %d partition(s) mounted\n", n_mounts);
	}

	/* Start background writeback thread */
	{
		cthread_t wb_thread;
		wb_thread = cthread_fork((cthread_fn_t)writeback_thread, NULL);
		cthread_detach(wb_thread);
		cthread_set_name(wb_thread, "writeback");
		printf("ext2: writeback thread started (%d ms interval)\n",
		       WRITEBACK_INTERVAL_MS);
	}

	printf("ext2: ready, entering message loop\n");

	/* MIG server loop — receives from port set covering all mounts */
	mach_msg_server(ext2fs_server_server, 8192, port_set,
			MACH_MSG_OPTION_NONE);

	return 0;
}
