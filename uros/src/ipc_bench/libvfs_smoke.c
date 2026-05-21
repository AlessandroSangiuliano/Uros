/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * libvfs_smoke.c — end-to-end smoke test for libvfs (#220 v0.1).
 *
 * Verifies the path: client -> libvfs -> name_server (mount lookup)
 * -> ext_server (vfs.defs RPCs) -> ext2 backend.  Runs once after the
 * FLIPC v2 suite; output is a compact PASS/FAIL summary.
 */

#include <libvfs.h>
#include <mach.h>
#include <mach/clock.h>
#include <mach/clock_types.h>
#include <stdio.h>
#include <string.h>

void
bench_libvfs_smoke(void)
{
	vfs_fd_t fd;
	vfs_stat_t st;
	char buf[64];
	ssize_t n;
	int ok = 1;

	printf("\n--- libvfs smoke test (#220 v0.1) ---\n");

	if (vfs_init() != 0) {
		printf("  libvfs: vfs_init failed\n");
		return;
	}

	/* Path lookup + open via mount table -> ext_server at "/" */
	fd = vfs_open("/hello.txt", VFS_O_RDONLY, 0);
	if (fd == VFS_FD_INVALID) {
		printf("  libvfs: vfs_open(/hello.txt) FAILED — "
		       "is /hello.txt on the rootfs?\n");
		return;
	}
	printf("  libvfs: vfs_open(/hello.txt) -> fd=%d\n", fd);

	if (vfs_fstat(fd, &st) != 0) {
		printf("  libvfs: vfs_fstat FAILED\n");
		ok = 0;
	} else {
		printf("  libvfs: vfs_fstat: size=%llu type=%u\n",
		       (unsigned long long)st.st_size, (unsigned)st.st_type);
	}

	memset(buf, 0, sizeof(buf));
	n = vfs_read(fd, buf, sizeof(buf) - 1);
	if (n < 0) {
		printf("  libvfs: vfs_read FAILED\n");
		ok = 0;
	} else {
		buf[n < (ssize_t)sizeof(buf) ? n : (ssize_t)(sizeof(buf) - 1)] =
			'\0';
		printf("  libvfs: vfs_read -> %ld bytes: \"%s\"\n",
		       (long)n, buf);
	}

	if (vfs_close(fd) != 0) {
		printf("  libvfs: vfs_close FAILED\n");
		ok = 0;
	}

	/* Second path: stat without opening, using mount lookup cache. */
	if (vfs_stat("/hello.txt", &st) != 0) {
		printf("  libvfs: vfs_stat(/hello.txt) FAILED\n");
		ok = 0;
	} else {
		printf("  libvfs: vfs_stat(/hello.txt) size=%llu\n",
		       (unsigned long long)st.st_size);
	}

	/* Writable namespace (#264): create -> write -> read back. */
	if (1) {
		const char *p = "/uros_w.txt";
		const char *msg = "writable ext2!";
		vfs_fd_t wfd = vfs_open(p, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC,
					0644);
		if (wfd == VFS_FD_INVALID) {
			printf("  libvfs: O_CREAT %s FAILED\n", p);
			ok = 0;
		} else {
			ssize_t w = vfs_write(wfd, msg, strlen(msg));
			char rb[32];
			ssize_t r;
			vfs_lseek(wfd, 0, VFS_SEEK_SET);
			memset(rb, 0, sizeof(rb));
			r = vfs_read(wfd, rb, sizeof(rb) - 1);
			printf("  libvfs: create+write %ld, read %ld: \"%s\"\n",
			       (long)w, (long)r, rb);
			if (w != (ssize_t)strlen(msg) || r != w ||
			    strcmp(rb, msg) != 0)
				ok = 0;
			vfs_close(wfd);
		}
	}

	/* Namespace ops (#231): rename then unlink, same mount. */
	if (1) {
		vfs_stat_t st;
		if (vfs_rename("/uros_w.txt", "/uros_r.txt") != 0) {
			printf("  libvfs: rename FAILED\n");
			ok = 0;
		} else if (vfs_stat("/uros_r.txt", &st) != 0 ||
			   vfs_stat("/uros_w.txt", &st) == 0) {
			printf("  libvfs: rename visibility wrong\n");
			ok = 0;
		} else {
			printf("  libvfs: rename /uros_w.txt -> /uros_r.txt OK "
			       "(size=%llu)\n", (unsigned long long)st.st_size);
			if (vfs_unlink("/uros_r.txt") != 0 ||
			    vfs_stat("/uros_r.txt", &st) == 0) {
				printf("  libvfs: unlink FAILED\n");
				ok = 0;
			} else {
				printf("  libvfs: unlink /uros_r.txt OK\n");
			}
		}
	}

	/* Try the second mount if present.  Failure is OK — depends on
	 * which AHCI partitions QEMU exposes today. */
	fd = vfs_open("/mnt/disk1/hello.txt", VFS_O_RDONLY, 0);
	if (fd == VFS_FD_INVALID) {
		printf("  libvfs: /mnt/disk1/hello.txt unreachable "
		       "(optional mount, skipping)\n");
	} else {
		printf("  libvfs: /mnt/disk1/hello.txt fd=%d "
		       "(mount cache routed via /mnt/disk1)\n", fd);
		vfs_close(fd);
	}

	printf("  libvfs: %s\n", ok ? "PASS" : "FAIL");
}

/*
 * #232 — A/B throughput: read a large file through vfs_read with the
 * FLIPC v2 fast-path disabled (Mach fs_read) vs enabled (shared-memory
 * channel).  Same call path, same chunking — only the data plane
 * differs.  Run under the disk suite (needs /bench_large.dat + the
 * realtime clock for timing).
 */
static unsigned long
lvf_read_whole(vfs_fd_t fd, char *buf, unsigned int chunk, mach_port_t clock)
{
	tvalspec_t t0, t1;
	ssize_t n;

	vfs_lseek(fd, 0, VFS_SEEK_SET);
	clock_get_time(clock, &t0);
	while ((n = vfs_read(fd, buf, chunk)) > 0)
		;
	clock_get_time(clock, &t1);
	return (unsigned long)(t1.tv_sec - t0.tv_sec) * 1000000000UL
	       + (unsigned long)(t1.tv_nsec - t0.tv_nsec);
}

void
bench_libvfs_flipc(mach_port_t clock)
{
	static char buf[65536];
	const char *path = "/bench_large.dat";
	static const unsigned int chunks[] = { 4096, 16384, 65536 };
	vfs_fd_t fd;
	vfs_stat_t st;
	unsigned long fsize;
	unsigned int ci;

	printf("\n--- libvfs FLIPC v2 fast-path read A/B (#232) ---\n");

	if (vfs_init() != 0)
		return;
	fd = vfs_open(path, VFS_O_RDONLY, 0);
	if (fd == VFS_FD_INVALID) {
		printf("  open %s failed — skipping (seed bench_large.dat)\n",
		       path);
		return;
	}
	if (vfs_fstat(fd, &st) != 0) {
		printf("  fstat failed\n");
		vfs_close(fd);
		return;
	}
	fsize = (unsigned long)st.st_size;

	/* Warm the page cache AND establish the FLIPC channel (first large
	 * read connects lazily) so neither cost lands inside the timed run. */
	vfs_flipc_set_enabled(1);
	(void)lvf_read_whole(fd, buf, sizeof(buf), clock);

	printf("  file=%lu KB, warm cache; Mach fs_read vs FLIPC fast-path:\n",
	       fsize / 1024);
	printf("  chunk    Mach MB/s   FLIPC MB/s   speedup\n");

	for (ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ci++) {
		unsigned int chunk = chunks[ci];
		unsigned long mach_ns, flipc_ns;
		unsigned long mach_us, flipc_us, mach_mbps, flipc_mbps, ratio;

		vfs_flipc_set_enabled(0);
		mach_ns = lvf_read_whole(fd, buf, chunk, clock);
		vfs_flipc_set_enabled(1);
		flipc_ns = lvf_read_whole(fd, buf, chunk, clock);

		mach_us  = mach_ns / 1000;
		flipc_us = flipc_ns / 1000;
		mach_mbps  = mach_us  ? fsize / mach_us  : 0;
		flipc_mbps = flipc_us ? fsize / flipc_us : 0;
		ratio = flipc_us ? (mach_us * 100) / flipc_us : 0;

		printf("  %4uK    %8lu    %8lu     %lu.%02lux\n",
		       chunk / 1024, mach_mbps, flipc_mbps,
		       ratio / 100, ratio % 100);
	}

	vfs_close(fd);
	vfs_flipc_set_enabled(1);
}
