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
