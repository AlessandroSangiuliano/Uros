/*
 * flipc_bench — standalone libvfs FLIPC v2 fast-path A/B benchmark (#272).
 *
 * Replaces the in-process A/B that #232 wired into ipc_bench's disk suite.
 * That version ran once per chunk WHILE the system was still booting and
 * every server contended for the single CPU under KVM, so per-chunk
 * numbers swung wildly run-to-run (16K read measured 0.72x and 6.16x).
 *
 * This is a musl-linked userland binary launched from the shell AFTER
 * boot, when the system is quiescent.  It runs each A/B point N times and
 * reports the MEDIAN (dropping a warmup), so the FLIPC-vs-Mach ratio is
 * stable.  Chunk set, run count, file path and which directions to
 * measure are all command-line controllable.
 *
 *   flipc_bench [-n RUNS] [-c K1,K2,...] [-f PATH] [-r] [-w]
 *     -n RUNS   timed runs per A/B point, median reported (default 5)
 *     -c LIST   comma-separated chunk sizes in KiB (default 4,16,64,1024)
 *     -f PATH   read benchmark file (default /bench_large.dat)
 *     -r        read A/B only
 *     -w        write A/B only
 *   (no -r/-w runs both)
 *
 * Scope (#272): warm read/write only.  Cold (page-cache miss) needs a
 * drop-cache RPC in ext_server and is tracked separately.
 *
 * We reopen the file per timed run to reset the read/write position
 * rather than vfs_lseek(): libvfs.h types off_t as 32-bit (the sa_mach
 * world) while this binary is musl (off_t 64-bit), so calling vfs_lseek
 * across that boundary would mismatch the ABI.  Reopening is untimed and
 * cheap, and the FLIPC channel is cached per fs_server (not per fd), so
 * it is NOT re-established on reopen — only the per-fd prefetch window
 * re-warms, exactly as a seek-to-zero would force anyway.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pull off_t/ssize_t from musl (included above) and stop libvfs.h from
 * redefining them with its narrower sa_mach types. */
#define _SSIZE_T_DEFINED 1
#define _OFF_T_DEFINED   1
#include <libvfs.h>

#include <mach.h>
#include <mach/clock.h>
#include <mach/clock_types.h>

/* Trap, defined in libmach_core; no prototype reaches the musl include
 * world, so declare it here. */
extern mach_port_t mach_host_self(void);

#define FB_MAX_CHUNKS    8
#define FB_MAX_RUNS      64
#define FB_DEFAULT_RUNS  5
#define FB_WRITE_TOTAL   (4UL * 1024 * 1024)   /* 4 MiB write workload */
#define FB_BUF_BYTES     (1024 * 1024)         /* 1 MiB == largest chunk */
#define FB_WRITE_PATH    "/flipc_wtest.dat"

static char fb_buf[FB_BUF_BYTES];

/* ------------------------------------------------------------------ */
/* Timing + statistics                                                 */
/* ------------------------------------------------------------------ */

static unsigned long
elapsed_ns(const tvalspec_t *t0, const tvalspec_t *t1)
{
	return (unsigned long)(t1->tv_sec - t0->tv_sec) * 1000000000UL
	       + (unsigned long)(t1->tv_nsec - t0->tv_nsec);
}

/* Median of n samples (ascending insertion sort on a small array). */
static unsigned long
median_ns(unsigned long *s, int n)
{
	for (int i = 1; i < n; i++) {
		unsigned long v = s[i];
		int j = i - 1;
		while (j >= 0 && s[j] > v) {
			s[j + 1] = s[j];
			j--;
		}
		s[j + 1] = v;
	}
	return s[n / 2];
}

static void
report_row(unsigned int chunk, unsigned long bytes,
           unsigned long mach_ns, unsigned long flipc_ns)
{
	unsigned long mach_us  = mach_ns / 1000;
	unsigned long flipc_us = flipc_ns / 1000;
	unsigned long mach_mbps  = mach_us  ? bytes / mach_us  : 0;
	unsigned long flipc_mbps = flipc_us ? bytes / flipc_us : 0;
	unsigned long ratio = flipc_us ? (mach_us * 100) / flipc_us : 0;

	printf("  %5uK   %9lu   %9lu     %lu.%02lux\n",
	       chunk / 1024, mach_mbps, flipc_mbps,
	       ratio / 100, ratio % 100);
}

/* ------------------------------------------------------------------ */
/* Read / write whole-file workloads (one reopen each, position reset)  */
/* ------------------------------------------------------------------ */

