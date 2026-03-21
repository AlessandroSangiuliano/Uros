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
 * disk_bench.c — Disk I/O performance benchmark module for ipc_bench
 *
 * Measures end-to-end I/O performance through the Uros userspace
 * driver stack:
 *
 *   Raw read:  ipc_bench → device_read RPC → ahci_driver → AHCI DMA
 *   Raw write: ipc_bench → device_write RPC → ahci_driver → AHCI DMA
 *   FS read:   ipc_bench → ext2_read RPC → ext2_server
 *                        → device_read RPC → ahci_driver → AHCI DMA
 *   FS write:  ipc_bench → ext2_write RPC → ext2_server
 *                        → device_write RPC → ahci_driver → AHCI DMA
 *
 * Both ahci_driver and ext2_server are discovered via netname_look_up.
 * If either server is not running, the corresponding benchmarks are
 * skipped (not an error — allows running ipc_bench without --ahci).
 */

#include <mach.h>
#include <mach/mach_port.h>
#include <mach/clock.h>
#include <mach/clock_types.h>
#include <device/device.h>
#include <device/device_types.h>
#include <servers/netname.h>
#include <sa_mach.h>
#include <stdio.h>

#include "ext2fs_server.h"

#include "disk_bench.h"

/* ===================================================================
 * Timing helpers (same as ipc_bench.c)
 * =================================================================== */

static mach_port_t	bench_clock_port;

static void
dget_time(tvalspec_t *tv)
{
	clock_get_time(bench_clock_port, tv);
}

static unsigned long
delapsed_ns(const tvalspec_t *before, const tvalspec_t *after)
{
	unsigned long ns;
	ns  = (unsigned long)(after->tv_sec  - before->tv_sec)  * 1000000000UL;
	ns += (unsigned long)(after->tv_nsec - before->tv_nsec);
	return ns;
}

/* ===================================================================
 * Server ports
 * =================================================================== */

static mach_port_t	ahci_port;	/* ahci_driver service port */
static mach_port_t	ext2_port;	/* ext2_server service port */

/* ===================================================================
 * Raw AHCI read benchmark
 *
 * Reads `total_sectors` sectors sequentially in `chunk_size`-byte
 * requests via device_read() to ahci_driver.  Measures latency per
 * request and throughput.
 * =================================================================== */

#define SECTOR_SIZE	512u

static void
bench_raw_read(const char *label, unsigned int chunk_size,
	       unsigned int total_bytes)
{
	tvalspec_t		t0, t1;
	unsigned long		total_ns;
	unsigned int		sectors_per_chunk, total_sectors;
	unsigned int		lba, iters, i;
	io_buf_ptr_t		buf;
	mach_msg_type_number_t	count;
	kern_return_t		kr;

	sectors_per_chunk = chunk_size / SECTOR_SIZE;
	total_sectors = total_bytes / SECTOR_SIZE;
	iters = total_sectors / sectors_per_chunk;
	if (iters == 0)
		iters = 1;

	/* Warmup: 4 reads */
	for (i = 0; i < 4; i++) {
		kr = device_read(ahci_port, 0, (recnum_t)i * sectors_per_chunk,
				 (io_buf_len_t)chunk_size, &buf, &count);
		if (kr != KERN_SUCCESS) {
			printf("  %s: device_read warmup failed kr=%d\n",
			       label, kr);
			return;
		}
		vm_deallocate(mach_task_self(), (vm_offset_t)buf, count);
	}

	/* Timed sequential read */
	dget_time(&t0);
	lba = 0;
	for (i = 0; i < iters; i++) {
		kr = device_read(ahci_port, 0, (recnum_t)lba,
				 (io_buf_len_t)chunk_size, &buf, &count);
		if (kr != KERN_SUCCESS) {
			printf("  %s: device_read failed at lba=%u kr=%d\n",
			       label, lba, kr);
			return;
		}
		vm_deallocate(mach_task_self(), (vm_offset_t)buf, count);
		lba += sectors_per_chunk;
	}
	dget_time(&t1);

	total_ns = delapsed_ns(&t0, &t1);

	{
		unsigned long us_per_op = (total_ns / iters) / 1000;
		unsigned long us_frac = ((total_ns / iters) % 1000) / 10;
		unsigned long total_us = total_ns / 1000;
		unsigned long mbps = 0, mbps_frac = 0;

		/* MB/s = total_bytes / total_seconds / (1024*1024)
		 *      = total_bytes / total_us * 1000000 / 1048576
		 *      = (total_bytes / 1024) * (1000000 / 1024) / total_us
		 *      ≈ (total_bytes >> 10) * 976 / total_us
		 * x10 for one decimal place */
		if (total_us > 0) {
			unsigned long kb = total_bytes / 1024;
			unsigned long x10 = kb * 9765 / total_us;
			mbps = x10 / 10;
			mbps_frac = x10 % 10;
		}

		printf("  %-28s %5lu.%02lu us/op  "
		       "(%u x %uB, %lu.%lu MB/s)\n",
		       label, us_per_op, us_frac, iters, chunk_size,
		       mbps, mbps_frac);
	}
}

