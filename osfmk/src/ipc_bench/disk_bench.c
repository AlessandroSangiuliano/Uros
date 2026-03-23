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
 * Dirty list sync benchmark
 *
 * Measures sync latency with 2 files open: both clean vs one dirty.
 * Shows dirty list efficiency — only dirty files are flushed.
 * =================================================================== */

static void
bench_dirty_list_sync(void)
{
	kern_return_t	kr;
	natural_t	fid1, fid2;
	tvalspec_t	t0, t1;
	unsigned long	total_ns;
	vm_offset_t	buf;
	int		i;

	kr = ext2_open(ext2_port, "hello.txt", &fid1);
	if (kr != KERN_SUCCESS) return;
	kr = ext2_open(ext2_port, "bench.dat", &fid2);
	if (kr != KERN_SUCCESS) {
		ext2_close(ext2_port, fid1);
		return;
	}

	/* Both clean — sync iterates empty dirty list */
	ext2_sync(ext2_port);  /* warmup */
	dget_time(&t0);
	for (i = 0; i < SYNC_BENCH_ITERS; i++)
		ext2_sync(ext2_port);
	dget_time(&t1);
	total_ns = delapsed_ns(&t0, &t1);
	{
		unsigned long us_per = (total_ns / SYNC_BENCH_ITERS) / 1000;
		unsigned long us_fr = ((total_ns / SYNC_BENCH_ITERS) % 1000) / 10;
		unsigned long total_us = total_ns / 1000;
		printf("  %-28s %5lu.%02lu us/sync  (%d iters, %lu us total)\n",
		       "sync (2 open, 0 dirty)",
		       us_per, us_fr, SYNC_BENCH_ITERS, total_us);
	}

	/* Write one file, sync — dirty list has 1 entry */
	kr = vm_allocate(mach_task_self(), &buf, 4, TRUE);
	if (kr == KERN_SUCCESS) {
		((char *)buf)[0] = 'T';
		((char *)buf)[1] = 'E';
		((char *)buf)[2] = 'S';
		((char *)buf)[3] = 'T';
	}

	/* warmup: write + sync cycle */
	for (i = 0; i < 4; i++) {
		ext2_write(ext2_port, fid1, 0, (pointer_t)buf, 4);
		ext2_sync(ext2_port);
	}

	dget_time(&t0);
	for (i = 0; i < SYNC_BENCH_ITERS; i++) {
		ext2_write(ext2_port, fid1, 0, (pointer_t)buf, 4);
		ext2_sync(ext2_port);
	}
	dget_time(&t1);
	vm_deallocate(mach_task_self(), buf, 4);
	total_ns = delapsed_ns(&t0, &t1);
	{
		unsigned long us_per = (total_ns / SYNC_BENCH_ITERS) / 1000;
		unsigned long us_fr = ((total_ns / SYNC_BENCH_ITERS) % 1000) / 10;
		unsigned long total_us = total_ns / 1000;
		printf("  %-28s %5lu.%02lu us/op   (%d iters, %lu us total)\n",
		       "write+sync (2 open, 1 dirty)",
		       us_per, us_fr, SYNC_BENCH_ITERS, total_us);
	}

	ext2_close(ext2_port, fid1);
	ext2_close(ext2_port, fid2);
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
 * ext2 merge-sync benchmark
 *
 * Writes a large file in 1 KB chunks (creating many dirty cache blocks),
 * then syncs.  The page cache request merging should combine contiguous
 * dirty blocks into fewer device_write calls.
 * =================================================================== */

#define MERGE_WRITE_SIZE	(64 * 1024)	/* 64 KB of 1 KB writes */

static void
bench_ext2_merge_sync(const char *label, char *filename)
{
	tvalspec_t	t0, t1;
	unsigned long	total_ns;
	kern_return_t	kr;
	natural_t	fid, file_size;
	pointer_t	orig_data;
	mach_msg_type_number_t orig_count;
	vm_offset_t	write_buf;
	unsigned int	write_total;
	unsigned int	off;

	kr = ext2_open(ext2_port, filename, &fid);
	if (kr != KERN_SUCCESS) {
		printf("  %s: open failed kr=%d\n", label, kr);
		return;
	}

	kr = ext2_stat(ext2_port, fid, &file_size);
	if (kr != KERN_SUCCESS || file_size == 0) {
		printf("  %s: stat failed or empty\n", label);
		ext2_close(ext2_port, fid);
		return;
	}

	write_total = MERGE_WRITE_SIZE;
	if (write_total > file_size)
		write_total = file_size;

	/* Save original data for restore */
	kr = ext2_read(ext2_port, fid, 0, write_total,
		       &orig_data, &orig_count);
	if (kr != KERN_SUCCESS) {
		printf("  %s: read %u bytes failed kr=%d\n",
		       label, write_total, kr);
		ext2_close(ext2_port, fid);
		return;
	}

	/* Prepare 1 KB write buffer */
	kr = vm_allocate(mach_task_self(), &write_buf, 1024, TRUE);
	if (kr != KERN_SUCCESS) {
		printf("  %s: vm_allocate failed kr=%d\n", label, kr);
		vm_deallocate(mach_task_self(), orig_data, orig_count);
		ext2_close(ext2_port, fid);
		return;
	}
	memset((void *)write_buf, 0xDD, 1024);

	/* Write 1 KB chunks across the file — dirtying many cache blocks */
	for (off = 0; off < write_total; off += 1024) {
		kr = ext2_write(ext2_port, fid, off,
				write_buf, 1024);
		if (kr != KERN_SUCCESS) {
			printf("  %s: write at %u failed kr=%d\n",
			       label, off, kr);
			break;
		}
	}

	/* Timed sync — this is where merging happens */
	printf("  %s: %u dirty blocks, syncing...\n",
	       label, write_total / 1024);
	dget_time(&t0);
	kr = ext2_sync(ext2_port);
	dget_time(&t1);

	/* Restore original and sync */
	ext2_write(ext2_port, fid, 0,
		   orig_data, (mach_msg_type_number_t)orig_count);
	ext2_sync(ext2_port);

	vm_deallocate(mach_task_self(), write_buf, 1024);
	vm_deallocate(mach_task_self(), orig_data, orig_count);
	ext2_close(ext2_port, fid);

	total_ns = delapsed_ns(&t0, &t1);

	{
		unsigned long us = total_ns / 1000;
		unsigned long us_frac = (total_ns % 1000) / 10;

		printf("  %-28s %5lu.%02lu us "
		       "(%u blocks)\n",
		       label, us, us_frac,
		       write_total / 1024);
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
 * ext2 write stress tests
 *
 * Exercises edge cases and resilience of the write path:
 *  1. Overwrite storm — same offset N times, verify last value
 *  2. Partial block write — write at mid-block offset
 *  3. Cross-block write — write spanning two blocks
 *  4. File extension — grow file beyond original size into indirect
 *  5. Persist across close — write, close, reopen, verify
 *  6. Write burst + sync — many writes then sync, verify all
 * =================================================================== */

static int
verify_pattern(mach_port_t port, natural_t fid,
	       unsigned int offset, unsigned int size,
	       unsigned char expected)
{
	pointer_t data;
	mach_msg_type_number_t count;
	kern_return_t kr;
	unsigned int i, errors = 0;

	kr = ext2_read(port, fid, offset, size, &data, &count);
	if (kr != KERN_SUCCESS) {
		printf("      verify read failed kr=%d\n", kr);
		return -1;
	}
	for (i = 0; i < count && i < size; i++) {
		if (((unsigned char *)data)[i] != expected)
			errors++;
	}
	vm_deallocate(mach_task_self(), data, count);
	if (errors || count != size) {
		printf("      FAIL: %u/%u errors (got %u bytes)\n",
		       errors, size, count);
		return -1;
	}
	return 0;
}

static void
stress_ext2_write(void)
{
	kern_return_t	kr;
	natural_t	fid, file_size;
	pointer_t	orig_data;
	mach_msg_type_number_t orig_count;
	vm_offset_t	buf;
	unsigned int	i, pass, fail;

	printf("  ext2 write stress tests:\n");

	/* Open and save original */
	kr = ext2_open(ext2_port, "hello.txt", &fid);
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: ext2_open failed kr=%d\n", kr);
		return;
	}
	ext2_stat(ext2_port, fid, &file_size);
	kr = ext2_read(ext2_port, fid, 0, file_size,
		       &orig_data, &orig_count);
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: save original failed\n");
		ext2_close(ext2_port, fid);
		return;
	}

	pass = 0;
	fail = 0;

	/* --- Test 1: Overwrite storm ---
	 * Write 100 times at offset 0, each with a different byte.
	 * Only the last value should remain. */
	{
		kr = vm_allocate(mach_task_self(), &buf, file_size, TRUE);
		if (kr != KERN_SUCCESS) goto restore;

		for (i = 0; i < 100; i++) {
			memset((void *)buf, (unsigned char)(i + 1), file_size);
			kr = ext2_write(ext2_port, fid, 0, buf,
					(mach_msg_type_number_t)file_size);
			if (kr != KERN_SUCCESS) {
				printf("    1-overwrite: FAIL write %u kr=%d\n",
				       i, kr);
				vm_deallocate(mach_task_self(), buf, file_size);
				fail++;
				goto test2;
			}
		}
		vm_deallocate(mach_task_self(), buf, file_size);

		if (verify_pattern(ext2_port, fid, 0, file_size, 100) == 0) {
			printf("    1-overwrite-storm:   PASS (100 writes)\n");
			pass++;
		} else {
			printf("    1-overwrite-storm:   FAIL\n");
			fail++;
		}
	}

test2:
	/* --- Test 2: Partial block write ---
	 * Write 10 bytes at offset 5 (mid-block), verify surrounding
	 * data is unchanged. */
	{
		unsigned char pat = 0xEE;
		pointer_t rdata;
		mach_msg_type_number_t rcount;
		int ok = 1;

		/* First fill the whole file with 0xAA */
		kr = vm_allocate(mach_task_self(), &buf, file_size, TRUE);
		if (kr != KERN_SUCCESS) goto restore;
		memset((void *)buf, 0xAA, file_size);
		ext2_write(ext2_port, fid, 0, buf,
			   (mach_msg_type_number_t)file_size);
		vm_deallocate(mach_task_self(), buf, file_size);

		/* Write 10 bytes of 0xEE at offset 5 */
		kr = vm_allocate(mach_task_self(), &buf, 10, TRUE);
		if (kr != KERN_SUCCESS) goto restore;
		memset((void *)buf, pat, 10);
		ext2_write(ext2_port, fid, 5, buf, 10);
		vm_deallocate(mach_task_self(), buf, 10);

		/* Read whole file and check */
		kr = ext2_read(ext2_port, fid, 0, file_size,
			       &rdata, &rcount);
		if (kr != KERN_SUCCESS) {
			printf("    2-partial-block:     FAIL read kr=%d\n",
			       kr);
			fail++;
			goto test3;
		}
		for (i = 0; i < rcount; i++) {
			unsigned char expected = (i >= 5 && i < 15) ?
				pat : 0xAA;
			if (((unsigned char *)rdata)[i] != expected) {
				ok = 0;
				break;
			}
		}
		vm_deallocate(mach_task_self(), rdata, rcount);
		if (ok) {
			printf("    2-partial-block:     PASS\n");
			pass++;
		} else {
			printf("    2-partial-block:     FAIL at byte %u\n",
			       i);
			fail++;
		}
	}

test3:
	/* --- Test 3: Cross-block write ---
	 * Extend file to 2048 bytes (2 blocks @ 1024), write 128 bytes
	 * straddling the block boundary (offset 960..1088). */
	{
		unsigned int big = 2048;
		unsigned int woff = 960, wlen = 128;
		pointer_t rdata;
		mach_msg_type_number_t rcount;
		int ok = 1;

		/* Fill 2048 bytes with 0x11 */
		kr = vm_allocate(mach_task_self(), &buf, big, TRUE);
		if (kr != KERN_SUCCESS) goto restore;
		memset((void *)buf, 0x11, big);
		kr = ext2_write(ext2_port, fid, 0, buf,
				(mach_msg_type_number_t)big);
		vm_deallocate(mach_task_self(), buf, big);
		if (kr != KERN_SUCCESS) {
			printf("    3-cross-block:       FAIL extend kr=%d\n",
			       kr);
			fail++;
			goto test4;
		}

		/* Write 0xFF across block boundary */
		kr = vm_allocate(mach_task_self(), &buf, wlen, TRUE);
		if (kr != KERN_SUCCESS) goto restore;
		memset((void *)buf, 0xFF, wlen);
		kr = ext2_write(ext2_port, fid, woff, buf,
				(mach_msg_type_number_t)wlen);
		vm_deallocate(mach_task_self(), buf, wlen);
		if (kr != KERN_SUCCESS) {
			printf("    3-cross-block:       FAIL write kr=%d\n",
			       kr);
			fail++;
			goto test4;
		}

		/* Verify: 0..959 = 0x11, 960..1087 = 0xFF, 1088..2047 = 0x11 */
		kr = ext2_read(ext2_port, fid, 0, big, &rdata, &rcount);
		if (kr != KERN_SUCCESS || rcount != big) {
			printf("    3-cross-block:       FAIL read kr=%d\n",
			       kr);
			if (kr == KERN_SUCCESS)
				vm_deallocate(mach_task_self(), rdata, rcount);
			fail++;
			goto test4;
		}
		for (i = 0; i < big; i++) {
			unsigned char expected =
				(i >= woff && i < woff + wlen) ? 0xFF : 0x11;
			if (((unsigned char *)rdata)[i] != expected) {
				ok = 0;
				break;
			}
		}
		vm_deallocate(mach_task_self(), rdata, rcount);
		if (ok) {
			printf("    3-cross-block:       PASS (960..1088)\n");
			pass++;
		} else {
			printf("    3-cross-block:       FAIL at byte %u\n",
			       i);
			fail++;
		}
	}

test4:
	/* --- Test 4: File extension to indirect blocks ---
	 * Grow file to 14 KB (>12 direct blocks), verify data. */
	{
		unsigned int big = 14 * 1024;
		int ok = 1;
		pointer_t rdata;
		mach_msg_type_number_t rcount;

		kr = vm_allocate(mach_task_self(), &buf, big, TRUE);
		if (kr != KERN_SUCCESS) goto restore;
		for (i = 0; i < big; i++)
			((unsigned char *)buf)[i] = (unsigned char)(i & 0xFF);
		kr = ext2_write(ext2_port, fid, 0, buf,
				(mach_msg_type_number_t)big);
		if (kr != KERN_SUCCESS) {
			printf("    4-indirect-extend:   FAIL write kr=%d\n",
			       kr);
			vm_deallocate(mach_task_self(), buf, big);
			fail++;
			goto test5;
		}

		/* Sync to disk */
		ext2_sync(ext2_port);

		/* Read back and verify */
		kr = ext2_read(ext2_port, fid, 0, big, &rdata, &rcount);
		if (kr != KERN_SUCCESS || rcount != big) {
			printf("    4-indirect-extend:   FAIL read kr=%d "
			       "count=%u\n", kr, rcount);
			if (kr == KERN_SUCCESS)
				vm_deallocate(mach_task_self(), rdata, rcount);
			vm_deallocate(mach_task_self(), buf, big);
			fail++;
			goto test5;
		}
		for (i = 0; i < big; i++) {
			if (((unsigned char *)rdata)[i] !=
			    ((unsigned char *)buf)[i]) {
				ok = 0;
				break;
			}
		}
		vm_deallocate(mach_task_self(), rdata, rcount);
		vm_deallocate(mach_task_self(), buf, big);
		if (ok) {
			printf("    4-indirect-extend:   PASS (14 KB)\n");
			pass++;
		} else {
			printf("    4-indirect-extend:   FAIL at byte %u\n",
			       i);
			fail++;
		}
	}

test5:
	/* --- Test 5: Persist across close/reopen ---
	 * Write pattern, sync, close, reopen, verify. */
	{
		unsigned int sz = 64;
		int ok;

		kr = vm_allocate(mach_task_self(), &buf, sz, TRUE);
		if (kr != KERN_SUCCESS) goto restore;
		memset((void *)buf, 0x55, sz);
		ext2_write(ext2_port, fid, 0, buf, (mach_msg_type_number_t)sz);
		vm_deallocate(mach_task_self(), buf, sz);
		ext2_sync(ext2_port);
		ext2_close(ext2_port, fid);

		/* Reopen */
		kr = ext2_open(ext2_port, "hello.txt", &fid);
		if (kr != KERN_SUCCESS) {
			printf("    5-persist-reopen:    FAIL reopen kr=%d\n",
			       kr);
			fail++;
			/* Can't continue — fid invalid */
			printf("  stress: %u PASS, %u FAIL\n", pass, fail);
			vm_deallocate(mach_task_self(), orig_data, orig_count);
			return;
		}

		ok = (verify_pattern(ext2_port, fid, 0, sz, 0x55) == 0);
		if (ok) {
			printf("    5-persist-reopen:    PASS\n");
			pass++;
		} else {
			printf("    5-persist-reopen:    FAIL\n");
			fail++;
		}
	}

	/* --- Test 6: Write burst + sync ---
	 * 200 sequential 32-byte writes at different offsets, sync,
	 * then verify all. */
	{
		unsigned int chunk = 32;
		unsigned int total = 200 * chunk; /* 6400 bytes */
		int ok = 1;
		pointer_t rdata;
		mach_msg_type_number_t rcount;

		for (i = 0; i < 200; i++) {
			unsigned char val = (unsigned char)(i + 1);
			kr = vm_allocate(mach_task_self(), &buf, chunk, TRUE);
			if (kr != KERN_SUCCESS) goto restore;
			memset((void *)buf, val, chunk);
			kr = ext2_write(ext2_port, fid, i * chunk, buf,
					(mach_msg_type_number_t)chunk);
			vm_deallocate(mach_task_self(), buf, chunk);
			if (kr != KERN_SUCCESS) {
				printf("    6-burst+sync:        FAIL write "
				       "%u kr=%d\n", i, kr);
				fail++;
				goto restore;
			}
		}

		ext2_sync(ext2_port);

		/* Verify all 200 chunks */
		kr = ext2_read(ext2_port, fid, 0, total, &rdata, &rcount);
		if (kr != KERN_SUCCESS || rcount != total) {
			printf("    6-burst+sync:        FAIL read kr=%d "
			       "count=%u\n", kr, rcount);
			if (kr == KERN_SUCCESS)
				vm_deallocate(mach_task_self(), rdata, rcount);
			fail++;
			goto restore;
		}
		for (i = 0; i < 200 && ok; i++) {
			unsigned char expected = (unsigned char)(i + 1);
			unsigned int j;
			for (j = 0; j < chunk; j++) {
				if (((unsigned char *)rdata)[i * chunk + j] !=
				    expected) {
					ok = 0;
					printf("    6-burst+sync:        FAIL "
					       "chunk %u byte %u "
					       "(got 0x%02x want 0x%02x)\n",
					       i, j,
					       ((unsigned char *)rdata)
						   [i * chunk + j],
					       expected);
					break;
				}
			}
		}
		vm_deallocate(mach_task_self(), rdata, rcount);
		if (ok) {
			printf("    6-burst+sync:        PASS "
			       "(200 x 32B)\n");
			pass++;
		} else {
			fail++;
		}
	}

restore:
	/* Restore original content and size */
	ext2_write(ext2_port, fid, 0, orig_data,
		   (mach_msg_type_number_t)orig_count);
	ext2_sync(ext2_port);
	vm_deallocate(mach_task_self(), orig_data, orig_count);
	ext2_close(ext2_port, fid);

	printf("  stress: %u PASS, %u FAIL\n", pass, fail);
}

/* ===================================================================
 * Dirty list test
 *
 * Opens multiple files, writes to all of them (making them dirty),
 * then syncs and verifies all writes persisted.  Exercises the
 * server-side dirty inode list with multiple entries.
 * =================================================================== */

static void
test_dirty_list(void)
{
	kern_return_t	kr;
	natural_t	fid1, fid2;
	natural_t	size1, size2;
	pointer_t	orig1, orig2;
	mach_msg_type_number_t orig1_cnt, orig2_cnt;
	pointer_t	rdata;
	mach_msg_type_number_t rcount;
	vm_offset_t	buf;
	unsigned int	pass = 0, fail = 0;
	const char	*pattern_a = "DIRTY_A_TEST_12345";
	const char	*pattern_b = "DIRTY_B_TEST_67890";
	unsigned int	len_a = 18, len_b = 18;

	printf("  dirty list test:\n");

	/* Open both files */
	kr = ext2_open(ext2_port, "hello.txt", &fid1);
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: open hello.txt kr=%d\n", kr);
		return;
	}
	kr = ext2_open(ext2_port, "bench.dat", &fid2);
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: open bench.dat kr=%d\n", kr);
		ext2_close(ext2_port, fid1);
		return;
	}

	/* Save originals */
	ext2_stat(ext2_port, fid1, &size1);
	kr = ext2_read(ext2_port, fid1, 0, size1, &orig1, &orig1_cnt);
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: read hello.txt kr=%d\n", kr);
		goto close_both;
	}

	/* Save first 64 bytes of bench.dat */
	kr = ext2_read(ext2_port, fid2, 0, 64, &orig2, &orig2_cnt);
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: read bench.dat kr=%d\n", kr);
		vm_deallocate(mach_task_self(), orig1, orig1_cnt);
		goto close_both;
	}

	/* Write pattern A to hello.txt */
	kr = vm_allocate(mach_task_self(), &buf, len_a, TRUE);
	if (kr == KERN_SUCCESS) {
		memcpy((void *)buf, pattern_a, len_a);
		kr = ext2_write(ext2_port, fid1, 0,
				(pointer_t)buf, len_a);
		vm_deallocate(mach_task_self(), buf, len_a);
	}
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: write hello.txt kr=%d\n", kr);
		fail++;
		goto restore;
	}

	/* Write pattern B to bench.dat */
	kr = vm_allocate(mach_task_self(), &buf, len_b, TRUE);
	if (kr == KERN_SUCCESS) {
		memcpy((void *)buf, pattern_b, len_b);
		kr = ext2_write(ext2_port, fid2, 0,
				(pointer_t)buf, len_b);
		vm_deallocate(mach_task_self(), buf, len_b);
	}
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: write bench.dat kr=%d\n", kr);
		fail++;
		goto restore;
	}

	/* Both files now dirty — sync should flush both via dirty list */
	kr = ext2_sync(ext2_port);
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: sync kr=%d\n", kr);
		fail++;
		goto restore;
	}

	/* Close and reopen to verify persistence */
	ext2_close(ext2_port, fid1);
	ext2_close(ext2_port, fid2);

	kr = ext2_open(ext2_port, "hello.txt", &fid1);
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: reopen hello.txt kr=%d\n", kr);
		fail++;
		goto done;
	}
	kr = ext2_open(ext2_port, "bench.dat", &fid2);
	if (kr != KERN_SUCCESS) {
		printf("    FAIL: reopen bench.dat kr=%d\n", kr);
		fail++;
		ext2_close(ext2_port, fid1);
		goto done;
	}

	/* Verify pattern A in hello.txt */
	kr = ext2_read(ext2_port, fid1, 0, len_a, &rdata, &rcount);
	if (kr != KERN_SUCCESS || rcount < len_a) {
		printf("    FAIL: readback hello.txt kr=%d cnt=%u\n",
		       kr, rcount);
		fail++;
	} else {
		unsigned int k, match = 1;
		for (k = 0; k < len_a; k++) {
			if (((char *)rdata)[k] != pattern_a[k]) {
				match = 0;
				break;
			}
		}
		if (!match) {
			printf("    FAIL: hello.txt data mismatch\n");
			fail++;
		} else {
			printf("    dirty-list file 1:   PASS\n");
			pass++;
		}
	}
	if (rcount > 0)
		vm_deallocate(mach_task_self(), rdata, rcount);

	/* Verify pattern B in bench.dat */
	kr = ext2_read(ext2_port, fid2, 0, len_b, &rdata, &rcount);
	if (kr != KERN_SUCCESS || rcount < len_b) {
		printf("    FAIL: readback bench.dat kr=%d cnt=%u\n",
		       kr, rcount);
		fail++;
	} else {
		unsigned int k, match = 1;
		for (k = 0; k < len_b; k++) {
			if (((char *)rdata)[k] != pattern_b[k]) {
				match = 0;
				break;
			}
		}
		if (!match) {
			printf("    FAIL: bench.dat data mismatch\n");
			fail++;
		} else {
			printf("    dirty-list file 2:   PASS\n");
			pass++;
		}
	}
	if (rcount > 0)
		vm_deallocate(mach_task_self(), rdata, rcount);

