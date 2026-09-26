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
 * What fs_stat says about the file, field by field (#553).
 *
 * 🔑 THE STAT RECORD IS THE ROUTINE WITH #553'S SHAPE.  vfs_stat_t is 80 bytes
 * of 8-aligned fields, and while vfs.defs declared it as twenty 32-bit words
 * MIG put it four bytes away from where the compiler did.  The assertions
 * migcom generates now hold -- which proves the client and the server agree
 * about the layout, not that the layout is the one the server WRITES.  So the
 * fields are read back and checked against values this program knows, at
 * offsets spread across the record: a u64 at 8 and 16, u32s at 48, 52 and 64,
 * a byte at 68.  A record shifted by four bytes gets every one of them wrong.
 *
 * st_blocks is derived rather than copied: the file's data blocks, plus the
 * single-indirect block once there are more than twelve, in 512-byte units.
 * That holds for every ext2 block size from 1 KiB up, because 62674 bytes
 * never needs a double-indirect block.
 */
static void
xf_check_stat(const char *path, int arm)
{
	vfs_stat_t	st;
	uint64_t	nblk, want_blocks;
	const char	*bad = NULL;

	if (vfs_stat(path, &st) != 0) {
		printf("%s: [%d] WRONG — fs_stat(%s) failed on a file this "
		       "program has just %s\n", tag, arm, path,
		       strcmp(tag, "xfile_write") == 0 ? "written" : "opened");
		failed++;
		return;
	}

	want_blocks = 0;
	if (st.st_blksize >= 1024 && st.st_blksize <= 65536 &&
	    (st.st_blksize & (st.st_blksize - 1)) == 0) {
		nblk = (XF_SIZE + st.st_blksize - 1) / st.st_blksize;
		want_blocks = (nblk + (nblk > 12 ? 1 : 0)) *
			      (st.st_blksize / 512);
	}
	if (want_blocks == 0)
		bad = "st_blksize is not an ext2 block size";
	else if (st.st_size != XF_SIZE)
		bad = "st_size";
	else if (st.st_type != VFS_FT_REG)
		bad = "st_type is not a regular file";
	else if ((st.st_mode & 0777) != 0644)
		bad = "st_mode is not the 0644 the writer created it with";
	else if (st.st_nlink != 1)
		bad = "st_nlink";
	else if (st.st_ino == 0)
		bad = "st_ino is zero";
	else if (st.st_blocks != want_blocks)
		bad = "st_blocks is not what the size and block size make";

	if (bad != NULL) {
		printf("%s: [%d] WRONG — fs_stat(%s): %s — ino %llu size %llu "
		       "blocks %llu (expected %llu) mode 0%o nlink %u blksize "
		       "%u type %u\n", tag, arm, path, bad,
		       (unsigned long long)st.st_ino,
		       (unsigned long long)st.st_size,
		       (unsigned long long)st.st_blocks,
		       (unsigned long long)want_blocks,
		       (unsigned)st.st_mode, (unsigned)st.st_nlink,
		       (unsigned)st.st_blksize, (unsigned)st.st_type);
		failed++;
		return;
	}
	printf("%s: [%d] fs_stat reads back ino %llu, %llu bytes in %llu "
	       "sectors of %u-byte blocks, mode 0%o, one link, a regular file "
	       "(#553)\n", tag, arm, (unsigned long long)st.st_ino,
	       (unsigned long long)st.st_size,
	       (unsigned long long)st.st_blocks, (unsigned)st.st_blksize,
	       (unsigned)(st.st_mode & 0777));
	passed++;
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

/*
 * #599 X1: a block a write allocates starts as zeros.  A scratch file next to
 * `path' is filled with 0xA5 over XF_FRESH_BLOCKS blocks, synced and removed;
 * a second one gets one byte in the middle of each of as many blocks, and
 * every other byte of them must read back as zero.  The second file's blocks
 * are, as a rule, the first one's again, and that is the point: a partial
 * write of a fresh block read the block first, and got its last owner's
 * bytes from the cache or from the disk.
 */
#define XF_FRESH_BLOCKS	8u
#define XF_FRESH_BYTE	0x5Au
#define XF_OLD_BYTE	0xA5u

static int
xf_scratch_name(char *out, size_t len, const char *path, const char *leaf)
{
	const char	*slash = strrchr(path, '/');
	size_t		 dir;

	if (slash == NULL)
		return -1;
	dir = (size_t)(slash - path) + 1;
	if (dir + strlen(leaf) + 1 > len)
		return -1;
	memcpy(out, path, dir);
	strcpy(out + dir, leaf);
	return 0;
}

/* The step that failed, or 0 when the file is there and read back. */
static const char *
xf_fresh_setup(const char *a, const char *b, uint32_t bs, vfs_fd_t *fdp)
{
	unsigned char	one = XF_FRESH_BYTE;
	uint32_t	done, n, i;
	vfs_fd_t	fd;

	if (vfs_open_rc(a, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, 0644,
			&fd) != KERN_SUCCESS)
		return "creating the first file";
	memset(buf, XF_OLD_BYTE, sizeof(buf));
	for (done = 0; done < XF_FRESH_BLOCKS * bs; done += n) {
		n = XF_FRESH_BLOCKS * bs - done;
		if (n > sizeof(buf))
			n = sizeof(buf);
		if (vfs_write(fd, buf, n) != (ssize_t)n) {
			(void)vfs_close(fd);
			return "filling the first file";
		}
	}
	if (vfs_sync(fd) != 0) {
		(void)vfs_close(fd);
		return "syncing the first file";
	}
	(void)vfs_close(fd);
	if (vfs_unlink(a) != 0)
		return "removing the first file";

	if (vfs_open_rc(b, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, 0644,
			&fd) != KERN_SUCCESS)
		return "creating the second file";
	for (i = 0; i < XF_FRESH_BLOCKS; i++)
		if (vfs_lseek(fd, (off_t)(i * bs + bs / 2), VFS_SEEK_SET) !=
		    (off_t)(i * bs + bs / 2) || vfs_write(fd, &one, 1) != 1) {
			(void)vfs_close(fd);
			return "writing the second file";
		}
	if (vfs_lseek(fd, 0, VFS_SEEK_SET) != 0) {
		(void)vfs_close(fd);
		return "rewinding the second file";
	}
	*fdp = fd;
	return 0;
}

static void
xf_fresh_blocks(const char *path, int arm)
{
	char		 a[128], b[128];
	vfs_stat_t	 st;
	vfs_fd_t	 fd;
	const char	*step;
	uint32_t	 bs, size, off, n, i, bad = 0, first = 0;
	unsigned char	 first_val = 0, want;
	ssize_t		 r;

	if (vfs_stat(path, &st) != 0 || st.st_blksize < 1024 ||
	    st.st_blksize > 65536 ||
	    (st.st_blksize & (st.st_blksize - 1)) != 0 ||
	    xf_scratch_name(a, sizeof(a), path, "xf_fresh_a.dat") != 0 ||
	    xf_scratch_name(b, sizeof(b), path, "xf_fresh_b.dat") != 0) {
		printf("%s: [%d] WRONG — no block size or scratch names for "
		       "the fresh-block arm next to %s\n", tag, arm, path);
		failed++;
		return;
	}
	bs = (uint32_t)st.st_blksize;
	step = xf_fresh_setup(a, b, bs, &fd);
	if (step != 0) {
		printf("%s: [%d] WRONG — the fresh-block arm failed %s\n", tag,
		       arm, step);
		failed++;
		(void)vfs_unlink(b);
		return;
	}

	size = (XF_FRESH_BLOCKS - 1) * bs + bs / 2 + 1;
	for (off = 0; off < size; off += n) {
		n = size - off;
		if (n > sizeof(buf))
			n = sizeof(buf);
		r = vfs_read(fd, buf, n);
		if (r != (ssize_t)n)
			break;
		for (i = 0; i < n; i++) {
			want = (off + i) % bs == bs / 2 ? XF_FRESH_BYTE : 0;
			if (buf[i] != want && bad++ == 0) {
				first = off + i;
				first_val = buf[i];
			}
		}
	}
	(void)vfs_close(fd);
	(void)vfs_unlink(b);
	if (off < size) {
		printf("%s: [%d] WRONG — the read of %s at offset %u returned "
		       "%ld of %u bytes\n", tag, arm, b, off, (long)r, n);
		failed++;
	} else if (bad != 0) {
		printf("%s: [%d] WRONG — %u bytes of %u fresh blocks are not "
		       "what was written, the first at offset %u (0x%02x): a "
		       "new block kept its last owner's bytes (#599)\n", tag,
		       arm, bad, XF_FRESH_BLOCKS, first, first_val);
		failed++;
	} else {
		printf("%s: [%d] a byte in each of %u fresh blocks, and every "
		       "other byte of them reads 0 -- none of the removed "
		       "file's 0x%02x (#599)\n", tag, arm, XF_FRESH_BLOCKS,
		       XF_OLD_BYTE);
		passed++;
	}
}

/*
 * #599 X2: a directory made on the blocks of a file removed unsynced works.
 * A scratch file next to `path' is written over XF_FRESH_BLOCKS blocks and
 * removed with no sync, so its blocks go back to the bitmap with their bytes
 * still in the cache, dirty; a directory made next, which as a rule takes one
 * of them, must then take a file that can be created and looked up -- before
 * a sync and after it.
 *
 * ⚠️ Since ee2c26e2 this no longer shows the discard it was written for: the
 * directory's first block goes to the disk through write_data_block, whose
 * page_cache_wrote replaces the stale copy, so the arm passes with
 * block_free's and block_alloc's discards taken out (found in review).  The
 * discard's own arm is X4 below, through a block written OUTSIDE the cache.
 */
#define XF_DEAD_BYTE	0xC7u

/* The step that failed, with its answer in *rc, or 0. */
static const char *
xf_owner_steps(const char *dead, const char *dir, const char *inner,
	       uint32_t bs, int *rc)
{
	uint32_t	done, n;
	vfs_fd_t	fd;

	*rc = vfs_open_rc(dead, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, 0644,
			  &fd);
	if (*rc != KERN_SUCCESS)
		return "creating the file to remove";
	memset(buf, XF_DEAD_BYTE, sizeof(buf));
	for (done = 0; done < XF_FRESH_BLOCKS * bs; done += n) {
		n = XF_FRESH_BLOCKS * bs - done;
		if (n > sizeof(buf))
			n = sizeof(buf);
		if (vfs_write(fd, buf, n) != (ssize_t)n) {
			(void)vfs_close(fd);
			*rc = -1;
			return "writing the file to remove";
		}
	}
	(void)vfs_close(fd);
	if ((*rc = vfs_unlink(dead)) != 0)
		return "removing the file, unsynced";
	if ((*rc = vfs_mkdir(dir, 0755)) != 0)
		return "making the directory";
	*rc = vfs_open_rc(inner, VFS_O_RDWR | VFS_O_CREAT, 0644, &fd);
	if (*rc != KERN_SUCCESS)
		return "creating a file in the new directory";
	(void)vfs_close(fd);
	{
		vfs_stat_t st;

		if ((*rc = vfs_stat(inner, &st)) != 0)
			return "looking the file up, before a sync";
	}
	*rc = vfs_open_rc(inner, VFS_O_RDONLY, 0, &fd);
	if (*rc != KERN_SUCCESS)
		return "opening the file to sync";
	*rc = vfs_sync(fd);
	(void)vfs_close(fd);
	if (*rc != 0)
		return "syncing";
	{
		vfs_stat_t st;

		if ((*rc = vfs_stat(inner, &st)) != 0)
			return "looking the file up, after the sync";
	}
	return 0;
}

static void
xf_owner_change(const char *path, int arm)
{
	char		 dead[128], dir[128], inner[128];
	vfs_stat_t	 st;
	const char	*step;
	uint32_t	 bs;
	int		 rc = 0;

	if (vfs_stat(path, &st) != 0 || st.st_blksize < 1024 ||
	    st.st_blksize > 65536 ||
	    (st.st_blksize & (st.st_blksize - 1)) != 0 ||
	    xf_scratch_name(dead, sizeof(dead), path, "xf_dead.dat") != 0 ||
	    xf_scratch_name(dir, sizeof(dir), path, "xf_dir") != 0 ||
	    xf_scratch_name(inner, sizeof(inner), path,
			    "xf_dir/inner") != 0) {
		printf("%s: [%d] WRONG — no block size or scratch names for "
		       "the owner-change arm next to %s\n", tag, arm, path);
		failed++;
		return;
	}
	bs = (uint32_t)st.st_blksize;
	/* Leftovers of an earlier boot on the same disk */
	(void)vfs_unlink(inner);
	(void)vfs_rmdir(dir);
	(void)vfs_unlink(dead);

	step = xf_owner_steps(dead, dir, inner, bs, &rc);
	(void)vfs_unlink(inner);
	(void)vfs_rmdir(dir);
	if (step != 0) {
		printf("%s: [%d] WRONG — %s failed (0x%x), after a file's "
		       "blocks were freed with their bytes unsynced (#599)\n",
		       tag, arm, step, (unsigned)rc);
		failed++;
	} else {
		printf("%s: [%d] a directory made on the blocks of a file "
		       "removed unsynced takes a file, found before a sync "
		       "and after it (#599)\n", tag, arm);
		passed++;
	}
}

/*
 * #599 X3: an inode written through a file kept open does not undo another
 * inode written since in the same inode-table block.  A file is created and
 * kept open; a second one is created, written and closed; then the first is
 * written and synced.  Both stay on the disk: the host's e2fsck finds a name
 * pointing at an unused inode if the first file's flush wrote back a copy of
 * the block taken when it was opened.  The arm itself can only say what it
 * did -- this server answers a stat of the second file from its caches.
 */
static void
xf_neighbour_inodes(const char *path, int arm)
{
	char		 a[128], b[128];
	unsigned char	 one = 0x11;
	vfs_stat_t	 st;
	vfs_fd_t	 fa = -1, fb;
	const char	*step = 0;

	if (xf_scratch_name(a, sizeof(a), path, "xf_keep_a.dat") != 0 ||
	    xf_scratch_name(b, sizeof(b), path, "xf_keep_b.dat") != 0) {
		printf("%s: [%d] WRONG — no scratch names next to %s\n", tag,
		       arm, path);
		failed++;
		return;
	}
	(void)vfs_unlink(a);
	(void)vfs_unlink(b);
	if (vfs_open_rc(a, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, 0644,
			&fa) != KERN_SUCCESS)
		step = "creating the file kept open";
	else if (vfs_write(fa, &one, 1) != 1)
		step = "writing the file kept open";
	else if (vfs_open_rc(b, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, 0644,
			     &fb) != KERN_SUCCESS)
		step = "creating the second file";
	else {
		if (vfs_write(fb, &one, 1) != 1 || vfs_sync(fb) != 0)
			step = "writing the second file";
		(void)vfs_close(fb);
		if (step == 0 && (vfs_write(fa, &one, 1) != 1 ||
				  vfs_sync(fa) != 0))
			step = "writing the file kept open, again";
		if (step == 0 && vfs_stat(b, &st) != 0)
			step = "looking the second file up";
	}
	if (fa >= 0)
		(void)vfs_close(fa);
	if (step != 0) {
		printf("%s: [%d] WRONG — %s failed\n", tag, arm, step);
		failed++;
		return;
	}
	printf("%s: [%d] %s kept open across the making of %s, then written "
	       "and synced; both left for the host's e2fsck (#599)\n", tag,
	       arm, a, b);
	passed++;
}

/*
 * #599 X4: a block that changes owner leaves nothing of its old owner in the
 * cache -- the discard in block_free and block_alloc, seen through a block
 * this server writes to the disk directly: an indirect block (indirect_set's
 * write_disk_block).
 *
 * A scratch file of XF_DEAD_I_BLOCKS data blocks -- no indirect block -- is
 * written and removed with no sync, so its blocks go back to the bitmap with
 * their bytes still cached, dirty.  A second file then writes ONLY its file
 * block 12 and 13: the allocator, lowest free first, hands out the data
 * block for 12 and, right after it, the single-indirect block -- both from
 * the removed file's blocks.  The data blocks go through the cache and
 * replace their stale copies; the indirect block does not.  With the stale
 * copy left in the cache, the sync writes the removed file's bytes over the
 * indirect block, and the second file, opened again (so no private copy of
 * the map answers), cannot read blocks 12 and 13.
 *
 * ⚠️ The first version wrote the second file from block 0, and its indirect
 * block landed on the removed file's own indirect block, which was never in
 * the cache: it passed with the discards taken out (found in review).
 */
#define XF_DEAD_I_BLOCKS	12u
#define XF_IND_FIRST		12u	/* the first block through the indirect */
#define XF_IND_COUNT		2u

static unsigned char
xf_ind_byte(uint32_t block)
{
	return (unsigned char)(0x21u + block);
}

/* The step that failed, or 0 when the second file reads back whole. */
static const char *
xf_indirect_steps(const char *dead, const char *ind, uint32_t bs,
		  uint32_t *bad_block)
{
	uint32_t	done, n, b, i;
	vfs_fd_t	fd;
	ssize_t		r;

	if (vfs_open_rc(dead, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, 0644,
			&fd) != KERN_SUCCESS)
		return "creating the file to remove";
	memset(buf, XF_DEAD_BYTE, sizeof(buf));
	for (done = 0; done < XF_DEAD_I_BLOCKS * bs; done += n) {
		n = XF_DEAD_I_BLOCKS * bs - done;
		if (n > sizeof(buf))
			n = sizeof(buf);
		if (vfs_write(fd, buf, n) != (ssize_t)n) {
			(void)vfs_close(fd);
			return "writing the file to remove";
		}
	}
	(void)vfs_close(fd);
	if (vfs_unlink(dead) != 0)
		return "removing the file, unsynced";

	if (vfs_open_rc(ind, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, 0644,
			&fd) != KERN_SUCCESS)
		return "creating the file with an indirect block";
	if (vfs_lseek(fd, (off_t)(XF_IND_FIRST * bs), VFS_SEEK_SET) !=
	    (off_t)(XF_IND_FIRST * bs)) {
		(void)vfs_close(fd);
		return "seeking to its first indirect block";
	}
	for (b = XF_IND_FIRST; b < XF_IND_FIRST + XF_IND_COUNT; b++)
		for (done = 0; done < bs; done += n) {
			n = bs - done;
			if (n > sizeof(buf))
				n = sizeof(buf);
			memset(buf, xf_ind_byte(b), n);
			if (vfs_write(fd, buf, n) != (ssize_t)n) {
				(void)vfs_close(fd);
				return "writing through its indirect block";
			}
		}
	if (vfs_sync(fd) != 0) {
		(void)vfs_close(fd);
		return "syncing";
	}
	(void)vfs_close(fd);

	if (vfs_open_rc(ind, VFS_O_RDONLY, 0, &fd) != KERN_SUCCESS)
		return "opening it again";
	if (vfs_lseek(fd, (off_t)(XF_IND_FIRST * bs), VFS_SEEK_SET) !=
	    (off_t)(XF_IND_FIRST * bs)) {
		(void)vfs_close(fd);
		return "seeking to its first indirect block again";
	}
	for (b = XF_IND_FIRST; b < XF_IND_FIRST + XF_IND_COUNT; b++)
		for (done = 0; done < bs; done += n) {
			n = bs - done;
			if (n > sizeof(buf))
				n = sizeof(buf);
			r = vfs_read(fd, buf, n);
			if (r != (ssize_t)n) {
				(void)vfs_close(fd);
				*bad_block = b;
				return "reading it back";
			}
			for (i = 0; i < n; i++)
				if (buf[i] != xf_ind_byte(b)) {
					(void)vfs_close(fd);
					*bad_block = b;
					return "comparing what it read";
				}
		}
	(void)vfs_close(fd);
	return 0;
}

static void
xf_indirect_owner(const char *path, int arm)
{
	char		 dead[128], ind[128];
	vfs_stat_t	 st;
	const char	*step;
	uint32_t	 bs, bad = 0;

	if (vfs_stat(path, &st) != 0 || st.st_blksize < 1024 ||
	    st.st_blksize > 65536 ||
	    (st.st_blksize & (st.st_blksize - 1)) != 0 ||
	    xf_scratch_name(dead, sizeof(dead), path, "xf_dead_i.dat") != 0 ||
	    xf_scratch_name(ind, sizeof(ind), path, "xf_ind.dat") != 0) {
		printf("%s: [%d] WRONG — no block size or scratch names for "
		       "the indirect-block arm next to %s\n", tag, arm, path);
		failed++;
		return;
	}
	bs = (uint32_t)st.st_blksize;
	(void)vfs_unlink(dead);		/* an earlier boot's */
	(void)vfs_unlink(ind);
	step = xf_indirect_steps(dead, ind, bs, &bad);
	(void)vfs_unlink(ind);
	if (step != 0 && bad != 0) {
		printf("%s: [%d] WRONG — %s failed at file block %u: a file "
		       "written through its indirect block, on the blocks of "
		       "one removed unsynced, did not read back after a sync "
		       "(#599)\n", tag, arm, step, bad);
		failed++;
	} else if (step != 0) {
		printf("%s: [%d] WRONG — the indirect-block arm failed %s "
		       "(#599)\n", tag, arm, step);
		failed++;
	} else {
		printf("%s: [%d] file blocks %u..%u written through an indirect "
		       "block, on the blocks of a file removed unsynced, read "
		       "back after a sync, opened again (#599)\n", tag, arm,
		       XF_IND_FIRST, XF_IND_FIRST + XF_IND_COUNT - 1);
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
	kern_return_t	rc;

	rc = vfs_open_rc(path, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, 0644,
			 &fd);
	if (rc != KERN_SUCCESS) {
		printf("%s: [1] WRONG — %s could not be created, on a mount "
		       "that is there (0x%x)\n", tag, path, (unsigned)rc);
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
	xf_check_stat(path, 5);
	xf_fresh_blocks(path, 6);
	xf_owner_change(path, 7);
	xf_neighbour_inodes(path, 8);
	xf_indirect_owner(path, 9);
}

static void
xf_read(const char *path)
{
	vfs_stat_t	st;
	vfs_fd_t	fd;
	int		rc;

	/*
	 * #599: NOT ASKED only for a name that is not there.  Any other
	 * failure is an answer about the disk, and it was reported as this:
	 * on the entry-16 boot a directory refused as damaged read as "not on
	 * this disk".  libvfs now says which (VFS_ERR_NOENT, or the reason),
	 * and the value is printed as read.
	 */
	rc = vfs_stat(path, &st);
	if (rc == VFS_ERR_NOENT) {
		printf("%s: NOT ASKED — %s is not on this disk: nothing wrote "
		       "it for this boot to read\n", tag, path);
		return;
	}
	if (rc != 0) {
		printf("%s: [1] WRONG — a stat of %s failed with 0x%x, which "
		       "is not \"absent\": the disk answered, and not with "
		       "the file\n", tag, path, (unsigned)rc);
		failed++;
		return;
	}
	xf_check_stat(path, 1);

	rc = vfs_open_rc(path, VFS_O_RDONLY, 0, &fd);
	if (rc != KERN_SUCCESS) {
		printf("%s: [2] WRONG — %s answers a stat and refuses an "
		       "open (0x%x)\n", tag, path, (unsigned)rc);
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
