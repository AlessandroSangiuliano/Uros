/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A file written by one target and read by the other (#498).
 *
 * ext2's on-disk structures are little-endian and fixed-width by definition,
 * so the risk in porting a filesystem server is entirely in what they are cast
 * TO: a block number read into a type that is eight bytes here and was four
 * there, an offset computed in the wrong width.  A server that reads back what
 * it wrote itself cannot see that -- the same wrong cast on both halves agrees
 * with itself.  Only a file that crosses from one target's code to the other's
 * can.
 *
 * ── Two roles, one binary ─────────────────────────────────────────────
 *
 *	xfile_test write PATH	create PATH, fill it, sync it, read it back
 *	xfile_test read  PATH	check a file that some OTHER boot wrote
 *
 * Both are in both bundles.  On an ordinary boot the reader finds nothing and
 * says NOT ASKED; the crossing is made by booting one target and then the
 * other with the same disk attached.
 *
 * ── What the file is made of ──────────────────────────────────────────
 *
 * A 32-byte header -- magic, total size, the name of the target that wrote it
 * -- encoded BYTE BY BYTE and never through a struct, so that this program
 * does not carry the width assumption it exists to catch.  Then a body in
 * which every 8-byte word encodes its own index: a word that lands in the
 * wrong place, or a block that comes back from the wrong address, is a
 * mismatch at a named offset rather than a plausible byte.
 *
 * 🔑 The size is chosen for the block map, not for being round.  On a
 * 4 KiB-block filesystem 62674 bytes is twelve direct blocks and four more
 * reached through the single-indirect block, the last of them partly used.
 * The indirect block is an array of 32-bit block numbers on disk, which is
 * exactly the kind of field a port gets wrong.
 *
 * ⚠️ The writes are 3000 bytes each, which does not divide the block size,
 * so most of them straddle a block boundary and some end inside one.  A
 * server that only handled whole blocks would pass a test that wrote in
 * whole blocks.
 */

#include <stdio.h>
#include <string.h>
#include <mach.h>
#include <mach/bootstrap.h>
#include <mach/mach_traps.h>
#include <mach/thread_switch.h>
#include <sa_mach.h>
#include <mach_init.h>			/* name_server_port */
#include <servers/netname.h>
#include <servers/netname_defs.h>	/* netname_name_t */
#include <libvfs.h>

#if defined(__x86_64__)
#define XF_TARGET	"x86_64"
#elif defined(__i386__)
#define XF_TARGET	"i386"
#else
#error "xfile_test: name this target"
#endif

#define XF_SIZE		62674u
#define XF_HDR		32u
#define XF_NAME_OFF	16u
#define XF_NAME_LEN	16u
#define XF_WRITE_CHUNK	3000u
#define XF_READ_CHUNK	5000u
#define XF_MIX		0x9E3779B97F4A7C15ull

/* How long ext_server may take to register its root, in 100 ms steps. */
#define XF_WAIT_STEPS	1200u

static const unsigned char xf_magic[8] = {
	'U', 'R', 'O', 'S', 'X', 'F', 'T', '1'
};

static const char	*tag;		/* "xfile_write" or "xfile_read" */
static int		 passed, failed;
static unsigned char	 buf[XF_READ_CHUNK];

/*
 * The body byte at offset `off'.  The word index is multiplied by an odd
 * constant, which is a bijection modulo 2^64, so no two words carry the same
 * value.
 */
static unsigned char
xf_byte(uint32_t off)
{
	uint64_t v = (uint64_t)(off >> 3) * XF_MIX;

	return (unsigned char)(v >> ((off & 7u) * 8u));
}

static void
put_le64(unsigned char *p, uint64_t v)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t
get_le64(const unsigned char *p)
{
	uint64_t v = 0;
	unsigned int i;

	for (i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}

/* The expected byte at `off' of a file whose header is `hdr'. */
static unsigned char
xf_expected(uint32_t off, const unsigned char *hdr)
{
	return off < XF_HDR ? hdr[off] : xf_byte(off);
}

static void
xf_header(unsigned char *hdr, const char *writer)
{
	memset(hdr, 0, XF_HDR);
	memcpy(hdr, xf_magic, sizeof(xf_magic));
	put_le64(hdr + 8, XF_SIZE);
	strncpy((char *)hdr + XF_NAME_OFF, writer, XF_NAME_LEN - 1);
}

/*
 * Wait until ext_server is SERVING, and not merely registered.
 *
 * ⚠️ It registers its mounts one at a time and only then enters its message
 * loop, so a lookup made between the first registration and the second would
 * find "/" and report /mnt/disk2 absent while it was on its way.  A longer
 * sleep would narrow that window; this closes it.  An RPC to the root mount is
 * answered by the loop, and the loop starts after the last mount is
 * registered -- so once vfs_stat("/") has returned, the set of mounts is the
 * one this boot will have.
 */
static int
xf_wait_for_ext_server(void)
{
	netname_name_t	matched;
	mach_port_t	port;
	vfs_stat_t	st;
	kern_return_t	kr;
	unsigned int	step;

	for (step = 0; step < XF_WAIT_STEPS; step++) {
		port = MACH_PORT_NULL;
		matched[0] = '\0';
		kr = netname_look_up_mount(name_server_port, "/", &port,
					   matched);
		if (kr == KERN_SUCCESS && port != MACH_PORT_NULL) {
			(void)mach_port_deallocate(mach_task_self(), port);
			break;
		}
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 100);
	}
	if (step == XF_WAIT_STEPS) {
		printf("%s: WRONG — no \"/\" mount in the name server after "
		       "%u s: ext_server never registered its root\n", tag,
		       XF_WAIT_STEPS / 10);
		return -1;
	}
	if (vfs_stat("/", &st) != 0) {
		printf("%s: WRONG — \"/\" is registered and a stat of it "
		       "failed: ext_server answers nothing\n", tag);
		return -1;
	}
	return 0;
}

/*
 * Is the mount that holds `path' the one its directory names?  A path under
 * /mnt/disk2 on a boot without that disk resolves to "/", and a write there
 * would land on the root filesystem and look like a success.
 */
static int
xf_mount_present(const char *path, char *dir, size_t dirlen)
{
	netname_name_t	matched;
	mach_port_t	port = MACH_PORT_NULL;
	kern_return_t	kr;
	const char	*slash = strrchr(path, '/');
	size_t		n;

	n = (slash == NULL || slash == path) ? 1 : (size_t)(slash - path);
	if (n >= dirlen)
		n = dirlen - 1;
	memcpy(dir, path, n);
	dir[n] = '\0';
	if (slash == NULL)
		dir[0] = '/';

	matched[0] = '\0';
	kr = netname_look_up_mount(name_server_port, path, &port, matched);
	if (kr != KERN_SUCCESS)
		return 0;
	if (port != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), port);
	return strcmp(matched, dir) == 0;
}