restore:
	/* Restore originals */
	ext2_write(ext2_port, fid1, 0, orig1,
		   (mach_msg_type_number_t)orig1_cnt);
	ext2_write(ext2_port, fid2, 0, orig2,
		   (mach_msg_type_number_t)orig2_cnt);
	ext2_sync(ext2_port);
	vm_deallocate(mach_task_self(), orig1, orig1_cnt);
	vm_deallocate(mach_task_self(), orig2, orig2_cnt);

close_both:
	ext2_close(ext2_port, fid1);
	ext2_close(ext2_port, fid2);

done:
	printf("  dirty list: %u PASS, %u FAIL\n", pass, fail);
}

/* ===================================================================
 * Negative dentry cache test
 * =================================================================== */

static void
test_negative_dcache(void)
{
	kern_return_t	kr;
	natural_t	fid;
	tvalspec_t	t0, t1;
	unsigned long	total_ns;
	int		i, pass = 0, fail = 0;
	const int	ITERS = 100;

	printf("  negative dcache test:\n");

	/* Correctness: open non-existent file should fail */
	kr = ext2_open(ext2_port, "no_such_file.txt", &fid);
	if (kr != KERN_SUCCESS) {
		printf("    open non-existent:   PASS (kr=%d)\n", kr);
		pass++;
	} else {
		printf("    open non-existent:   FAIL (should not open)\n");
		ext2_close(ext2_port, fid);
		fail++;
	}

	/* Second attempt should hit negative cache (same result, faster) */
	kr = ext2_open(ext2_port, "no_such_file.txt", &fid);
	if (kr != KERN_SUCCESS) {
		printf("    neg cache hit:       PASS (kr=%d)\n", kr);
		pass++;
	} else {
		printf("    neg cache hit:       FAIL (should not open)\n");
		ext2_close(ext2_port, fid);
		fail++;
	}

	printf("  negative dcache: %d PASS, %d FAIL\n", pass, fail);

	/* Benchmark: repeated lookup of non-existent file */
	for (i = 0; i < 4; i++)
		ext2_open(ext2_port, "no_such_file.txt", &fid);

	dget_time(&t0);
	for (i = 0; i < ITERS; i++)
		ext2_open(ext2_port, "no_such_file.txt", &fid);
	dget_time(&t1);
	total_ns = delapsed_ns(&t0, &t1);
	{
		unsigned long us = (total_ns / ITERS) / 1000;
		unsigned long frac = ((total_ns / ITERS) % 1000) / 10;
		printf("  %-28s %5lu.%02lu us/op  (%d iters)\n",
		       "open non-existent (neg$)", us, frac, ITERS);
	}
}

