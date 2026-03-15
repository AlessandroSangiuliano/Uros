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

#include <ext2fs/ext2fs.h>
#include <file_system.h>
#include <page_cache.h>

#include "ext2fs_server_server.h"

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
 * Open file table
 * ================================================================ */

#define MAX_OPEN_FILES	16

struct open_file {
	int		in_use;
	fs_private_t	private;	/* opaque ext2fs state */
};

static struct open_file	open_files[MAX_OPEN_FILES];

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

	/* Look up the AHCI driver service port via netname.
	 * The AHCI driver may still be initializing (PCI scan, DMA alloc),
	 * so retry with mach_msg timeout to sleep properly between
	 * attempts instead of busy-waiting.  See #72 for the future
	 * name-server notification approach.
	 */
	{
		mach_port_t ahci_port;
		mach_port_t sleep_port;
		int retries;

		/* Dummy port for mach_msg timeout sleep */
		mach_port_allocate(mach_task_self(),
				   MACH_PORT_RIGHT_RECEIVE, &sleep_port);

		for (retries = 0; retries < 60; retries++) {
			kr = netname_look_up(name_server_port, "",
					     "ahci_driver", &ahci_port);
			if (kr == KERN_SUCCESS)
				break;
			/* Sleep ~100 ms via mach_msg receive timeout */
			{
				mach_msg_header_t msg;
				mach_msg(&msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
					 0, sizeof(msg), sleep_port,
					 100, MACH_PORT_NULL);
			}
		}
		mach_port_destroy(mach_task_self(), sleep_port);

		if (kr != KERN_SUCCESS) {
			printf("ext2: ahci_driver not found after "
			       "%d retries (kr=%d)\n", retries, kr);
			return 1;
		}
		printf("ext2: connected to ahci_driver (port=%d)\n",
		       ahci_port);
		ahci_dev.dev_port = ahci_port;
	}
	ahci_dev.rec_size = 512;

	/* Create page cache: 8192 entries = 8 MB with 1 KB blocks */
	ahci_dev.cache = page_cache_create(8192);
	if (!ahci_dev.cache)
		printf("ext2: warning: page cache alloc failed, "
		       "running uncached\n");
	else
		printf("ext2: page cache enabled (8192 blocks)\n");

	/* Verify ext2 superblock by opening a known test file. */
	{
		fs_private_t priv;
		int rc = ext2fs_open_file(&ahci_dev, "hello.txt", &priv);
		if (rc == 0) {
			printf("ext2: mounted, /hello.txt size=%u bytes\n",
			       (unsigned int)ext2fs_file_size(priv));
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

	printf("ext2: ready, entering message loop\n");

	/* MIG server loop */
	mach_msg_server(ext2fs_server_server, 8192, fs_port,
			MACH_MSG_OPTION_NONE);

	return 0;
}