/* ===================================================================
 * Raw AHCI write benchmark
 *
 * Writes `total_bytes` bytes sequentially in `chunk_size`-byte
 * requests via device_write() to ahci_driver.
 * Uses a dedicated range of sectors at the END of the disk to
 * avoid corrupting the ext2 filesystem at the beginning.
 * =================================================================== */

static void
bench_raw_write(const char *label, unsigned int chunk_size,
		unsigned int total_bytes)
{
	tvalspec_t		t0, t1;
	unsigned long		total_ns;
	unsigned int		sectors_per_chunk, total_sectors;
	unsigned int		lba, iters, i;
	kern_return_t		kr;
	vm_offset_t		write_buf;
	io_buf_len_t		written;
	unsigned int		disk_size_sects, write_start;

	/* Query disk size to write at the end (safe area) */
	{
		int st[3];
		mach_msg_type_number_t sc = 3;
		kr = device_get_status(ahci_port, DEV_GET_SIZE,
				       (dev_status_t)st, &sc);
		if (kr != KERN_SUCCESS) {
			printf("  %s: get_status failed kr=%d\n", label, kr);
			return;
		}
		disk_size_sects = (unsigned int)st[0];
	}

	sectors_per_chunk = chunk_size / SECTOR_SIZE;
	total_sectors = total_bytes / SECTOR_SIZE;
	iters = total_sectors / sectors_per_chunk;
	if (iters == 0)
		iters = 1;

	/* Write at end of disk minus a safety margin */
	if (total_sectors + 16 > disk_size_sects) {
		printf("  %s: disk too small (%u sects)\n",
		       label, disk_size_sects);
		return;
	}
	write_start = disk_size_sects - total_sectors - 16;

	/* Allocate write data buffer */
	kr = vm_allocate(mach_task_self(), &write_buf, chunk_size, TRUE);
	if (kr != KERN_SUCCESS) {
		printf("  %s: vm_allocate failed\n", label);
		return;
	}
	/* Fill with a pattern */
	memset((void *)write_buf, 0xA5, chunk_size);

	/* Warmup: 4 writes */
	for (i = 0; i < 4; i++) {
		kr = device_write(ahci_port, 0,
				  (recnum_t)(write_start + i * sectors_per_chunk),
				  (io_buf_ptr_t)write_buf,
				  (mach_msg_type_number_t)chunk_size,
				  &written);
		if (kr != KERN_SUCCESS) {
			printf("  %s: device_write warmup failed kr=%d\n",
			       label, kr);
			vm_deallocate(mach_task_self(), write_buf, chunk_size);
			return;
		}
	}

	/* Timed sequential write */
	dget_time(&t0);
	lba = write_start;
	for (i = 0; i < iters; i++) {
		kr = device_write(ahci_port, 0, (recnum_t)lba,
				  (io_buf_ptr_t)write_buf,
				  (mach_msg_type_number_t)chunk_size,
				  &written);
		if (kr != KERN_SUCCESS) {
			printf("  %s: device_write failed at lba=%u kr=%d\n",
			       label, lba, kr);
			break;
		}
		lba += sectors_per_chunk;
	}
	dget_time(&t1);

	vm_deallocate(mach_task_self(), write_buf, chunk_size);

	total_ns = delapsed_ns(&t0, &t1);

	{
		unsigned long us_per_op = (total_ns / iters) / 1000;
		unsigned long us_frac = ((total_ns / iters) % 1000) / 10;
		unsigned long total_us = total_ns / 1000;
		unsigned long mbps = 0, mbps_frac = 0;

		if (total_us > 0) {
			unsigned long kb = total_bytes / 1024;
			unsigned long x10 = kb * 9765 / total_us;
			mbps = x10 / 10;
			mbps_frac = x10 % 10;
		}

		printf("  %-28s %5lu.%02lu us/op  "
		       "(%u x %uB, %lu.%lu MB/s)\n",
		       label, us_per_op, us_frac, iters, chunk_size,
		       mbps, mbps_frac);
	}
}

