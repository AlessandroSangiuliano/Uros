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
 * ext2_server/ext2_server.c — Userspace ext2 filesystem server for Uros
 *
 * Architecture:
 *   - Block I/O via ahci_driver (netname_look_up + device_read RPC)
 *   - ext2 parsing provided by libsa_fs (ext2fs.c, unchanged)
 *   - MIG server interface: ext2fs_server.defs (subsystem 2920)
 *   - Port registered via netname_check_in("ext2_server")
 *
 * Clients:
 *   netname_look_up(name_server_port, "", "ext2_server", &fs_port);
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

/* ================================================================
 * Global Mach ports
 * ================================================================ */

static mach_port_t	host_port;
static mach_port_t	device_port;
static mach_port_t	security_port;
static mach_port_t	root_ledger_wired;
static mach_port_t	root_ledger_paged;
static mach_port_t	fs_port;

/*
 * The "device" that libsa_fs reads from.  dev_port is the ahci_driver
 * service port obtained via netname_look_up.  libsa_fs calls
 * device_read(dev_port, ...) which sends a MIG RPC to ahci_driver.
 * rec_size = 512 (sector size) so dbtorec() is identity.
 */
static struct device	ahci_dev;

/* ================================================================
 * Page cache writeback
 * ================================================================ */

/*
 * Context for the page cache writeback callback.
 * Stores the device port and the fs-block-to-sector multiplier.
 */
struct writeback_ctx {
	struct device	*dev;		/* device with blk handle */
	unsigned int	blk_to_sec;	/* EXT2_BLOCK_SIZE / DEV_BSIZE */
};

static struct writeback_ctx	wb_ctx;

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
 * Open file table
 * ================================================================ */

#define MAX_OPEN_FILES	16

struct open_file {
	int		in_use;
	fs_private_t	private;	/* opaque ext2fs state */
};

static struct open_file	open_files[MAX_OPEN_FILES];

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

	(void)arg;

	/* Create a port nobody sends to — mach_msg(RCV) with timeout
	 * gives us a portable sleep without busy-waiting. */
	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &sleep_port);
	if (kr != KERN_SUCCESS) {
		printf("ext2: writeback thread: port alloc failed\n");
		return NULL;
	}

	for (;;) {
		/* Sleep for WRITEBACK_INTERVAL_MS */
		(void)mach_msg(&msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
			       0, sizeof(msg), sleep_port,
			       WRITEBACK_INTERVAL_MS, MACH_PORT_NULL);

		/* Flush dirty metadata for all open files */
		for (int i = 0; i < MAX_OPEN_FILES; i++) {
			if (open_files[i].in_use)
				ext2fs_flush_metadata(open_files[i].private);
		}

		/* Flush dirty page cache blocks */
		if (ahci_dev.cache)
			page_cache_sync(ahci_dev.cache);
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
	fs_private_t priv;
	int fid, rc;
	(void)fs_port_arg;

	rc = ext2fs_open_file(&ahci_dev, path, &priv);
	if (rc != 0) {
		printf("ext2: open \"%s\" failed (rc=%d)\n", path, rc);
		return KERN_FAILURE;
	}

	for (fid = 0; fid < MAX_OPEN_FILES; fid++)
		if (!open_files[fid].in_use)
			break;
	if (fid == MAX_OPEN_FILES) {
		ext2fs_close_file(priv);
		free(priv);
		return KERN_RESOURCE_SHORTAGE;
	}

	open_files[fid].in_use  = 1;
	open_files[fid].private = priv;

	*fid_out = (natural_t)(fid + 1);	/* 1-based */
	printf("ext2: opened \"%s\" -> fid=%u\n", path, fid + 1);
	return KERN_SUCCESS;
}