/* ===================================================================
 * File handle clone test (two independent openers of the same file)
 * =================================================================== */

static void
test_file_clone(void)
{
	kern_return_t	kr;
	natural_t	fid1, fid2;
	pointer_t	data1, data2;
	mach_msg_type_number_t cnt1, cnt2;
	int		pass = 0, fail = 0;

	printf("  file clone test:\n");

	/* Open same file twice — second should be cloned */
	kr = ext2_open(ext2_port, "hello.txt", &fid1);
	if (kr != KERN_SUCCESS) {
		printf("    open 1:              FAIL (kr=%d)\n", kr);
		fail++;
		printf("  file clone: %d PASS, %d FAIL\n", pass, fail);
		return;
	}

	kr = ext2_open(ext2_port, "hello.txt", &fid2);
	if (kr != KERN_SUCCESS) {
		printf("    open 2 (clone):      FAIL (kr=%d)\n", kr);
		fail++;
		ext2_close(ext2_port, fid1);
		printf("  file clone: %d PASS, %d FAIL\n", pass, fail);
		return;
	}

	/* Must get different fids */
	if (fid1 != fid2) {
		printf("    separate fids:       PASS (fid1=%u, fid2=%u)\n",
		       fid1, fid2);
		pass++;
	} else {
		printf("    separate fids:       FAIL (same fid=%u)\n", fid1);
		fail++;
	}

	/* Read from both independently */
	kr = ext2_read(ext2_port, fid1, 0, 4096, &data1, &cnt1);
	if (kr == KERN_SUCCESS && cnt1 > 0) {
		printf("    read fid1:           PASS (%u bytes)\n", cnt1);
		pass++;
	} else {
		printf("    read fid1:           FAIL (kr=%d)\n", kr);
		fail++;
	}

	kr = ext2_read(ext2_port, fid2, 0, 4096, &data2, &cnt2);
	if (kr == KERN_SUCCESS && cnt2 > 0) {
		printf("    read fid2:           PASS (%u bytes)\n", cnt2);
		pass++;
	} else {
		printf("    read fid2:           FAIL (kr=%d)\n", kr);
		fail++;
	}

	/* Data from both reads must match */
	if (cnt1 == cnt2 && cnt1 > 0) {
		unsigned int i;
		int match = 1;
		unsigned char *p1 = (unsigned char *)data1;
		unsigned char *p2 = (unsigned char *)data2;
		for (i = 0; i < cnt1; i++) {
			if (p1[i] != p2[i]) {
				match = 0;
				break;
			}
		}
		if (match) {
			printf("    data match:          PASS\n");
			pass++;
		} else {
			printf("    data match:          FAIL (mismatch at %u)\n", i);
			fail++;
		}
	}

	if (cnt1 > 0)
		vm_deallocate(mach_task_self(), data1, cnt1);
	if (cnt2 > 0)
		vm_deallocate(mach_task_self(), data2, cnt2);

	/* Close first, then read from second (must still work) */
	ext2_close(ext2_port, fid1);
	kr = ext2_read(ext2_port, fid2, 0, 4096, &data2, &cnt2);
	if (kr == KERN_SUCCESS && cnt2 > 0) {
		printf("    read after close 1:  PASS (%u bytes)\n", cnt2);
		pass++;
		vm_deallocate(mach_task_self(), data2, cnt2);
	} else {
		printf("    read after close 1:  FAIL (kr=%d)\n", kr);
		fail++;
	}

	ext2_close(ext2_port, fid2);

	printf("  file clone: %d PASS, %d FAIL\n", pass, fail);
}