/* ===================================================================
 * ext2 file read benchmark
 *
 * Opens a file via ext2_open, reads it in one or more chunks via
 * ext2_read, then closes via ext2_close.  Measures the full
 * open-read-close cycle.
 * =================================================================== */

static void
bench_ext2_file_read(const char *label, char *filename,
		     unsigned int read_chunk)
{
	tvalspec_t	t0, t1;
	unsigned long	total_ns;
	kern_return_t	kr;
	natural_t	fid, file_size;
	unsigned int	offset, bytes_read;
	pointer_t	data;
	mach_msg_type_number_t data_count;
	int		reads;

	/* Open */
	kr = ext2_open(ext2_port, filename, &fid);
	if (kr != KERN_SUCCESS) {
		printf("  %s: ext2_open(\"%s\") failed kr=%d\n",
		       label, filename, kr);
		return;
	}

	/* Stat */
	kr = ext2_stat(ext2_port, fid, &file_size);
	if (kr != KERN_SUCCESS) {
		printf("  %s: ext2_stat failed kr=%d\n", label, kr);
		ext2_close(ext2_port, fid);
		return;
	}

	if (file_size == 0) {
		printf("  %s: file is empty\n", label);
		ext2_close(ext2_port, fid);
		return;
	}

	/* Warmup: read the file once */
	offset = 0;
	while (offset < file_size) {
		unsigned int want = file_size - offset;
		if (want > read_chunk)
			want = read_chunk;
		kr = ext2_read(ext2_port, fid, offset, want,
			       &data, &data_count);
		if (kr != KERN_SUCCESS || data_count == 0)
			break;
		vm_deallocate(mach_task_self(), data, data_count);
		offset += data_count;
	}

	/* Timed run: re-read the entire file */
	dget_time(&t0);
	offset = 0;
	bytes_read = 0;
	reads = 0;
	while (offset < file_size) {
		unsigned int want = file_size - offset;
		if (want > read_chunk)
			want = read_chunk;
		kr = ext2_read(ext2_port, fid, offset, want,
			       &data, &data_count);
		if (kr != KERN_SUCCESS || data_count == 0)
			break;
		vm_deallocate(mach_task_self(), data, data_count);
		offset += data_count;
		bytes_read += data_count;
		reads++;
	}
	dget_time(&t1);

	ext2_close(ext2_port, fid);

	total_ns = delapsed_ns(&t0, &t1);

	{
		unsigned long us_total = total_ns / 1000;
		unsigned long us_per_read = reads ? (total_ns / reads) / 1000 : 0;
		unsigned long us_frac = reads ?
			((total_ns / reads) % 1000) / 10 : 0;

		printf("  %-28s %5lu.%02lu us/read  "
		       "(%u bytes, %d reads, %lu us total)\n",
		       label, us_per_read, us_frac,
		       bytes_read, reads, us_total);
	}
}