kern_return_t
ds_ext2_stat(
	mach_port_t	fs_port_arg,
	natural_t	fid,
	natural_t	*file_size_out)
{
	int idx = (int)fid - 1;
	(void)fs_port_arg;

	if (idx < 0 || idx >= MAX_OPEN_FILES || !open_files[idx].in_use)
		return KERN_INVALID_ARGUMENT;

	*file_size_out = (natural_t)ext2fs_file_size(open_files[idx].private);
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
	int idx = (int)fid - 1;
	fs_private_t priv;
	kern_return_t kr;
	vm_offset_t buf;
	size_t fsize;
	int rc;
	(void)fs_port_arg;

	if (idx < 0 || idx >= MAX_OPEN_FILES || !open_files[idx].in_use)
		return KERN_INVALID_ARGUMENT;

	priv  = open_files[idx].private;
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
	int idx = (int)fid - 1;
	(void)fs_port_arg;

	if (idx < 0 || idx >= MAX_OPEN_FILES || !open_files[idx].in_use)
		return KERN_INVALID_ARGUMENT;

	ext2fs_close_file(open_files[idx].private);
	free(open_files[idx].private);
	open_files[idx].private = NULL;
	open_files[idx].in_use  = 0;

	printf("ext2: closed fid=%u\n", fid);
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
	int idx = (int)fid - 1;
	fs_private_t priv;
	int rc;
	(void)fs_port_arg;

	if (idx < 0 || idx >= MAX_OPEN_FILES || !open_files[idx].in_use)
		return KERN_INVALID_ARGUMENT;

	priv = open_files[idx].private;

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

	return KERN_SUCCESS;
}