/*
 * Read the whole file from offset 0 and compare every byte.  The header's
 * writer name is taken from the file -- the reader cannot know it -- after
 * checking that it is one this program writes.
 */
static void
xf_check_contents(vfs_fd_t fd, int arm_hdr, int arm_body)
{
	unsigned char	hdr[XF_HDR];
	char		writer[XF_NAME_LEN];
	uint32_t	off = 0, bad = 0, first_bad = 0;
	unsigned char	got_bad = 0, want_bad = 0;
	ssize_t		r;
	uint32_t	i;

	if (vfs_lseek(fd, 0, VFS_SEEK_SET) != 0) {
		printf("%s: [%d] WRONG — a seek to offset 0 failed\n", tag,
		       arm_hdr);
		failed++;
		return;
	}

	r = vfs_read(fd, hdr, XF_HDR);
	if (r != (ssize_t)XF_HDR) {
		printf("%s: [%d] WRONG — the header read returned %ld of %u "
		       "bytes\n", tag, arm_hdr, (long)r, XF_HDR);
		failed++;
		return;
	}
	memcpy(writer, hdr + XF_NAME_OFF, XF_NAME_LEN);
	writer[XF_NAME_LEN - 1] = '\0';
	if (memcmp(hdr, xf_magic, sizeof(xf_magic)) != 0 ||
	    get_le64(hdr + 8) != XF_SIZE ||
	    (strcmp(writer, "i386") != 0 && strcmp(writer, "x86_64") != 0)) {
		printf("%s: [%d] WRONG — the header is not one this program "
		       "writes: magic %02x%02x%02x%02x..., size %llu, writer "
		       "\"%s\"\n", tag, arm_hdr, hdr[0], hdr[1], hdr[2],
		       hdr[3], (unsigned long long)get_le64(hdr + 8), writer);
		failed++;
		return;
	}
	printf("%s: [%d] the header says %u bytes, written by %s and read "
	       "by %s\n", tag, arm_hdr, XF_SIZE, writer, XF_TARGET);
	passed++;

	off = XF_HDR;
	while (off < XF_SIZE) {
		r = vfs_read(fd, buf, XF_READ_CHUNK);
		if (r <= 0)
			break;
		for (i = 0; i < (uint32_t)r && off + i < XF_SIZE; i++) {
			unsigned char want = xf_expected(off + i, hdr);

			if (buf[i] == want)
				continue;
			if (bad == 0) {
				first_bad = off + i;
				got_bad = buf[i];
				want_bad = want;
			}
			bad++;
		}
		off += (uint32_t)r;
	}

	if (off < XF_SIZE) {
		printf("%s: [%d] WRONG — the file ended at %u of %u bytes\n",
		       tag, arm_body, off, XF_SIZE);
		failed++;
	} else if (bad != 0) {
		printf("%s: [%d] WRONG — %u byte(s) differ; the first at "
		       "offset %u (block %u of 4 KiB) is 0x%02x, expected "
		       "0x%02x\n", tag, arm_body, bad, first_bad,
		       first_bad / 4096u, got_bad, want_bad);
		failed++;
	} else {
		printf("%s: [%d] all %u bytes after the header are the ones "
		       "the writer computed\n", tag, arm_body, XF_SIZE - XF_HDR);
		passed++;
	}
}