/* ===================================================================
 * ext2 write correctness test
 *
 * Opens hello.txt, saves original content, writes a test pattern,
 * reads back and verifies, then restores the original content and
 * syncs to disk.  This is a functional test, not a benchmark.
 * =================================================================== */

static void
test_ext2_write_verify(void)
{
	kern_return_t	kr;
	natural_t	fid, file_size;
	pointer_t	orig_data;
	mach_msg_type_number_t orig_count;
	vm_offset_t	write_buf, read_data;
	mach_msg_type_number_t read_count;
	unsigned int	i, errors;

	printf("  ext2 write test:\n");

	/* Open hello.txt */
	kr = ext2_open(ext2_port, "hello.txt", &fid);
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: ext2_open failed kr=%d\n", kr);
		return;
	}

	/* Get file size */
	kr = ext2_stat(ext2_port, fid, &file_size);
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: ext2_stat failed kr=%d\n", kr);
		ext2_close(ext2_port, fid);
		return;
	}
	printf("    file size = %u bytes\n", file_size);

	if (file_size == 0) {
		printf("    FAIL: file is empty\n");
		ext2_close(ext2_port, fid);
		return;
	}

	/* Save original content */
	kr = ext2_read(ext2_port, fid, 0, file_size,
		       &orig_data, &orig_count);
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: ext2_read (save original) failed kr=%d\n",
		       kr);
		ext2_close(ext2_port, fid);
		return;
	}

	/* Prepare test pattern */
	kr = vm_allocate(mach_task_self(), &write_buf,
			 (vm_size_t)file_size, TRUE);
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: vm_allocate failed\n");
		vm_deallocate(mach_task_self(), orig_data, orig_count);
		ext2_close(ext2_port, fid);
		return;
	}
	for (i = 0; i < file_size; i++)
		((char *)write_buf)[i] = (char)(0x41 + (i % 26)); /* A-Z */

	/* Write test pattern */
	kr = ext2_write(ext2_port, fid, 0,
			write_buf, (mach_msg_type_number_t)file_size);
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: ext2_write failed kr=%d\n", kr);
		vm_deallocate(mach_task_self(), write_buf,
			      (vm_size_t)file_size);
		vm_deallocate(mach_task_self(), orig_data, orig_count);
		ext2_close(ext2_port, fid);
		return;
	}
	printf("    write %u bytes OK\n", file_size);

	/* Read back and verify */
	kr = ext2_read(ext2_port, fid, 0, file_size,
		       &read_data, &read_count);
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: ext2_read (readback) failed kr=%d\n", kr);
		vm_deallocate(mach_task_self(), write_buf,
			      (vm_size_t)file_size);
		vm_deallocate(mach_task_self(), orig_data, orig_count);
		ext2_close(ext2_port, fid);
		return;
	}

	errors = 0;
	for (i = 0; i < file_size && i < read_count; i++) {
		if (((char *)read_data)[i] != ((char *)write_buf)[i])
			errors++;
	}

	if (errors == 0 && read_count == file_size)
		printf("    readback verify OK (%u bytes match)\n", file_size);
	else
		printf("    FAIL: readback mismatch (%u errors, "
		       "got %u bytes expected %u)\n",
		       errors, read_count, file_size);

	vm_deallocate(mach_task_self(), read_data, read_count);

	/* Restore original content */
	kr = ext2_write(ext2_port, fid, 0,
			orig_data, (mach_msg_type_number_t)orig_count);
	if (kr != KERN_SUCCESS)
		printf("    WARNING: restore write failed kr=%d\n", kr);

	/* Sync to disk */
	kr = ext2_sync(ext2_port);
	if (kr != KERN_SUCCESS)
		printf("    WARNING: ext2_sync failed kr=%d\n", kr);
	else
		printf("    sync OK (restored original)\n");

	vm_deallocate(mach_task_self(), write_buf, (vm_size_t)file_size);
	vm_deallocate(mach_task_self(), orig_data, orig_count);
	ext2_close(ext2_port, fid);
}