/* ===================================================================
 * VFS vnode sharing test — two openers share inode state
 * =================================================================== */

static void
test_vnode_sharing(void)
{
	kern_return_t	kr;
	natural_t	fid1, fid2;
	pointer_t	data;
	mach_msg_type_number_t cnt;
	int		pass = 0, fail = 0;
	const char	*test_file = "hello.txt";

	printf("  vnode sharing test:\n");

	/* Open same file twice */
	kr = ext2_open(ext2_port, test_file, &fid1);
	if (kr != KERN_SUCCESS) {
		printf("    open fid1:           FAIL (kr=%d)\n", kr);
		fail++;
		printf("  vnode sharing: %d PASS, %d FAIL\n", pass, fail);
		return;
	}

	kr = ext2_open(ext2_port, test_file, &fid2);
	if (kr != KERN_SUCCESS) {
		printf("    open fid2:           FAIL (kr=%d)\n", kr);
		fail++;
		ext2_close(ext2_port, fid1);
		printf("  vnode sharing: %d PASS, %d FAIL\n", pass, fail);
		return;
	}

	/* Read original content via fid2 */
	kr = ext2_read(ext2_port, fid2, 0, 4096, &data, &cnt);
	if (kr != KERN_SUCCESS || cnt == 0) {
		printf("    initial read fid2:   FAIL (kr=%d)\n", kr);
		fail++;
		ext2_close(ext2_port, fid1);
		ext2_close(ext2_port, fid2);
		printf("  vnode sharing: %d PASS, %d FAIL\n", pass, fail);
		return;
	}
	printf("    initial read fid2:   PASS (%u bytes)\n", cnt);
	pass++;

	/* Save original content for restore */
	unsigned char orig[512];
	unsigned int orig_len = cnt < sizeof(orig) ? cnt : sizeof(orig);
	memcpy(orig, (void *)data, orig_len);
	vm_deallocate(mach_task_self(), data, cnt);

	/* Write modified content via fid1 */
	unsigned char mod[64];
	memcpy(mod, orig, 64 < orig_len ? 64 : orig_len);
	mod[0] ^= 0xFF;  /* flip first byte */

	kr = ext2_write(ext2_port, fid1, 0,
			(pointer_t)mod, 64);
	if (kr != KERN_SUCCESS) {
		printf("    write via fid1:      FAIL (kr=%d)\n", kr);
		fail++;
		ext2_close(ext2_port, fid1);
		ext2_close(ext2_port, fid2);
		printf("  vnode sharing: %d PASS, %d FAIL\n", pass, fail);
		return;
	}
	printf("    write via fid1:      PASS\n");
	pass++;

	/* Read back via fid2 — must see the modification */
	kr = ext2_read(ext2_port, fid2, 0, 64, &data, &cnt);
	if (kr != KERN_SUCCESS || cnt == 0) {
		printf("    read via fid2:       FAIL (kr=%d)\n", kr);
		fail++;
	} else {
		unsigned char *p = (unsigned char *)data;
		if (p[0] == mod[0]) {
			printf("    shared write visible: PASS\n");
			pass++;
		} else {
			printf("    shared write visible: FAIL (expected 0x%02x, got 0x%02x)\n",
			       mod[0], p[0]);
			fail++;
		}
		vm_deallocate(mach_task_self(), data, cnt);
	}

	/* Restore original content */
	ext2_write(ext2_port, fid1, 0,
		   (pointer_t)orig, 64 < orig_len ? 64 : orig_len);
	ext2_sync(ext2_port);

	/* Close first opener, second must still work */
	ext2_close(ext2_port, fid1);

	kr = ext2_read(ext2_port, fid2, 0, 4096, &data, &cnt);
	if (kr == KERN_SUCCESS && cnt > 0) {
		printf("    read after close 1:  PASS (%u bytes)\n", cnt);
		pass++;
		vm_deallocate(mach_task_self(), data, cnt);
	} else {
		printf("    read after close 1:  FAIL (kr=%d)\n", kr);
		fail++;
	}

	ext2_close(ext2_port, fid2);

	printf("  vnode sharing: %d PASS, %d FAIL\n", pass, fail);
}