static unsigned long
read_once(const char *path, unsigned int chunk, mach_port_t clock)
{
	tvalspec_t t0, t1;
	ssize_t n;
	vfs_fd_t fd = vfs_open(path, VFS_O_RDONLY, 0);
	if (fd == VFS_FD_INVALID)
		return 0;

	clock_get_time(clock, &t0);
	while ((n = vfs_read(fd, fb_buf, chunk)) > 0)
		;
	clock_get_time(clock, &t1);

	vfs_close(fd);
	return elapsed_ns(&t0, &t1);
}

static unsigned long
write_once(unsigned int chunk, unsigned long total, mach_port_t clock)
{
	tvalspec_t t0, t1;
	unsigned long off = 0;
	vfs_fd_t fd = vfs_open(FB_WRITE_PATH,
	                       VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, 0644);
	if (fd == VFS_FD_INVALID)
		return 0;

	clock_get_time(clock, &t0);
	while (off < total) {
		unsigned int want = (total - off) < chunk ? (total - off) : chunk;
		ssize_t w = vfs_write(fd, fb_buf, want);
		if (w <= 0)
			break;
		off += w;
	}
	vfs_sync(fd);
	clock_get_time(clock, &t1);

	vfs_close(fd);
	return elapsed_ns(&t0, &t1);
}

static unsigned long
read_median(const char *path, unsigned int chunk, int runs, mach_port_t clock)
{
	unsigned long s[FB_MAX_RUNS];
	(void)read_once(path, chunk, clock);          /* warmup */
	for (int i = 0; i < runs; i++)
		s[i] = read_once(path, chunk, clock);
	return median_ns(s, runs);
}

static unsigned long
write_median(unsigned int chunk, unsigned long total, int runs,
             mach_port_t clock)
{
	unsigned long s[FB_MAX_RUNS];
	(void)write_once(chunk, total, clock);        /* warmup */
	for (int i = 0; i < runs; i++)
		s[i] = write_once(chunk, total, clock);
	return median_ns(s, runs);
}

/* ------------------------------------------------------------------ */
/* Benchmarks                                                          */
/* ------------------------------------------------------------------ */

static void
bench_read(const char *path, const unsigned int *chunks, int nch,
           int runs, mach_port_t clock)
{
	vfs_fd_t fd;
	vfs_stat_t st;
	unsigned long fsize;

	printf("\n--- libvfs FLIPC v2 read A/B (warm, median of %d) ---\n", runs);

	fd = vfs_open(path, VFS_O_RDONLY, 0);
	if (fd == VFS_FD_INVALID) {
		printf("  open %s failed — skipping (seed bench_large.dat)\n", path);
		return;
	}
	if (vfs_fstat(fd, &st) != 0) {
		printf("  fstat failed\n");
		vfs_close(fd);
		return;
	}
	fsize = (unsigned long)st.st_size;
	vfs_close(fd);

	printf("  file=%s (%lu KB)  Mach fs_read vs FLIPC fast-path:\n",
	       path, fsize / 1024);
	printf("  chunk    Mach MB/s   FLIPC MB/s   speedup\n");

	for (int ci = 0; ci < nch; ci++) {
		unsigned int chunk = chunks[ci];
		unsigned long mach_ns, flipc_ns;

		vfs_flipc_set_enabled(0);
		mach_ns = read_median(path, chunk, runs, clock);
		vfs_flipc_set_enabled(1);
		flipc_ns = read_median(path, chunk, runs, clock);

		report_row(chunk, fsize, mach_ns, flipc_ns);
	}

	vfs_flipc_set_enabled(1);
}