/* ===================================================================
 * ext2 write latency benchmark
 *
 * Writes a fixed-size buffer to offset 0 of hello.txt repeatedly,
 * then restores the original content and syncs.
 * =================================================================== */

#define WRITE_BENCH_ITERS	50

static void
bench_ext2_write(const char *label, char *filename)
{
	tvalspec_t	t0, t1;
	unsigned long	total_ns;
	kern_return_t	kr;
	natural_t	fid, file_size;
	pointer_t	orig_data;
	mach_msg_type_number_t orig_count;
	vm_offset_t	write_buf;
	int		i;

	/* Open */
	kr = ext2_open(ext2_port, filename, &fid);
	if (kr != KERN_SUCCESS) {
		printf("  %s: ext2_open(\"%s\") failed kr=%d\n",
		       label, filename, kr);
		return;
	}

	/* Get file size */
	kr = ext2_stat(ext2_port, fid, &file_size);
	if (kr != KERN_SUCCESS || file_size == 0) {
		printf("  %s: ext2_stat failed or empty\n", label);
		ext2_close(ext2_port, fid);
		return;
	}

	/* Save original */
	kr = ext2_read(ext2_port, fid, 0, file_size,
		       &orig_data, &orig_count);
	if (kr != KERN_SUCCESS) {
		printf("  %s: save original failed kr=%d\n", label, kr);
		ext2_close(ext2_port, fid);
		return;
	}

	/* Allocate write buffer */
	kr = vm_allocate(mach_task_self(), &write_buf,
			 (vm_size_t)file_size, TRUE);
	if (kr != KERN_SUCCESS) {
		vm_deallocate(mach_task_self(), orig_data, orig_count);
		ext2_close(ext2_port, fid);
		return;
	}
	memset((void *)write_buf, 0xBB, file_size);

	/* Warmup: 4 writes */
	for (i = 0; i < 4; i++) {
		ext2_write(ext2_port, fid, 0,
			   write_buf, (mach_msg_type_number_t)file_size);
	}

	/* Timed writes */
	dget_time(&t0);
	for (i = 0; i < WRITE_BENCH_ITERS; i++) {
		kr = ext2_write(ext2_port, fid, 0,
				write_buf,
				(mach_msg_type_number_t)file_size);
		if (kr != KERN_SUCCESS)
			break;
	}
	dget_time(&t1);

	/* Restore original and sync */
	ext2_write(ext2_port, fid, 0,
		   orig_data, (mach_msg_type_number_t)orig_count);
	ext2_sync(ext2_port);

	vm_deallocate(mach_task_self(), write_buf, (vm_size_t)file_size);
	vm_deallocate(mach_task_self(), orig_data, orig_count);
	ext2_close(ext2_port, fid);

	total_ns = delapsed_ns(&t0, &t1);

	{
		unsigned long us_per_op = (total_ns / i) / 1000;
		unsigned long us_frac = ((total_ns / i) % 1000) / 10;
		unsigned long total_us = total_ns / 1000;

		printf("  %-28s %5lu.%02lu us/write "
		       "(%u bytes, %d writes, %lu us total)\n",
		       label, us_per_op, us_frac,
		       file_size, i, total_us);
	}
}

/* ===================================================================
 * ext2 sync latency benchmark
 * =================================================================== */

#define SYNC_BENCH_ITERS	20