static void
xf_write(const char *path)
{
	unsigned char	hdr[XF_HDR];
	vfs_fd_t	fd;
	uint32_t	off = 0, n, i;
	ssize_t		r = 0;

	fd = vfs_open(path, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, 0644);
	if (fd == VFS_FD_INVALID) {
		printf("%s: [1] WRONG — %s could not be created, on a mount "
		       "that is there\n", tag, path);
		failed++;
		return;
	}

	xf_header(hdr, XF_TARGET);
	while (off < XF_SIZE) {
		n = XF_SIZE - off < XF_WRITE_CHUNK ? XF_SIZE - off
						   : XF_WRITE_CHUNK;
		for (i = 0; i < n; i++)
			buf[i] = xf_expected(off + i, hdr);
		r = vfs_write(fd, buf, n);
		if (r != (ssize_t)n)
			break;
		off += n;
	}
	if (off < XF_SIZE) {
		printf("%s: [1] WRONG — the write at offset %u returned %ld "
		       "of %u bytes\n", tag, off, (long)r, n);
		failed++;
		(void)vfs_close(fd);
		return;
	}
	printf("%s: [1] wrote %u bytes to %s in %u-byte writes\n", tag,
	       XF_SIZE, path, XF_WRITE_CHUNK);
	passed++;

	/*
	 * 🔴 The arm #483 is about, on whichever target this runs.  A failed
	 * sync is a file that may exist here and nowhere else: what the other
	 * target reads is what reached the disk, not what this server holds.
	 */
	if (vfs_sync(fd) != 0) {
		printf("%s: [2] WRONG — the sync failed: what the other target "
		       "reads is what reached the disk, and this says it did "
		       "not\n", tag);
		failed++;
	} else {
		printf("%s: [2] the sync succeeded\n", tag);
		passed++;
	}

	/*
	 * ⚠️ This reads through the SAME server, so it can show that the server
	 * hands back what it was given and not that the disk holds it.  The
	 * disk is the other target's question, and the host's.
	 */
	xf_check_contents(fd, 3, 4);
	(void)vfs_close(fd);
}

static void
xf_read(const char *path)
{
	vfs_stat_t	st;
	vfs_fd_t	fd;

	if (vfs_stat(path, &st) != 0) {
		printf("%s: NOT ASKED — %s is not on this disk: nothing wrote "
		       "it for this boot to read\n", tag, path);
		return;
	}
	if (st.st_size != XF_SIZE) {
		printf("%s: [1] WRONG — %s is %llu bytes and the writer "
		       "wrote %u\n", tag, path,
		       (unsigned long long)st.st_size, XF_SIZE);
		failed++;
	} else {
		printf("%s: [1] %s is %u bytes, the size its writer "
		       "wrote\n", tag, path, XF_SIZE);
		passed++;
	}

	fd = vfs_open(path, VFS_O_RDONLY, 0);
	if (fd == VFS_FD_INVALID) {
		printf("%s: [2] WRONG — %s answers a stat and refuses an "
		       "open\n", tag, path);
		failed++;
		return;
	}
	xf_check_contents(fd, 2, 3);
	(void)vfs_close(fd);
}

int
main(int argc, char **argv)
{
	mach_port_t	host, device, wired, paged, security;
	char		dir[sizeof(netname_name_t)];
	kern_return_t	kr;
	int		writing;

	kr = bootstrap_ports(bootstrap_port, &host, &device, &wired, &paged,
			     &security);
	if (kr != KERN_SUCCESS)
		return 1;
	printf_init(device);

	if (argc != 3 ||
	    (strcmp(argv[1], "write") != 0 && strcmp(argv[1], "read") != 0)) {
		printf("xfile_test: WRONG — started with %d argument(s); the "
		       "line in bootstrap.conf must say `read PATH' or "
		       "`write PATH'\n", argc - 1);
		return 1;
	}
	writing = strcmp(argv[1], "write") == 0;
	tag = writing ? "xfile_write" : "xfile_read";

	printf("%s: started — %s %s, on %s (#498)\n", tag, argv[1], argv[2],
	       XF_TARGET);

	if (vfs_init() != 0) {
		printf("%s: WRONG — vfs_init failed\n", tag);
		failed++;
	} else if (xf_wait_for_ext_server() != 0) {
		failed++;
	} else if (!xf_mount_present(argv[2], dir, sizeof(dir))) {
		printf("%s: NOT ASKED — %s is not mounted on this boot, so "
		       "%s would land on another filesystem\n", tag, dir,
		       argv[2]);
		return 0;
	} else if (writing) {
		xf_write(argv[2]);
	} else {
		xf_read(argv[2]);
	}

	if (passed + failed != 0)
		printf("%s: %d of %d arms passed\n", tag, passed,
		       passed + failed);
	return failed == 0 ? 0 : 1;
}