static void
bench_write(const unsigned int *chunks, int nch, int runs, mach_port_t clock)
{
	const unsigned long total = FB_WRITE_TOTAL;
	vfs_fd_t fd;

	printf("\n--- libvfs FLIPC v2 write A/B (warm, median of %d) ---\n", runs);

	for (unsigned int i = 0; i < FB_BUF_BYTES; i++)
		fb_buf[i] = (char)(i & 0xff);

	printf("  file=%lu KB  Mach fs_write vs FLIPC write-behind (incl. sync):\n",
	       total / 1024);
	printf("  chunk    Mach MB/s   FLIPC MB/s   speedup\n");

	for (int ci = 0; ci < nch; ci++) {
		unsigned int chunk = chunks[ci];
		unsigned long mach_ns, flipc_ns;

		vfs_flipc_set_enabled(0);
		mach_ns = write_median(chunk, total, runs, clock);
		vfs_flipc_set_enabled(1);
		flipc_ns = write_median(chunk, total, runs, clock);

		report_row(chunk, total, mach_ns, flipc_ns);
	}

	/* Correctness: the buffer pattern is buf[j]=j&0xff and FB_BUF_BYTES
	 * is a multiple of 256, so byte at absolute offset X is X&0xff. */
	fd = vfs_open(FB_WRITE_PATH, VFS_O_RDONLY, 0);
	if (fd != VFS_FD_INVALID) {
		int ok = 1;
		unsigned long off = 0;
		while (off < total && ok) {
			ssize_t r = vfs_read(fd, fb_buf, FB_BUF_BYTES);
			if (r <= 0) { ok = 0; break; }
			for (unsigned i = 0; i < (unsigned)r; i++)
				if (fb_buf[i] != (char)((off + i) & 0xff)) {
					ok = 0;
					break;
				}
			off += r;
		}
		printf("  write A/B readback: %s (%lu bytes)\n",
		       ok ? "OK" : "MISMATCH", off);
		vfs_close(fd);
	}

	vfs_unlink(FB_WRITE_PATH);
	vfs_flipc_set_enabled(1);
}

/* ------------------------------------------------------------------ */
/* Argv parsing                                                        */
/* ------------------------------------------------------------------ */

/* Parse "4,16,64,1024" (KiB) into bytes; clamps to FB_BUF_BYTES and
 * FB_MAX_CHUNKS.  Returns the count parsed. */
static int
parse_chunks(const char *list, unsigned int *out)
{
	int n = 0;
	const char *p = list;
	while (*p && n < FB_MAX_CHUNKS) {
		long kib = strtol(p, (char **)&p, 10);
		if (kib > 0) {
			unsigned long bytes = (unsigned long)kib * 1024;
			if (bytes > FB_BUF_BYTES)
				bytes = FB_BUF_BYTES;
			out[n++] = (unsigned int)bytes;
		}
		while (*p == ',')
			p++;
	}
	return n;
}

/* Diagnostic (#272 follow-up): N times { vfs_open; vfs_close } with no
 * read, no clock, no per-iteration printf — isolates the fd open/close
 * cycle from everything else to pin down the churn-corruption bug. */
static void
diag_open_close(const char *path, int iters)
{
	printf("flipc_bench: open/close diag iters=%d\n", iters);
	for (int i = 0; i < iters; i++) {
		vfs_fd_t fd = vfs_open(path, VFS_O_RDONLY, 0);
		if (fd == VFS_FD_INVALID) {
			printf("  open #%d FAILED\n", i);
			break;
		}
		vfs_close(fd);
	}
	printf("flipc_bench: open/close diag done\n");
}

/* Diagnostic: N times { vfs_open; read whole file; vfs_close } with NO
 * clock_get_time — separates the read path from the timing RPC. */
static void
diag_open_read_close(const char *path, int iters)
{
	printf("flipc_bench: open/read/close diag iters=%d\n", iters);
	for (int i = 0; i < iters; i++) {
		vfs_fd_t fd = vfs_open(path, VFS_O_RDONLY, 0);
		ssize_t n;
		if (fd == VFS_FD_INVALID) {
			printf("  open #%d FAILED\n", i);
			break;
		}
		while ((n = vfs_read(fd, fb_buf, 65536)) > 0)
			;
		vfs_close(fd);
	}
	printf("flipc_bench: open/read/close diag done\n");
}

/* Diagnostic: N times clock_get_time only — isolates the timing RPC from
 * the vfs open/read/close path entirely. */
static void
diag_clock(mach_port_t clock, int iters)
{
	tvalspec_t t;
	printf("flipc_bench: clock diag iters=%d\n", iters);
	for (int i = 0; i < iters; i++) {
		clock_get_time(clock, &t);
		clock_get_time(clock, &t);
	}
	printf("flipc_bench: clock diag done\n");
}

/* Diagnostic: N times the exact read_once() body (open + clock + read +
 * clock + close), in a flat loop with no read_median/bench_read wrapper —
 * isolates the interleaving of clock_get_time and the vfs RPCs. */
static void
diag_full(const char *path, mach_port_t clock, int iters)
{
	printf("flipc_bench: full read_once diag iters=%d\n", iters);
	for (int i = 0; i < iters; i++)
		(void)read_once(path, 65536, clock);
	printf("flipc_bench: full read_once diag done\n");
}