static void
bench_ext2_sync(const char *label)
{
	tvalspec_t	t0, t1;
	unsigned long	total_ns;
	kern_return_t	kr;
	int		i;

	/* Warmup */
	for (i = 0; i < 4; i++)
		ext2_sync(ext2_port);

	/* Timed syncs */
	dget_time(&t0);
	for (i = 0; i < SYNC_BENCH_ITERS; i++) {
		kr = ext2_sync(ext2_port);
		if (kr != KERN_SUCCESS)
			break;
	}
	dget_time(&t1);

	total_ns = delapsed_ns(&t0, &t1);

	{
		unsigned long us_per_op = (total_ns / i) / 1000;
		unsigned long us_frac = ((total_ns / i) % 1000) / 10;
		unsigned long total_us = total_ns / 1000;

		printf("  %-28s %5lu.%02lu us/sync  (%d iters, %lu us total)\n",
		       label, us_per_op, us_frac, i, total_us);
	}
}

/* ===================================================================
 * ext2 write+sync benchmark (end-to-end with disk persistence)
 *
 * Each iteration does write + sync, measuring the real cost of a
 * durable write: IPC + page cache + metadata writeback + disk flush.
 * =================================================================== */

#define WRITE_SYNC_ITERS	20

static void
bench_ext2_write_sync(const char *label, char *filename)
{
	tvalspec_t	t0, t1;
	unsigned long	total_ns;
	kern_return_t	kr;
	natural_t	fid, file_size;
	pointer_t	orig_data;
	mach_msg_type_number_t orig_count;
	vm_offset_t	write_buf;
	int		i;

	/* Open */
	kr = ext2_open(ext2_port, filename, &fid);
	if (kr != KERN_SUCCESS) {
		printf("  %s: ext2_open failed kr=%d\n", label, kr);
		return;
	}

	kr = ext2_stat(ext2_port, fid, &file_size);
	if (kr != KERN_SUCCESS || file_size == 0) {
		printf("  %s: ext2_stat failed or empty\n", label);
		ext2_close(ext2_port, fid);
		return;
	}

	/* Save original */
	kr = ext2_read(ext2_port, fid, 0, file_size,
		       &orig_data, &orig_count);
	if (kr != KERN_SUCCESS) {
		ext2_close(ext2_port, fid);
		return;
	}

	kr = vm_allocate(mach_task_self(), &write_buf,
			 (vm_size_t)file_size, TRUE);
	if (kr != KERN_SUCCESS) {
		vm_deallocate(mach_task_self(), orig_data, orig_count);
		ext2_close(ext2_port, fid);
		return;
	}
	memset((void *)write_buf, 0xCC, file_size);

	/* Warmup: 2 write+sync */
	for (i = 0; i < 2; i++) {
		ext2_write(ext2_port, fid, 0,
			   write_buf, (mach_msg_type_number_t)file_size);
		ext2_sync(ext2_port);
	}

	/* Timed write+sync */
	dget_time(&t0);
	for (i = 0; i < WRITE_SYNC_ITERS; i++) {
		kr = ext2_write(ext2_port, fid, 0,
				write_buf,
				(mach_msg_type_number_t)file_size);
		if (kr != KERN_SUCCESS)
			break;
		kr = ext2_sync(ext2_port);
		if (kr != KERN_SUCCESS)
			break;
	}
	dget_time(&t1);

	/* Restore and sync */
	ext2_write(ext2_port, fid, 0,
		   orig_data, (mach_msg_type_number_t)orig_count);
	ext2_sync(ext2_port);

	vm_deallocate(mach_task_self(), write_buf, (vm_size_t)file_size);
	vm_deallocate(mach_task_self(), orig_data, orig_count);
	ext2_close(ext2_port, fid);

	total_ns = delapsed_ns(&t0, &t1);

	{
		unsigned long us_per_op = (total_ns / i) / 1000;
		unsigned long us_frac = ((total_ns / i) % 1000) / 10;
		unsigned long total_us = total_ns / 1000;

		printf("  %-28s %5lu.%02lu us/op   "
		       "(%u bytes, %d iters, %lu us total)\n",
		       label, us_per_op, us_frac,
		       file_size, i, total_us);
	}
}