/* ===================================================================
 * Batch open+read / read+close RPC test and benchmark
 * =================================================================== */

static void
test_batch_rpc(void)
{
	kern_return_t	kr;
	natural_t	fid, file_size;
	pointer_t	data;
	mach_msg_type_number_t data_count;
	tvalspec_t	t0, t1;
	unsigned long	total_ns;
	int		i, pass = 0, fail = 0;
	const int	ITERS = 100;

	printf("  batch RPC test:\n");

	/* Correctness: ext2_open_read */
	kr = ext2_open_read(ext2_port, "hello.txt", 0, 4096,
			    &fid, &data, &data_count);
	if (kr == KERN_SUCCESS && data_count > 0) {
		printf("    open_read:           PASS (fid=%u, %u bytes)\n",
		       fid, data_count);
		pass++;
		vm_deallocate(mach_task_self(), data, data_count);
		ext2_close(ext2_port, fid);
	} else {
		printf("    open_read:           FAIL (kr=%d)\n", kr);
		fail++;
	}

	/* Correctness: ext2_read_close */
	kr = ext2_open(ext2_port, "hello.txt", &fid);
	if (kr == KERN_SUCCESS) {
		kr = ext2_read_close(ext2_port, fid, 0, 4096,
				     &data, &data_count);
		if (kr == KERN_SUCCESS && data_count > 0) {
			printf("    read_close:          PASS (%u bytes)\n",
			       data_count);
			pass++;
			vm_deallocate(mach_task_self(), data, data_count);
		} else {
			printf("    read_close:          FAIL (kr=%d)\n", kr);
			fail++;
		}
	} else {
		printf("    read_close:          FAIL (open kr=%d)\n", kr);
		fail++;
	}

	/* Correctness: open_read non-existent file */
	kr = ext2_open_read(ext2_port, "no_such.txt", 0, 4096,
			    &fid, &data, &data_count);
	if (kr != KERN_SUCCESS) {
		printf("    open_read (absent):  PASS (kr=%d)\n", kr);
		pass++;
	} else {
		printf("    open_read (absent):  FAIL (should not open)\n");
		vm_deallocate(mach_task_self(), data, data_count);
		ext2_close(ext2_port, fid);
		fail++;
	}

	printf("  batch RPC: %d PASS, %d FAIL\n", pass, fail);

	/* Benchmark: 3 RPC (open+read+close) vs 2 RPC (open_read+close) */

	/* Warmup */
	for (i = 0; i < 4; i++) {
		kr = ext2_open(ext2_port, "hello.txt", &fid);
		if (kr == KERN_SUCCESS) {
			ext2_read(ext2_port, fid, 0, 4096,
				  &data, &data_count);
			vm_deallocate(mach_task_self(), data, data_count);
			ext2_close(ext2_port, fid);
		}
	}

	/* 3 RPC: open + read + close */
	dget_time(&t0);
	for (i = 0; i < ITERS; i++) {
		kr = ext2_open(ext2_port, "hello.txt", &fid);
		if (kr != KERN_SUCCESS) break;
		ext2_read(ext2_port, fid, 0, 4096, &data, &data_count);
		vm_deallocate(mach_task_self(), data, data_count);
		ext2_close(ext2_port, fid);
	}
	dget_time(&t1);
	total_ns = delapsed_ns(&t0, &t1);
	{
		unsigned long us = (total_ns / i) / 1000;
		unsigned long frac = ((total_ns / i) % 1000) / 10;
		printf("  %-28s %5lu.%02lu us/op  (%d iters)\n",
		       "open+read+close (3 RPC)", us, frac, i);
	}

	/* Warmup */
	for (i = 0; i < 4; i++) {
		kr = ext2_open_read(ext2_port, "hello.txt", 0, 4096,
				    &fid, &data, &data_count);
		if (kr == KERN_SUCCESS) {
			vm_deallocate(mach_task_self(), data, data_count);
			ext2_close(ext2_port, fid);
		}
	}

	/* 2 RPC: open_read + close */
	dget_time(&t0);
	for (i = 0; i < ITERS; i++) {
		kr = ext2_open_read(ext2_port, "hello.txt", 0, 4096,
				    &fid, &data, &data_count);
		if (kr != KERN_SUCCESS) break;
		vm_deallocate(mach_task_self(), data, data_count);
		ext2_close(ext2_port, fid);
	}
	dget_time(&t1);
	total_ns = delapsed_ns(&t0, &t1);
	{
		unsigned long us = (total_ns / i) / 1000;
		unsigned long frac = ((total_ns / i) % 1000) / 10;
		printf("  %-28s %5lu.%02lu us/op  (%d iters)\n",
		       "open_read+close (2 RPC)", us, frac, i);
	}

	/* Warmup */
	for (i = 0; i < 4; i++) {
		kr = ext2_open_read(ext2_port, "hello.txt", 0, 4096,
				    &fid, &data, &data_count);
		if (kr == KERN_SUCCESS) {
			vm_deallocate(mach_task_self(), data, data_count);
			ext2_read_close(ext2_port, fid, 0, 0,
					&data, &data_count);
		}
	}

	/* 1 RPC: open_read (read all) + read_close (0 bytes, just close) */
	dget_time(&t0);
	for (i = 0; i < ITERS; i++) {
		kr = ext2_open_read(ext2_port, "hello.txt", 0, 4096,
				    &fid, &data, &data_count);
		if (kr != KERN_SUCCESS) break;
		vm_deallocate(mach_task_self(), data, data_count);
		ext2_read_close(ext2_port, fid, 0, 0, &data, &data_count);
	}
	dget_time(&t1);
	total_ns = delapsed_ns(&t0, &t1);
	{
		unsigned long us = (total_ns / i) / 1000;
		unsigned long frac = ((total_ns / i) % 1000) / 10;
		printf("  %-28s %5lu.%02lu us/op  (%d iters)\n",
		       "open_read+read_close (2)", us, frac, i);
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

		/* Negative dentry cache test + benchmark */
		test_negative_dcache();

		/* File clone test */
		test_file_clone();

		/* VFS vnode sharing test */
		test_vnode_sharing();

		/* Batch RPC test + benchmark */
		test_batch_rpc();

		bench_ext2_file_read("ext2 read (hello.txt)", "hello.txt",
				     4096);

		/* Sequential read: bench.dat (~4 MB) at different chunk sizes */
		bench_ext2_file_read("ext2 seq read 1K", "bench.dat", 1024);
		bench_ext2_file_read("ext2 seq read 4K", "bench.dat", 4096);
		bench_ext2_file_read("ext2 seq read 32K", "bench.dat", 32768);
		bench_ext2_file_read("ext2 seq read 64K", "bench.dat", 65536);
		bench_ext2_file_read("ext2 seq read 256K", "bench.dat", 262144);

		/* Write correctness test */
		test_ext2_write_verify();

		/* Write benchmarks */
		bench_ext2_write("ext2 write (cached)", "hello.txt");
		bench_ext2_sync("ext2 sync (clean)");
		bench_ext2_write_sync("ext2 write+sync (durable)",
				      "hello.txt");

		/* Dirty list sync benchmark */
		bench_dirty_list_sync();

		/* Merge test: write many 1K blocks then sync */
		printf("  ext2 merge test:\n");
		bench_ext2_merge_sync("ext2 merge sync (bench.dat)",
				      "bench.dat");

		/* Stress tests */
		stress_ext2_write();

		/* Dirty list test — multiple dirty files + sync */
		test_dirty_list();
	}

	if (have_ahci && have_ext2) {
		printf("  (ext2 reads go: ipc_bench -> ext2_server "
		       "-> ahci_driver -> AHCI HW)\n");
	}

	printf("\n");
}