/* Diagnostic: like -R but with the FLIPC fast-path FORCED OFF, so every
 * read goes through the Mach fs_read OOL path (the suspect). */
static void
diag_mach(const char *path, int iters)
{
	printf("flipc_bench: mach-path read diag iters=%d\n", iters);
	vfs_flipc_set_enabled(0);
	for (int i = 0; i < iters; i++) {
		vfs_fd_t fd = vfs_open(path, VFS_O_RDONLY, 0);
		ssize_t n;
		if (fd == VFS_FD_INVALID) {
			printf("  open #%d FAILED\n", i);
			break;
		}
		while ((n = vfs_read(fd, fb_buf, 65536)) > 0)
			;
		vfs_close(fd);
	}
	vfs_flipc_set_enabled(1);
	printf("flipc_bench: mach-path read diag done\n");
}

int
main(int argc, char **argv)
{
	unsigned int chunks[FB_MAX_CHUNKS] = { 4096, 16384, 65536, 1048576 };
	int nch = 4;
	int runs = FB_DEFAULT_RUNS;
	int do_read = 0, do_write = 0;
	int diag_oc = 0;
	int diag_orc = 0;
	int diag_clk = 0;
	int diag_x = 0;
	int diag_m = 0;
	const char *path = "/bench_large.dat";

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-n") && i + 1 < argc) {
			runs = atoi(argv[++i]);
		} else if (!strcmp(a, "-c") && i + 1 < argc) {
			int n = parse_chunks(argv[++i], chunks);
			if (n > 0)
				nch = n;
		} else if (!strcmp(a, "-f") && i + 1 < argc) {
			path = argv[++i];
		} else if (!strcmp(a, "-O") && i + 1 < argc) {
			diag_oc = atoi(argv[++i]);
		} else if (!strcmp(a, "-R") && i + 1 < argc) {
			diag_orc = atoi(argv[++i]);
		} else if (!strcmp(a, "-C") && i + 1 < argc) {
			diag_clk = atoi(argv[++i]);
		} else if (!strcmp(a, "-X") && i + 1 < argc) {
			diag_x = atoi(argv[++i]);
		} else if (!strcmp(a, "-M") && i + 1 < argc) {
			diag_m = atoi(argv[++i]);
		} else if (!strcmp(a, "-r")) {
			do_read = 1;
		} else if (!strcmp(a, "-w")) {
			do_write = 1;
		} else {
			printf("flipc_bench: usage: %s [-n runs] [-c k1,k2,...] "
			       "[-f path] [-r] [-w]\n", argv[0]);
			printf("  diagnostics: -O N (open/close) | -R N (open+read+"
			       "close, FLIPC) | -M N (same, Mach OOL) |\n"
			       "               -C N (clock only) | -X N (read_once)\n");
			return 2;
		}
	}
	if (runs < 1)  runs = 1;
	if (runs > FB_MAX_RUNS) runs = FB_MAX_RUNS;
	if (!do_read && !do_write)
		do_read = do_write = 1;

	mach_port_t host = mach_host_self();
	mach_port_t clock = MACH_PORT_NULL;
	if (host_get_clock_service(host, REALTIME_CLOCK, &clock)
	    != KERN_SUCCESS || clock == MACH_PORT_NULL) {
		printf("flipc_bench: clock unavailable\n");
		return 1;
	}

	if (vfs_init() != 0) {
		printf("flipc_bench: vfs_init failed\n");
		return 1;
	}

	if (diag_oc > 0) {
		diag_open_close(path, diag_oc);
		printf("flipc_bench: done\n");
		return 0;
	}
	if (diag_orc > 0) {
		diag_open_read_close(path, diag_orc);
		printf("flipc_bench: done\n");
		return 0;
	}
	if (diag_clk > 0) {
		diag_clock(clock, diag_clk);
		printf("flipc_bench: done\n");
		return 0;
	}
	if (diag_x > 0) {
		diag_full(path, clock, diag_x);
		printf("flipc_bench: done\n");
		return 0;
	}
	if (diag_m > 0) {
		diag_mach(path, diag_m);
		printf("flipc_bench: done\n");
		return 0;
	}

	printf("flipc_bench: runs=%d chunks=%d\n", runs, nch);
	if (do_read)
		bench_read(path, chunks, nch, runs, clock);
	if (do_write)
		bench_write(chunks, nch, runs, clock);

	printf("flipc_bench: done\n");
	return 0;
}