/* ===================================================================
 * ext2 open/close latency benchmark
 *
 * Measures the cost of ext2_open + ext2_close for a small file.
 * This exercises the full IPC round-trip twice (open + close) plus
 * the ext2 superblock/inode/directory lookup.
 * =================================================================== */

#define OPEN_CLOSE_ITERS	100

static void
bench_ext2_open_close(const char *label, char *filename)
{
	tvalspec_t	t0, t1;
	unsigned long	total_ns;
	kern_return_t	kr;
	natural_t	fid;
	int		i;

	/* Warmup */
	for (i = 0; i < 4; i++) {
		kr = ext2_open(ext2_port, filename, &fid);
		if (kr != KERN_SUCCESS) {
			printf("  %s: ext2_open(\"%s\") failed kr=%d\n",
			       label, filename, kr);
			return;
		}
		ext2_close(ext2_port, fid);
	}

	/* Timed run */
	dget_time(&t0);
	for (i = 0; i < OPEN_CLOSE_ITERS; i++) {
		kr = ext2_open(ext2_port, filename, &fid);
		if (kr != KERN_SUCCESS)
			break;
		ext2_close(ext2_port, fid);
	}
	dget_time(&t1);

	total_ns = delapsed_ns(&t0, &t1);

	{
		unsigned long us_per_op = (total_ns / i) / 1000;
		unsigned long us_frac = ((total_ns / i) % 1000) / 10;
		unsigned long total_us = total_ns / 1000;

		printf("  %-28s %5lu.%02lu us/op  (%d iters, %lu us total)\n",
		       label, us_per_op, us_frac, i, total_us);
	}
}

/* ===================================================================
 * Public entry point
 * =================================================================== */

void
bench_disk_run(mach_port_t host_port, mach_port_t clock)
{
	kern_return_t kr;
	int have_ahci = 0, have_ext2 = 0;

	bench_clock_port = clock;

	/* Discover servers via netname */
	kr = netname_look_up(name_server_port, "", "ahci_driver", &ahci_port);
	if (kr == KERN_SUCCESS) {
		have_ahci = 1;
	}

	kr = netname_look_up(name_server_port, "", "ext2_server", &ext2_port);
	if (kr == KERN_SUCCESS) {
		have_ext2 = 1;
	}

	if (!have_ahci && !have_ext2) {
		printf("--- Disk I/O benchmark: skipped "
		       "(no ahci_driver or ext2_server) ---\n");
		return;
	}

	printf("--- Disk I/O benchmark ---\n");

	if (have_ahci) {
		printf("  ahci_driver: port=%d\n", ahci_port);

		/* Sequential raw reads at different chunk sizes */
		bench_raw_read("raw 1 KB seq read",   1024,  64 * 1024);
		bench_raw_read("raw 4 KB seq read",   4096,  64 * 1024);
		bench_raw_read("raw 64 KB seq read", 65536, 256 * 1024);

		/* Sequential raw writes at different chunk sizes */
		bench_raw_write("raw 1 KB seq write",   1024,  64 * 1024);
		bench_raw_write("raw 4 KB seq write",   4096,  64 * 1024);
		bench_raw_write("raw 64 KB seq write", 65536, 256 * 1024);
	}

	if (have_ext2) {
		printf("  ext2_server: port=%d\n", ext2_port);

		/* File operations */
		bench_ext2_open_close("ext2 open+close", "hello.txt");
		bench_ext2_file_read("ext2 read (hello.txt)", "hello.txt",
				     4096);

		/* Write correctness test */
		test_ext2_write_verify();

		/* Write benchmarks */
		bench_ext2_write("ext2 write (cached)", "hello.txt");
		bench_ext2_sync("ext2 sync (clean)");
		bench_ext2_write_sync("ext2 write+sync (durable)",
				      "hello.txt");
	}

	if (have_ahci && have_ext2) {
		printf("  (ext2 reads go: ipc_bench -> ext2_server "
		       "-> ahci_driver -> AHCI HW)\n");
	}

	printf("\n");
}