kern_return_t
ds_ext2_sync(
	mach_port_t	fs_port_arg)
{
	int rc;
	(void)fs_port_arg;

	/* Flush dirty metadata (inode, group descriptors, superblock) */
	for (int i = 0; i < MAX_OPEN_FILES; i++) {
		if (open_files[i].in_use) {
			rc = ext2fs_flush_metadata(open_files[i].private);
			if (rc != 0)
				return KERN_FAILURE;
		}
	}

	/* Sync page cache to disk */
	if (ahci_dev.cache) {
		rc = page_cache_sync(ahci_dev.cache);
		if (rc != 0) {
			printf("ext2: sync failed, %d blocks not written\n",
			       rc);
			return KERN_FAILURE;
		}
	}

	return KERN_SUCCESS;
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

	/* Open block device via libblk.
	 * blk_open() discovers the driver via netname, waits for
	 * registration, opens the device, and probes capabilities. */
	{
		struct blk_dev *bd = blk_open(name_server_port,
					      "ahci_driver");
		if (!bd) {
			printf("ext2: blk_open(ahci_driver) failed\n");
			return 1;
		}
		ahci_dev.blk = bd;
		ahci_dev.dev_port = blk_port(bd);
		ahci_dev.rec_size = blk_rec_size(bd);
	}

	/* Create initial page cache (non-DMA, no writeback yet).
	 * After discovering the block size we upgrade to DMA-backed. */
	ahci_dev.cache = page_cache_create(8192, NULL, NULL);
	if (!ahci_dev.cache)
		printf("ext2: warning: page cache alloc failed, "
		       "running uncached\n");
	else
		printf("ext2: page cache enabled (8192 blocks)\n");

	/* Verify ext2 superblock by opening a known test file.
	 * Also use the first open to discover the block size and
	 * configure the page cache writeback callback.
	 */
	{
		fs_private_t priv;
		int rc = ext2fs_open_file(&ahci_dev, "hello.txt", &priv);
		if (rc == 0) {
			struct ext2fs_file *fp =
				(struct ext2fs_file *)priv;

			{
				int blksz = EXT2_BLOCK_SIZE(fp->f_fs);
				printf("ext2: mounted, /hello.txt size=%u bytes"
				       " (blk=%d)\n",
				       (unsigned int)ext2fs_file_size(priv),
				       blksz);

				/* Set up writeback context */
				wb_ctx.dev = &ahci_dev;
				wb_ctx.blk_to_sec = blksz / DEV_BSIZE;

				/* Upgrade to DMA-backed page cache */
				{
					unsigned int kva, uva;
					unsigned int pa_list[1024];
					mach_msg_type_number_t pa_cnt = 1024;
					unsigned int n_entries = 4096;
					unsigned int n_pages;
					struct page_cache *dma_pc;

					n_pages = (n_entries * blksz + 4095)
						  / 4096;
					if (n_pages > 1024)
						n_pages = 1024;

					kr = device_dma_alloc_sg(
						device_port, n_pages,
						mach_task_self(),
						&kva, &uva,
						pa_list, &pa_cnt);
					if (kr == KERN_SUCCESS) {
						dma_pc = page_cache_create_dma(
							n_entries,
							(vm_size_t)blksz,
							(vm_offset_t)uva,
							pa_list, pa_cnt,
							ext2_writeback,
							&wb_ctx);
						if (dma_pc) {
							page_cache_destroy(
								ahci_dev.cache);
							ahci_dev.cache = dma_pc;
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
				if (!ahci_dev.cache->pc_dma_pool &&
				    ahci_dev.cache) {
					ahci_dev.cache->pc_writeback =
						ext2_writeback;
					ahci_dev.cache->pc_writeback_ctx =
						&wb_ctx;
				}
				printf("ext2: writeback enabled\n");
			}

			ext2fs_close_file(priv);
			free(priv);
		} else {
			printf("ext2: mount OK (test file not found, rc=%d)\n",
			       rc);
		}
	}

#ifdef EXT2_BENCH
	/* Page cache benchmark: cold (disk) vs warm (cache) throughput.
	 * Build with -DEXT2_BENCH to enable.
	 */
	{
		fs_private_t priv;
		int rc = ext2fs_open_file(&ahci_dev, "bench.dat", &priv);
		if (rc != 0)
			rc = ext2fs_open_file(&ahci_dev, "hello.txt", &priv);
		if (rc == 0) {
			size_t fsize = ext2fs_file_size(priv);

			if (fsize > 0) {
				vm_offset_t buf;
				unsigned int lo0, hi0, lo1, hi1;
				unsigned long long cy0, cy1, elapsed;
				unsigned int us, mbs;
				int i;
				const int WARM_ITERS = 10;

				kr = vm_allocate(mach_task_self(), &buf,
						 fsize, TRUE);
				if (kr == KERN_SUCCESS) {
					/* Cold read (all blocks from disk) */
					__asm__ volatile("rdtsc"
						: "=a"(lo0), "=d"(hi0));
					ext2fs_read_file(priv, 0, buf, fsize);
					__asm__ volatile("rdtsc"
						: "=a"(lo1), "=d"(hi1));
					cy0 = ((unsigned long long)hi0 << 32)
					      | lo0;
					cy1 = ((unsigned long long)hi1 << 32)
					      | lo1;
					/* >>10 ≈ /1024, avoids __udivdi3 */
					elapsed = cy1 - cy0;
					us = (unsigned int)(elapsed >> 10);

					/* Close and reopen to clear per-file
					 * f_buf (page cache stays warm) */
					ext2fs_close_file(priv);
					free(priv);
					priv = NULL;

					rc = ext2fs_open_file(&ahci_dev,
							"bench.dat", &priv);
					if (rc != 0)
						rc = ext2fs_open_file(
							&ahci_dev,
							"hello.txt", &priv);

					printf("ext2 bench: %u KB, "
					       "%d warm iters\n",
					       (unsigned int)(fsize / 1024),
					       WARM_ITERS);
					mbs = us ? fsize / us : 0;
					printf("  cold: %u us (%u MB/s)\n",
					       us, mbs);

					if (rc == 0) {
						/* Warm reads (cache hits) */
						__asm__ volatile("rdtsc"
							: "=a"(lo0),
							  "=d"(hi0));
						for (i = 0; i < WARM_ITERS; i++)
							ext2fs_read_file(priv,
								0, buf, fsize);
						__asm__ volatile("rdtsc"
							: "=a"(lo1),
							  "=d"(hi1));
						cy0 = ((unsigned long long)
						       hi0 << 32) | lo0;
						cy1 = ((unsigned long long)
						       hi1 << 32) | lo1;
						elapsed = (cy1 - cy0);
						/* Per-iteration: shift then
						 * divide by WARM_ITERS */
						us = (unsigned int)
						     (elapsed >> 10) /
						     WARM_ITERS;
						mbs = us ? fsize / us : 0;
						printf("  warm: %u us "
						       "(%u MB/s)\n",
						       us, mbs);
					}

					vm_deallocate(mach_task_self(),
						      buf, fsize);
				}
			}

			if (priv) {
				ext2fs_close_file(priv);
				free(priv);
			}
		}
	}
	if (ahci_dev.cache)
		page_cache_print_stats(ahci_dev.cache);
#endif /* EXT2_BENCH */

	/* Allocate our service receive port */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &fs_port);
	if (kr != KERN_SUCCESS) {
		printf("ext2: port alloc failed\n");
		return 1;
	}
	kr = mach_port_insert_right(mach_task_self(), fs_port, fs_port,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("ext2: port right failed\n");
		return 1;
	}

	/* Register with name server */
	kr = netname_check_in(name_server_port, "ext2_server",
			      MACH_PORT_NULL, fs_port);
	if (kr != KERN_SUCCESS)
		printf("ext2: netname_check_in failed (kr=%d)\n", kr);
	else
		printf("ext2: registered as \"ext2_server\"\n");

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

	/* MIG server loop */
	mach_msg_server(ext2fs_server_server, 8192, fs_port,
			MACH_MSG_OPTION_NONE);

	return 0;
}
