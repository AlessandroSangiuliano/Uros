/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */
/*
 * MkLinux
 */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 * OLD HISTORY
 * Revision 2.8  93/05/10  19:40:24  rvb
 * 	Changed "" includes to <>
 * 	[93/04/30            mrt]
 * 
 * Revision 2.7  93/05/10  17:44:48  rvb
 * 	Include files specified with quotes dont work properly
 * 	when the C file in in the master directory but the
 * 	include file is in the shadow directory. Change to
 * 	using angle brackets.
 * 	Ian Dall <DALL@hfrd.dsto.gov.au>	4/28/93
 * 	[93/05/10  13:15:31  rvb]
 * 
 * Revision 2.6  93/01/14  17:09:10  danner
 * 	64bit clean.  Just type-bug fixes, actually.
 * 	[92/11/30            af]
 * 
 * Revision 2.5  92/03/01  00:39:32  rpd
 * 	Fixed device_get_status argument types.
 * 	[92/02/29            rpd]
 * 
 * Revision 2.4  92/02/23  22:25:43  elf
 * 	Removed debugging printf.
 * 	[92/02/23  13:16:45  af]
 * 
 * 	Added variation of file_direct to page on raw devices.
 * 	[92/02/22  18:53:04  af]
 * 
 * 	Added remove_file_direct().  Commented out some unused code.
 * 	[92/02/19  17:31:01  af]
 * 
 * Revision 2.3  92/01/24  18:14:31  rpd
 * 	Added casts to device_read dataCnt argument to mollify
 * 	buggy versions of gcc.
 * 	[92/01/24            rpd]
 * 
 * Revision 2.2  92/01/03  19:57:13  dbg
 * 	Make file_direct self-contained: add the few fields needed from
 * 	the superblock.
 * 	[91/10/17            dbg]
 * 
 * 	Deallocate unused superblocks when switching file systems.  Add
 * 	file_close routine to free space taken by file metadata.
 * 	[91/09/25            dbg]
 * 
 * 	Move outside of kernel.
 * 	Unmodify open_file to put arrays back on stack.
 * 	[91/09/04            dbg]
 * 
 * Revision 2.8  91/08/28  11:09:42  jsb
 * 	Added struct file_direct and associated functions.
 * 	[91/08/19            rpd]
 * 
 * Revision 2.7  91/07/31  17:24:07  dbg
 * 	Call vm_wire instead of vm_pageable.
 * 	[91/07/30  16:38:01  dbg]
 * 
 * Revision 2.6  91/05/18  14:28:45  rpd
 * 	Changed block_map to avoid blocking operations
 * 	while holding an exclusive lock.
 * 	[91/04/06            rpd]
 * 	Added locking in block_map.
 * 	[91/04/03            rpd]
 * 
 * Revision 2.5  91/05/14  15:22:53  mrt
 * 	Correcting copyright
 * 
 * Revision 2.4  91/02/05  17:01:23  mrt
 * 	Changed to new copyright
 * 	[91/01/28  14:54:52  mrt]
 * 
 * Revision 2.3  90/10/25  14:41:42  rwd
 * 	Modified open_file to allocate arrays from heap, not stack.
 * 	[90/10/23            rpd]
 * 
 * Revision 2.2  90/08/27  21:45:27  dbg
 * 	Reduce lint.
 * 	[90/08/13            dbg]
 * 
 * 	Created from include files, bsd4.3-Reno (public domain) source,
 * 	and old code written at CMU.
 * 	[90/07/17            dbg]
 * 
 */
/*
 * Stand-alone EXT2FS file reading package.
 */

#include <ext2fs/ext2fs.h>
#include "externs.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <device/device_types.h>
#include <device/device.h>
#include <page_cache.h>
#include <blk.h>

/*
 * MIG user stubs from ahci_batch.defs (subsystem 2950).
 * Weak: resolve to NULL for binaries that don't link the stubs (e.g.
 * bootstrap), causing a transparent fallback to individual I/O.
 */
extern kern_return_t device_write_batch(
	mach_port_t device,
	dev_mode_t mode,
	recnum_t *recnums, mach_msg_type_number_t recnumsCnt,
	unsigned int *sizes, mach_msg_type_number_t sizesCnt,
	io_buf_ptr_t data, mach_msg_type_number_t dataCnt,
	io_buf_len_t *bytes_written) __attribute__((weak));

extern kern_return_t device_read_phys(
	mach_port_t device,
	dev_mode_t mode, recnum_t recnum,
	io_buf_len_t bytes_wanted,
	vm_address_t *phys_addrs, mach_msg_type_number_t phys_addrsCnt,
	io_buf_len_t *bytes_read) __attribute__((weak));

extern kern_return_t device_write_phys(
	mach_port_t device,
	dev_mode_t mode, recnum_t recnum,
	io_buf_len_t bytes_to_write,
	vm_address_t *phys_addrs, mach_msg_type_number_t phys_addrsCnt,
	io_buf_len_t *bytes_written) __attribute__((weak));

/* ================================================================
 * Block I/O dispatch helpers
 *
 * If the device has a blk_dev handle, dispatch through libblk.
 * Otherwise fall back to direct device_read/write (bootstrap path).
 * ================================================================ */

/*
 * 🔴 THE COUNT IS CONVERTED HERE, AND NOWHERE ELSE (#498).
 *
 * The device hands back an out-of-line buffer with a MIG count, which is a
 * mach_msg_type_number_t -- four bytes on both targets.  Everything in this
 * file keeps a buffer's size as a vm_size_t, because that is what
 * vm_deallocate() takes -- four bytes on i386 and EIGHT on x86-64.
 *
 * The two used to meet at the call sites, as `(unsigned int *)&size': a
 * pointer cast that tells the device to write four bytes into an eight-byte
 * variable.  The upper half kept whatever was on the stack, and the
 * vm_deallocate() that followed released a region billions of bytes long.
 * The first create on x86-64 did that to a block bitmap it had just read,
 * unmapped its own text, and died returning from the trap -- at the
 * instruction after `syscall' in syscall_vm_deallocate.
 *
 * 🔑 It survived on i386 because the widths are equal there, and in most
 * places on x86-64 because the variable happened to live in a struct that
 * open had zeroed.  So the conversion lives in the one function every read
 * goes through, the callers pass their own type, and a caller that passes
 * anything else is a compiler error rather than a cast.
 */
static inline kern_return_t
ext2_dev_read(struct device *dev, recnum_t recnum,
	      io_buf_len_t bytes_wanted,
	      io_buf_ptr_t *data, vm_size_t *bytes_read)
{
	mach_msg_type_number_t count = 0;
	kern_return_t kr;

	if (dev->blk)
		kr = blk_read(dev->blk, recnum, bytes_wanted, data, &count);
	else
		kr = device_read(dev->dev_port, 0, recnum,
				 (int)bytes_wanted, data, &count);
	/*
	 * #599: every byte asked for, or an error.  A short answer was
	 * handed on as a read, and readahead built cache entries out of the
	 * bytes past it.  The out-of-line buffer goes back here.
	 */
	if (kr == KERN_SUCCESS && (io_buf_len_t)count < bytes_wanted) {
		if (count != 0)
			(void) vm_deallocate(mach_task_self(),
					     (vm_offset_t)*data, count);
		*data = 0;
		kr = D_IO_ERROR;
	}
	if (kr == KERN_SUCCESS)
		*bytes_read = (vm_size_t)count;
	return kr;
}

static inline kern_return_t
ext2_dev_write(struct device *dev, recnum_t recnum,
	       io_buf_ptr_t data, mach_msg_type_number_t data_count,
	       io_buf_len_t *bytes_written)
{
	if (dev->blk)
		return blk_write(dev->blk, recnum, data, data_count,
				 bytes_written);
	return device_write(dev->dev_port, 0, recnum,
			    data, data_count, (int *)bytes_written);
}

/* The same conversion as ext2_dev_read(), for the same reason. */
static inline kern_return_t
ext2_dev_read_overwrite(struct device *dev, recnum_t recnum,
			io_buf_len_t bytes_wanted,
			vm_offset_t buffer,
			vm_size_t *bytes_read)
{
	mach_msg_type_number_t count = 0;
	kern_return_t kr;

	if (dev->blk)
		kr = blk_read_overwrite(dev->blk, recnum, bytes_wanted,
					buffer, &count);
	else
		kr = device_read_overwrite(dev->dev_port, 0, recnum,
					   bytes_wanted, buffer, &count);
	if (kr == KERN_SUCCESS && (io_buf_len_t)count < bytes_wanted)
		kr = D_IO_ERROR;		/* #599: every byte, or an error */
	if (kr == KERN_SUCCESS)
		*bytes_read = (vm_size_t)count;
	return kr;
}

static inline int
ext2_dev_has_phys(struct device *dev)
{
	if (dev->blk)
		return blk_has_phys(dev->blk);
	return (device_read_phys != NULL);
}

static inline kern_return_t
ext2_dev_read_phys(struct device *dev, recnum_t recnum,
		   io_buf_len_t bytes_wanted,
		   vm_address_t *phys_addrs, unsigned int n_phys,
		   io_buf_len_t *bytes_read)
{
	if (dev->blk)
		return blk_read_phys(dev->blk, recnum, bytes_wanted,
				     phys_addrs, n_phys, bytes_read);
	return device_read_phys(dev->dev_port, 0, recnum, bytes_wanted,
				phys_addrs, n_phys, bytes_read);
}

static inline kern_return_t
ext2_dev_write_phys(struct device *dev, recnum_t recnum,
		    io_buf_len_t bytes_to_write,
		    vm_address_t *phys_addrs, unsigned int n_phys,
		    io_buf_len_t *bytes_written)
{
	if (dev->blk)
		return blk_write_phys(dev->blk, recnum, bytes_to_write,
				      phys_addrs, n_phys, bytes_written);
	return device_write_phys(dev->dev_port, 0, recnum, bytes_to_write,
				 phys_addrs, n_phys, bytes_written);
}

static inline int
ext2_dev_has_batch(struct device *dev)
{
	if (dev->blk)
		return blk_has_batch(dev->blk);
	return (device_write_batch != NULL);
}

/*
 * Historical no-op mutexes from the single-threaded boot-loader origin
 * of this file.  They guard nothing (the f_lock field they nominally
 * took does not even exist) and are kept only so stray call sites keep
 * compiling.  Real serialization (#384) is the vnode lock below.
 */
#define mutex_lock(a)
#define mutex_unlock(a)
#define mutex_init(a)

/*
 * #384: real locks.  ext_server drives this library from many threads
 * (MIG pool, FLIPC fast-path, writeback thread), so the shared state
 * needs actual mutual exclusion:
 *
 *  - vnode v_lock (per-inode): block-map walks (block_map), block-map
 *    mutation (write-extend, truncate), and metadata flush.  Taken via
 *    vnode_mutex_lock()/unlock(); no-op for path-walk handles that have
 *    no vnode yet (their f_ic_scratch is private).
 *  - ext2_alloc_lock (global): the block/inode allocator's read-modify-
 *    write of the shared bitmaps + group descriptors + superblock
 *    counters.  Concurrent allocators used to lose each other's bitmap
 *    updates (double-allocated blocks).  Leaf lock: nothing else is
 *    taken while holding it.
 *  - ext2_vnode_table_lock (global): vnode_get/put refcounts.
 *  - ext2_itable_lock (global, #599): the read-modify-write of an
 *    inode-table block -- write_new_inode, and a vnode's flush of its own
 *    inode.  Each reads the block afresh, changes one slot and writes the
 *    block back while holding it: a block holds many inodes, and a copy
 *    written back without being read again undid its neighbours.  Leaf.
 *  - ext2_icache_lock (global, #599): every mount's inode cache.  The
 *    writeback thread's flushes write it (icache_follow) while path walks
 *    on the MIG thread read and fill it (read_inode), and an entry is a
 *    number and 128 bytes: read unlocked, one inode's fields came under
 *    another's number (found in review).  Leaf.
 *
 * Lock order: v_lock -> ext2_alloc_lock (write-extend allocates while
 * holding the vnode); v_lock -> ext2_itable_lock (flush); v_lock -> pc_lock
 * (page cache) via the data path; v_lock -> ext2_icache_lock (flush).
 * Never the reverse.
 */
static pthread_mutex_t ext2_icache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ext2_alloc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ext2_itable_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ext2_vnode_table_lock = PTHREAD_MUTEX_INITIALIZER;

static void
vnode_mutex_lock(struct ext2fs_file *fp)
{
	if (fp->f_vnode)
		pthread_mutex_lock(&fp->f_vnode->v_lock);
}

static void
vnode_mutex_unlock(struct ext2fs_file *fp)
{
	if (fp->f_vnode)
		pthread_mutex_unlock(&fp->f_vnode->v_lock);
}

/*
 * #599: the block one call is reading through buf_read_file, and the buffer
 * its bytes are in.  It lives on the caller's stack for that call and no
 * longer.  The handle used to keep it between calls, and a handle is not one
 * caller's: the MIG and the FLIPC paths serve the same open file, so one
 * call released and refilled the buffer while the other was still reading
 * it.  Each caller of buf_read_file is a wrapper that starts a bref and
 * releases it on its only way out.
 */
struct ext2_bref {
	struct page_cache_entry *br_entry; /* the page-cache slot br_data
					   is in, pinned, or NULL */
	vm_offset_t	br_priv;	/* a buffer of our own, or 0 */
	vm_size_t	br_priv_size;
	vm_offset_t	br_data;	/* the block's bytes: br_entry's
					   slot, or br_priv */
	daddr_t		br_fblock;	/* the file block they are, or -1 */
};

static void
bref_init(struct ext2_bref *br)
{
	br->br_entry = NULL;
	br->br_priv = 0;
	br->br_priv_size = 0;
	br->br_data = 0;
	br->br_fblock = -1;
}

/*
 * The buffer goes and its block number with it, in one place.  A release
 * that kept the number let the next read inside that block find it held,
 * answer 0 + off, and have its caller dereference that.  A slot is given
 * back to the cache, which may reuse it from then on.
 */
static void
bref_release(struct ext2fs_file *fp, struct ext2_bref *br)
{
	if (br->br_entry != NULL)
		page_cache_put(fp->f_dev.cache, br->br_entry);
	if (br->br_priv != 0)
		(void) vm_deallocate(mach_task_self(), br->br_priv,
				     br->br_priv_size);
	bref_init(br);
}

/*
 * #384: drop this handle's private caches of the shared block map —
 * the indirect-block buffers (f_blk[]).  Used when the shared map
 * changed underneath them: the cached blocks may have been freed and
 * re-allocated, and walking a stale indirect buffer reads file DATA as
 * an indirect table.
 */
static void
handle_caches_drop(struct ext2fs_file *fp)
{
	int level;

	for (level = 0; level < NIADDR; level++) {
		if (fp->f_blk[level] != 0) {
			(void) vm_deallocate(mach_task_self(),
					     fp->f_blk[level],
					     fp->f_blksize[level]);
			fp->f_blk[level] = 0;
			fp->f_blksize[level] = 0;
		}
		fp->f_blkno[level] = -1;
	}
	fp->f_ra_last_block = -1;
}

/*
 * #384: called with v_lock held.  If another opener mutated the shared
 * block map since this handle last looked (truncate freed it, or a
 * write extended it), drop the private caches and adopt the current
 * generation.
 */
static void
vnode_gen_check(struct ext2fs_file *fp)
{
	if (!fp->f_vnode || fp->f_gen == fp->f_vnode->v_gen)
		return;
	handle_caches_drop(fp);
	fp->f_gen = fp->f_vnode->v_gen;
}

/*
 * #384: called with v_lock held, after mutating the shared block map.
 * Invalidates every OTHER handle's private caches (they re-sync in
 * vnode_gen_check) while keeping this handle current.
 */
static void
vnode_gen_bump(struct ext2fs_file *fp)
{
	if (fp->f_vnode) {
		fp->f_vnode->v_gen++;
		fp->f_gen = fp->f_vnode->v_gen;
	}
}

static void free_file_buffers(
		struct ext2fs_file *);

static int read_inode(
		ino_t,
		struct ext2fs_file *);

static int block_map(
		struct ext2fs_file *,
		daddr_t,
		daddr_t *);

static int buf_read_file(
		struct ext2fs_file *,
		struct ext2_bref *,
		vm_offset_t,
		vm_offset_t *,
		vm_size_t *);

static int write_file_locked(
		struct ext2fs_file *,
		vm_offset_t,
		vm_offset_t,
		vm_size_t);

static void ext2_selftest_dir(
		struct ext2fs_file *,
		struct page_cache *,
		unsigned int *,
		unsigned int *);

static int search_directory(
		char *,
	        struct ext2fs_file *,
		ino_t *);

static int ext2_dirent_check(
		const struct ext2fs_file *,
		const struct ext2_dir_entry *,
		vm_size_t,
		vm_offset_t);

static int read_fs(
		struct device *,
		struct ext2_super_block **,
		struct ext2_group_desc  **,
		vm_size_t *);

static int mount_fs(
		struct ext2fs_file *);

static void unmount_fs(
		struct ext2fs_file *);

static int write_super(
		struct ext2fs_file *);

/*
 * Per-mount filesystem state.
 * One instance per mounted partition, hung off struct device.mount_data.
 * Contains cached superblock, group descriptors, inode/vnode/dcache tables.
 *
 * Allocated dynamically by the filesystem dispatch layer (file_system.c)
 * via vm_allocate using fs_ops.mount_size.  Callers that bypass the
 * dispatch layer (e.g. ext_server) get an on-demand vm_allocate in
 * ext2_get_mount().
 */

#define ICACHE_SIZE	16		/* must be power of 2 */
#define ICACHE_HASH(ino) ((ino) & (ICACHE_SIZE - 1))

struct icache_entry {
	ino_t			ic_ino;		/* 0 = empty */
	struct ext2_inode	ic_inode;	/* cached inode data */
};

#define VNODE_TABLE_SIZE	32

#define DCACHE_SIZE	32		/* must be power of 2 */
#define DCACHE_NAME_MAX	60
#define DCACHE_NEGATIVE	((ino_t)-1)	/* sentinel for "known absent" */

/* dcache_lookup return codes */
#define DCACHE_MISS	0	/* not in cache */
#define DCACHE_HIT	1	/* positive hit, *ino_out set */
#define DCACHE_NEG	2	/* negative hit, name does not exist */

struct dcache_entry {
	ino_t		dc_parent;	/* 0 = empty */
	ino_t		dc_child;	/* DCACHE_NEGATIVE = negative entry */
	char		dc_name[DCACHE_NAME_MAX];
};

struct ext2_mount {
	struct ext2_super_block	*m_fs;
	struct ext2_group_desc	*m_gd;
	vm_size_t		 m_gd_size;
	int			 m_nindir[NIADDR];
	struct icache_entry	 m_icache[ICACHE_SIZE];
	struct ext2_vnode	 m_vnode_table[VNODE_TABLE_SIZE];
	struct dcache_entry	 m_dcache[DCACHE_SIZE];
};

int ext2fs_readdir(fs_private_t, struct fs_dirent *,
		   unsigned int, unsigned int *);

struct fs_ops ext2fs_ops = {
	ext2fs_open_file,
	ext2fs_close_file,
	ext2fs_read_file,
	ext2fs_file_size,
	ext2fs_file_is_directory,
	ext2fs_file_is_executable,
	ext2fs_readdir,
	sizeof(struct ext2_mount)
};

/*
 * Return the per-mount state for this device.
 * The dispatch layer pre-allocates mount_data via fs_ops.mount_size;
 * if called directly (ext_server), fall back to vm_allocate.
 */
static struct ext2_mount *
ext2_get_mount(struct device *dev)
{
	if (dev->mount_data)
		return (struct ext2_mount *)dev->mount_data;

	if (vm_allocate(mach_task_self(),
			(vm_address_t *)&dev->mount_data,
			sizeof(struct ext2_mount), TRUE) != KERN_SUCCESS)
		return NULL;

	return (struct ext2_mount *)dev->mount_data;
}

/*
 * Inode cache — per-mount, accessed via ext2_mount.
 */

/*
 * #599: every access under ext2_icache_lock, and a generation that moves
 * whenever the cache is told something newer than a disk read could know --
 * a flush's inode (icache_follow) or an invalidation.  read_inode takes the
 * generation before its disk read and fills the cache only if it has not
 * moved: otherwise a walk that read the disk before a flush put the
 * pre-flush inode back after the flush's (found in review).
 */
static unsigned int ext2_icache_gen;

static int
icache_get(struct ext2_mount *m, ino_t ino, struct ext2_inode *out,
	   unsigned int *gen)
{
	struct icache_entry *e = &m->m_icache[ICACHE_HASH(ino)];
	int hit = 0;

	pthread_mutex_lock(&ext2_icache_lock);
	if (e->ic_ino == ino) {
		*out = e->ic_inode;
		hit = 1;
	}
	*gen = ext2_icache_gen;
	pthread_mutex_unlock(&ext2_icache_lock);
	return hit;
}

/* A disk read's inode: kept only if nothing newer was said since `gen'. */
static void
icache_fill(struct ext2_mount *m, ino_t ino, const struct ext2_inode *inode,
	    unsigned int gen)
{
	struct icache_entry *e = &m->m_icache[ICACHE_HASH(ino)];

	pthread_mutex_lock(&ext2_icache_lock);
	if (gen == ext2_icache_gen) {
		e->ic_ino = ino;
		e->ic_inode = *inode;
	}
	pthread_mutex_unlock(&ext2_icache_lock);
}

/* What a flush wrote: newer than any disk read in flight. */
static void
icache_insert(struct ext2_mount *m, ino_t ino, const struct ext2_inode *inode)
{
	struct icache_entry *e = &m->m_icache[ICACHE_HASH(ino)];

	pthread_mutex_lock(&ext2_icache_lock);
	e->ic_ino = ino;
	e->ic_inode = *inode;
	ext2_icache_gen++;
	pthread_mutex_unlock(&ext2_icache_lock);
}

static void
icache_invalidate(struct ext2_mount *m, ino_t ino)
{
	struct icache_entry *e = &m->m_icache[ICACHE_HASH(ino)];

	pthread_mutex_lock(&ext2_icache_lock);
	if (e->ic_ino == ino)
		e->ic_ino = 0;
	ext2_icache_gen++;
	pthread_mutex_unlock(&ext2_icache_lock);
}

/*
 * Vnode table — per-mount, accessed via ext2_mount.
 */

static struct ext2_vnode *
vnode_get(struct ext2_mount *m, ino_t ino)
{
	int i, free_slot = -1;

	pthread_mutex_lock(&ext2_vnode_table_lock);
	for (i = 0; i < VNODE_TABLE_SIZE; i++) {
		if (m->m_vnode_table[i].v_ino == ino &&
		    m->m_vnode_table[i].v_refcount > 0) {
			m->m_vnode_table[i].v_refcount++;
			pthread_mutex_unlock(&ext2_vnode_table_lock);
			return &m->m_vnode_table[i];
		}
		if (free_slot < 0 && m->m_vnode_table[i].v_refcount == 0)
			free_slot = i;
	}

	if (free_slot < 0) {
		pthread_mutex_unlock(&ext2_vnode_table_lock);
		return NULL;
	}

	/* Free old resources if slot was cached */
	if (m->m_vnode_table[free_slot].v_inode_blk) {
		vm_deallocate(mach_task_self(),
			      m->m_vnode_table[free_slot].v_inode_blk,
			      m->m_vnode_table[free_slot].v_inode_blk_size);
	}
	memset(&m->m_vnode_table[free_slot], 0, sizeof(struct ext2_vnode));
	m->m_vnode_table[free_slot].v_ino = ino;
	m->m_vnode_table[free_slot].v_refcount = 1;
	pthread_mutex_init(&m->m_vnode_table[free_slot].v_lock, NULL);
	m->m_vnode_table[free_slot].v_gen = 1;
	pthread_mutex_unlock(&ext2_vnode_table_lock);
	return &m->m_vnode_table[free_slot];
}

static void
vnode_put(struct ext2_vnode *vn)
{
	if (!vn)
		return;
	pthread_mutex_lock(&ext2_vnode_table_lock);
	if (--vn->v_refcount <= 0) {
		if (vn->v_inode_blk) {
			vm_deallocate(mach_task_self(),
				      vn->v_inode_blk,
				      vn->v_inode_blk_size);
			vn->v_inode_blk = 0;
		}
		vn->v_ino = 0;
		vn->v_refcount = 0;
	}
	pthread_mutex_unlock(&ext2_vnode_table_lock);
}

/*
 * Directory entry cache — per-mount, accessed via ext2_mount.
 */

static unsigned int
dcache_hash(ino_t parent, const char *name)
{
	unsigned int h = parent;
	while (*name)
		h = h * 31 + (unsigned char)*name++;
	return h & (DCACHE_SIZE - 1);
}

static int
dcache_lookup(struct ext2_mount *m, ino_t parent, const char *name,
	      ino_t *ino_out)
{
	struct dcache_entry *e = &m->m_dcache[dcache_hash(parent, name)];
	if (e->dc_parent == parent && strcmp(e->dc_name, name) == 0) {
		if (e->dc_child == DCACHE_NEGATIVE)
			return DCACHE_NEG;
		*ino_out = e->dc_child;
		return DCACHE_HIT;
	}
	return DCACHE_MISS;
}

static void
dcache_insert(struct ext2_mount *m, ino_t parent, const char *name,
	      ino_t child)
{
	unsigned int idx = dcache_hash(parent, name);
	struct dcache_entry *e = &m->m_dcache[idx];
	e->dc_parent = parent;
	e->dc_child = child;
	strncpy(e->dc_name, name, DCACHE_NAME_MAX - 1);
	e->dc_name[DCACHE_NAME_MAX - 1] = '\0';
}

/*
 * Free file buffers, but don't close file.
 */
static void
free_file_buffers(register struct ext2fs_file *fp)
{
	register int level;

	/*
	 * Free the indirect blocks
	 */
	for (level = 0; level < NIADDR; level++) {
	    if (fp->f_blk[level] != 0) {
		(void) vm_deallocate(mach_task_self(),
				     fp->f_blk[level],
				     fp->f_blksize[level]);
		fp->f_blksize[level] = 0;
		fp->f_blk[level] = 0;
	    }
	    fp->f_blkno[level] = -1;
	}

	fp->f_ra_last_block = -1;

	/*
	 * Free the cached inode block
	 */
	if (fp->f_inode_blk != 0) {
	    (void) vm_deallocate(mach_task_self(),
				 fp->f_inode_blk,
				 fp->f_inode_blk_size);
	    fp->f_inode_blk = 0;
	    fp->f_inode_blk_size = 0;
	}
}

/*
 * Read a new inode into a file structure.
 *
 * Checks the inode cache first; on hit, copies the cached inode
 * directly without any device I/O.  On miss, reads from disk and
 * populates the cache.
 */
static int
read_inode(ino_t inumber, register struct ext2fs_file *fp)
{
	vm_offset_t		buf;
	vm_size_t		buf_size;
	register
	struct ext2_super_block	*fs;
	daddr_t			disk_block;
	kern_return_t		rc;
	unsigned int		icache_gen = 0;

#ifdef	DEBUG
	int	i = inumber;
	if(debug)
		printf("read_inode(%d)\n", i);
#endif
	fs = fp->f_fs;

	/*
	 * #599: checked before it is used.  The number comes off the disk --
	 * a directory record, most often -- and it picks the group descriptor
	 * and the block of the inode table, neither of which was bounded: a
	 * damaged record would have read (and unlink would have freed) an
	 * inode the filesystem does not have.
	 */
	if (inumber < 1 || inumber > (ino_t)fs->s_inodes_count) {
		printf("ext2: inode %lu asked for, and this filesystem has 1 to "
		       "%u — refused as damaged\n", (unsigned long)inumber,
		       (unsigned)fs->s_inodes_count);
		return FS_CORRUPT;
	}
	fp->f_ino = inumber;

	/* Check the inode cache first */
	{
	struct ext2_mount *m = (struct ext2_mount *)fp->f_dev.mount_data;
	if (m && icache_get(m, inumber, fp->f_ic, &icache_gen)) {
		free_file_buffers(fp);
		return (0);
	}
	}

	/* Cache miss — read from disk */
	disk_block = ext2_ino2blk(fs, fp->f_gd, inumber);

	rc = ext2_dev_read(&fp->f_dev,
			 (recnum_t) dbtorec(&fp->f_dev,
					    ext2_fsbtodb(fp->f_fs, disk_block)),
			 (int) EXT2_BLOCK_SIZE(fs),
			 (char **)&buf,
			 &buf_size);
	if (rc != KERN_SUCCESS)
	    return (rc);

	{
	    struct ext2_inode *raw_inode;
	    struct ext2_inode *inode;
	    unsigned long block;

	    /*
	     * Locate the inode within the block.  We cannot simply
	     * use pointer arithmetic on (struct ext2_inode *) because
	     * sizeof(struct ext2_inode) is 128, while modern ext2
	     * filesystems may use 256-byte (or larger) inodes.
	     * Instead we compute the byte offset using the actual
	     * on-disk inode size from the superblock.
	     */
	    raw_inode = (struct ext2_inode *)
		((char *)buf + ext2_itoo(fs, inumber) * EXT2_INODE_SIZE(fs));
	    inode = fp->f_ic;

	    inode->i_mode = le16_to_cpu(raw_inode->i_mode);
	    inode->i_uid = le16_to_cpu(raw_inode->i_uid);
	    inode->i_size = le32_to_cpu(raw_inode->i_size);
	    inode->i_atime = le32_to_cpu(raw_inode->i_atime);
	    inode->i_ctime = le32_to_cpu(raw_inode->i_ctime);
	    inode->i_mtime = le32_to_cpu(raw_inode->i_mtime);
	    inode->i_dtime = le32_to_cpu(raw_inode->i_dtime);
	    inode->i_gid = le16_to_cpu(raw_inode->i_gid);
	    inode->i_links_count = le16_to_cpu(raw_inode->i_links_count);
	    inode->i_blocks = le32_to_cpu(raw_inode->i_blocks);
	    inode->i_flags = le32_to_cpu(raw_inode->i_flags);
	    if ((((inode->i_mode) & IFMT) == IFLNK) && !inode->i_blocks)
		    for (block = 0; block < EXT2_N_BLOCKS; block++)
			    inode->i_block[block] = raw_inode->i_block[block];
	    else for (block = 0; block < EXT2_N_BLOCKS; block++)
		    inode->i_block[block] = le32_to_cpu(raw_inode->i_block[block]);
	    /* inode->i_version = ++event;	*/
	    inode->i_file_acl = le32_to_cpu(raw_inode->i_file_acl);
	    inode->i_dir_acl = le32_to_cpu(raw_inode->i_dir_acl);
	    inode->i_faddr = le32_to_cpu(raw_inode->i_faddr);
	    inode->i_frag = raw_inode->i_frag;
	    inode->i_fsize = raw_inode->i_fsize;
	}

	/* Populate the inode cache, unless it was told something newer */
	{
	struct ext2_mount *m = (struct ext2_mount *)fp->f_dev.mount_data;
	if (m)
		icache_fill(m, inumber, fp->f_ic, icache_gen);
	}

	/*
	 * Clear out the old buffers (this also frees the previous
	 * f_inode_blk, so we must cache the new block afterwards).
	 */
	free_file_buffers(fp);

	/*
	 * The raw inode block, handed to a new vnode or freed.  #599: NOT for
	 * write-back -- every flush reads the block afresh under
	 * ext2_itable_lock (vnode_inode_block), because a copy kept from
	 * here undid the inodes made in the same block since (8563299e).
	 */
	fp->f_inode_blk = buf;
	fp->f_inode_blk_size = buf_size;

	return (0);
}

/*
 * Given an offset in a file, find the disk block number that
 * contains that block.
 */
/*
 * #599: a block number read off the disk -- an inode's pointer, an indirect
 * block's entry -- checked before it is used: 0 (a hole) or inside
 * [s_first_data_block, s_blocks_count).  A damaged pointer was read from
 * wherever it named, handed to the page cache as a key, or written through.
 */
/*
 * Set only by ext2_blockio_selftest, for the refusals it provokes on
 * purpose, and only while it runs -- at ext_server's start, before any other
 * thread exists.  A boot log where the self-test's expected refusals read
 * like a damaged disk would teach its reader to skip the line that is one.
 */
static int	ext2_selftest_quiet;

static int
ext2_block_in_range(const struct ext2fs_file *fp, daddr_t b, const char *what)
{
	const struct ext2_super_block *fs = fp->f_fs;

	if (b == 0 || (b >= fs->s_first_data_block && b < fs->s_blocks_count))
		return 0;
	if (!ext2_selftest_quiet)
		printf("ext2: inode %u: %s block %lu is outside the filesystem "
	       "(%u..%u) — refused as damaged\n", (unsigned)fp->f_ino, what,
	       (unsigned long)b, (unsigned)fs->s_first_data_block,
	       (unsigned)fs->s_blocks_count - 1);
	return FS_CORRUPT;
}

static int
block_map_locked(
	struct ext2fs_file	*fp,
	daddr_t			file_block,
	daddr_t			*disk_block_p)	/* out */
{
	int		level;
	int		idx;
	daddr_t		ind_block_num;
	kern_return_t	rc;

#ifdef	DEBUG
	int i = file_block;
	if(debug)
		printf("block_map(%d)\n", i);
#endif
	/*
	 * Index structure of an inode:
	 *
	 * i_db[0..NDADDR-1]	hold block numbers for blocks
	 *			0..NDADDR-1
	 *
	 * i_ib[0]		index block 0 is the single indirect
	 *			block
	 *			holds block numbers for blocks
	 *			NDADDR .. NDADDR + NINDIR(fs)-1
	 *
	 * i_ib[1]		index block 1 is the double indirect
	 *			block
	 *			holds block numbers for INDEX blocks
	 *			for blocks
	 *			NDADDR + NINDIR(fs) ..
	 *			NDADDR + NINDIR(fs) + NINDIR(fs)**2 - 1
	 *
	 * i_ib[2]		index block 2 is the triple indirect
	 *			block
	 *			holds block numbers for double-indirect
	 *			blocks for blocks
	 *			NDADDR + NINDIR(fs) + NINDIR(fs)**2 ..
	 *			NDADDR + NINDIR(fs) + NINDIR(fs)**2
	 *				+ NINDIR(fs)**3 - 1
	 *
	 * #384: runs entirely under the vnode lock (see block_map()
	 * below); the shared block map cannot be freed or re-pointed
	 * underneath the walk, and f_blk[] is only touched by this
	 * handle, so replaced buffers are deallocated inline.  The
	 * indirect-block device reads happen under the lock too: they
	 * only contend with openers of the same file, and are rare
	 * (per-handle indirect cache).
	 */

	if (file_block < NDADDR) {
	    /* Direct block. */
	    rc = ext2_block_in_range(fp, fp->f_ic->i_block[file_block],
				     "a data");			/* #599 */
	    if (rc != 0)
		return (rc);
	    *disk_block_p = fp->f_ic->i_block[file_block];
	    return (0);
	}

	file_block -= NDADDR;

	/*
	 * nindir[0] = NINDIR
	 * nindir[1] = NINDIR**2
	 * nindir[2] = NINDIR**3
	 *	etc
	 */
	for (level = 0; level < NIADDR; level++) {
	    if (file_block < fp->f_nindir[level])
		break;
	    file_block -= fp->f_nindir[level];
	}
	if (level == NIADDR) {
	    /* Block number too high */
	    return (FS_NOT_IN_FILE);
	}

	ind_block_num = fp->f_ic->i_block[level + NDADDR];
	rc = ext2_block_in_range(fp, ind_block_num, "an indirect");	/* #599 */
	if (rc != 0)
	    return (rc);

	for (; level >= 0; level--) {

	    vm_offset_t	data;
	    vm_size_t		size;

	    if (ind_block_num == 0)
		break;

	    if (fp->f_blkno[level] == ind_block_num) {
		/*
		 *	Cache hit.  Just pick up the data.
		 */

		data = fp->f_blk[level];
	    }
	    else {
		rc = ext2_dev_read(&fp->f_dev,
				 (recnum_t) dbtorec(&fp->f_dev,
					    ext2_fsbtodb(fp->f_fs, ind_block_num)),
				 EXT2_BLOCK_SIZE(fp->f_fs),
				 (char **)&data,
				 &size);
		if (rc != KERN_SUCCESS)
		    return (rc);

		if (fp->f_blk[level] != 0)
		    (void) vm_deallocate(mach_task_self(),
					 fp->f_blk[level],
					 fp->f_blksize[level]);

		fp->f_blkno[level] = ind_block_num;
		fp->f_blk[level] = data;
		fp->f_blksize[level] = size;
	    }

	    if (level > 0) {
		idx = file_block / fp->f_nindir[level-1];
		file_block %= fp->f_nindir[level-1];
	    }
	    else
		idx = file_block;

	    ind_block_num = le32_to_cpu(((daddr_t *)data)[idx]);
	    rc = ext2_block_in_range(fp, ind_block_num,
				     level > 0 ? "an indirect" : "a data");
	    if (rc != 0)
		return (rc);				/* #599 */
	}

	*disk_block_p = ind_block_num;
	return (0);
}

/*
 * #384: public block_map — vnode-locked wrapper.  Re-syncs this
 * handle's private caches if the shared block map changed (truncate /
 * extension by another opener) before walking it.
 */
static int
block_map(
	struct ext2fs_file	*fp,
	daddr_t			file_block,
	daddr_t			*disk_block_p)	/* out */
{
	int rc;

	vnode_mutex_lock(fp);
	vnode_gen_check(fp);
	rc = block_map_locked(fp, file_block, disk_block_p);
	vnode_mutex_unlock(fp);
	return rc;
}

/*
 * Readahead: on sequential cache miss, prefetch up to RA_BLOCKS
 * contiguous disk blocks in a single device_read IPC and offer
 * them all to the page cache.
 *
 * #599: through a ticket taken before anything is read, and
 * page_cache_install, which takes only free or clean room, never a
 * block already held, and never bytes the disk may have changed since
 * the ticket (page_cache.h).  The old insert could publish a block's
 * pre-writeback bytes over the copy the writeback had just evicted.
 */
#define EXT2_RA_BLOCKS	32

static void
ext2_readahead(struct ext2fs_file *fp, daddr_t file_block,
	       daddr_t disk_block)
{
	struct ext2_super_block *fs = fp->f_fs;
	int block_size = EXT2_BLOCK_SIZE(fs);
	daddr_t max_file_block;
	int n_contig;
	int i, rc;
	vm_offset_t ra_buf;
	vm_size_t ra_buf_size;
	uint64_t ticket;

	if (!fp->f_dev.cache)
		return;
	ticket = page_cache_ticket(fp->f_dev.cache);

	max_file_block = (fp->f_ic->i_size + block_size - 1) / block_size;

	/*
	 * Find the longest run of physically contiguous blocks
	 * starting from disk_block+1 that are not yet cached.
	 */
	n_contig = 1;  /* include current block */
	for (i = 1; i < EXT2_RA_BLOCKS; i++) {
		daddr_t fb = file_block + i;
		daddr_t db;

		if (fb >= max_file_block)
			break;

		rc = block_map(fp, fb, &db);
		if (rc != 0 || db == 0)
			break;

		/* Must be physically contiguous */
		if (db != disk_block + i)
			break;

		/* Stop at a block already held (#599: a hint, no hit) */
		if (page_cache_contains(fp->f_dev.cache, db))
			break;

		n_contig++;
	}

	if (n_contig <= 1)
		return;

	/* Single large device_read for the whole run */
	rc = ext2_dev_read(&fp->f_dev,
			 (recnum_t) dbtorec(&fp->f_dev,
					    ext2_fsbtodb(fs, disk_block)),
			 n_contig * block_size,
			 (char **)&ra_buf, &ra_buf_size);
	if (rc != 0)
		return;

	/* Offer each block; caching a clean block is optional */
	for (i = 0; i < n_contig; i++)
		(void) page_cache_install(fp->f_dev.cache, disk_block + i,
					  ra_buf + i * block_size,
					  (vm_size_t)block_size, ticket);

	(void)vm_deallocate(mach_task_self(), ra_buf, ra_buf_size);
}

/*
 * #599: the page cache's fill for this handle's device -- page_cache_get runs
 * it, with no lock held, on a slot it has keyed FILLING.  All `size' bytes of
 * `block', or an error:
 *  - a DMA-pool slot on a device with the physical path: read into it
 *    through `phys';
 *  - a DMA-pool slot without that path: read out of line, then copied in --
 *    a DMA-pool page is never handed to a copy-path RPC;
 *  - a slab slot (phys 0): read into it in place.
 * No fallback on a refusal: the block server's KERN_NO_ACCESS is the answer.
 */
static int
ext2_fill(void *ctx, daddr_t block, vm_offset_t data, vm_size_t size,
	  vm_offset_t phys)
{
	struct ext2fs_file	*fp = (struct ext2fs_file *)ctx;
	recnum_t		 rec = (recnum_t)dbtorec(&fp->f_dev,
					   ext2_fsbtodb(fp->f_fs, block));
	io_buf_len_t		 br = 0;
	vm_size_t		 got = 0;
	vm_offset_t		 buf = 0;
	kern_return_t		 rc;

	if (phys != 0 && ext2_dev_has_phys(&fp->f_dev)) {
		vm_address_t pa = phys;

		rc = ext2_dev_read_phys(&fp->f_dev, rec, (io_buf_len_t)size,
					&pa, 1, &br);
		if (rc == 0 && br != (io_buf_len_t)size)
			rc = D_IO_ERROR;
		return rc;
	}
	if (phys != 0) {
		rc = ext2_dev_read(&fp->f_dev, rec, (io_buf_len_t)size,
				   (io_buf_ptr_t *)&buf, &got);
		if (rc == 0) {
			memcpy((void *)data, (void *)buf, size);
			(void) vm_deallocate(mach_task_self(), buf, got);
		}
		return rc;
	}
	return ext2_dev_read_overwrite(&fp->f_dev, rec, (io_buf_len_t)size,
				       data, &got);
}

/*
 * #599: `disk_block' through the page cache, for either branch of
 * buf_read_file.  Readahead first, outside the fill -- its block_map takes
 * the vnode lock, which a fill must never be inside -- and only for a
 * sequential read of a block not already held.  Then page_cache_get, which
 * reads a miss into a slot nobody else can see until the read has landed,
 * and withdraws it if the read fails: the old paths keyed the slot first and
 * read after, so a failed read left an unread block cached.
 *
 * Answers 0 and a pinned entry; 0 and NULL when the cache has no slot to
 * give (the caller reads uncached and keeps none); or the read's error, with
 * nothing cached.
 */
static int
ext2_cache_get(struct ext2fs_file *fp, daddr_t file_block, daddr_t disk_block,
	       struct page_cache_entry **ep)
{
	if (file_block == fp->f_ra_last_block + 1 &&
	    !page_cache_contains(fp->f_dev.cache, disk_block))
		ext2_readahead(fp, file_block, disk_block);
	return page_cache_get(fp->f_dev.cache, disk_block, ext2_fill, fp, ep);
}

/*
 * Read a portion of a file into an internal buffer.  Return
 * the location in the buffer and the amount in the buffer.
 * The buffer is br's, and stays valid until br is released
 * or handed to another call here for another block.
 */
static int
buf_read_file(
	register struct ext2fs_file	*fp,
	struct ext2_bref		*br,
	vm_offset_t			offset,
	vm_offset_t			*buf_p,		/* out */
	vm_size_t			*size_p)	/* out */
{
	register
	struct ext2_super_block	*fs;
	vm_offset_t		off;
	register daddr_t	file_block;
	daddr_t			disk_block;
	int			rc;
	vm_offset_t		block_size;

#ifdef	DEBUG
	if(debug)
		printf("buf_read_file(%d, %d)\n", offset, *size_p);
#endif 
	if (offset >= fp->f_ic->i_size)
	    return (FS_NOT_IN_FILE);

	fs = fp->f_fs;

	off = ext2_blkoff(fs, offset);
	file_block = ext2_lblkno(fs, offset);
	block_size = ext2_blksize(fs, fp, file_block);

	if (off || (!*buf_p) || *size_p < block_size ||
	    ((*buf_p) & (fp->f_dev.rec_size-1))) {
	    if (file_block != br->br_fblock) {
		struct page_cache_entry *e = NULL;

		/*
		 * #599: the old block goes, number and all, before anything
		 * that can fail; a failure below leaves nothing held.
		 */
		bref_release(fp, br);
	        rc = block_map(fp, file_block, &disk_block);
		if (rc != 0)
		    return (rc);

		if (disk_block == 0) {
		    if (vm_allocate(mach_task_self(), &br->br_priv,
				    block_size, TRUE) != KERN_SUCCESS) {
			br->br_priv = 0;
			return (KERN_RESOURCE_SHORTAGE);
		    }
		    memset((void *)br->br_priv, 0, block_size);
		    br->br_priv_size = block_size;
		    br->br_data = br->br_priv;
		} else {
		    if (fp->f_dev.cache) {
			rc = ext2_cache_get(fp, file_block, disk_block, &e);
			if (rc != 0)
			    return (rc);
		    }
		    if (e != NULL) {
			/* #599: held until br is released */
			br->br_entry = e;
			br->br_data = e->pc_data;
		    } else {
			/* No cache, or no slot to give: our own, cached
			 * nowhere */
			rc = ext2_dev_read(&fp->f_dev,
				     (recnum_t) dbtorec(&fp->f_dev,
							ext2_fsbtodb(fs,
								disk_block)),
				     (int) block_size,
				     (char **) &br->br_priv,
				     &br->br_priv_size);
			if (rc)
			    return (rc);
			br->br_data = br->br_priv;
		    }
		}

	        br->br_fblock = file_block;
	    }

	    /*
	     * Return address of byte in buffer corresponding to
	     * offset, and size of remainder of buffer after that
	     * byte.
	     */
	    *buf_p = br->br_data + off;
	    *size_p = block_size - off;

	} else {
		/*
		 * Directly read into caller buffer
		 */
	        rc = block_map(fp, file_block, &disk_block);
		if (rc != 0)
		    return (rc);

		if (disk_block == 0) {
			memset((void *)*buf_p, 0, block_size);
			*size_p = block_size;
			return 0;
		}

		if (fp->f_dev.cache) {
			struct page_cache_entry *e = NULL;

			rc = ext2_cache_get(fp, file_block, disk_block, &e);
			if (rc != 0)
				return (rc);
			if (e != NULL) {
				memcpy((void *)*buf_p, (void *)e->pc_data,
				       block_size);
				page_cache_put(fp->f_dev.cache, e);
				*size_p = block_size;
			} else {
				/* No slot to give: read uncached, keep none */
				rc = ext2_dev_read_overwrite(&fp->f_dev,
					(recnum_t) dbtorec(&fp->f_dev,
						ext2_fsbtodb(fs, disk_block)),
					(int) block_size, *buf_p, size_p);
				if (rc)
					return (rc);
			}
		} else {
			rc = ext2_dev_read_overwrite(&fp->f_dev,
				     (recnum_t) dbtorec(&fp->f_dev,
							ext2_fsbtodb(fs,
								disk_block)),
				     (int) block_size,
				     *buf_p,
				     size_p);
			if (rc)
				return (rc);
		}
	}

	/*
	 * Truncate buffer at end of file.
	 */
	if (*size_p > fp->f_ic->i_size - offset)
	    *size_p = fp->f_ic->i_size - offset;

	fp->f_ra_last_block = file_block;
	return (0);
}

/*
 * #599: one directory record, checked before anything in it is used.
 *
 * Every walk over a directory advances by the record's own rec_len, so a
 * record is trusted with the position of the next one.  search_directory
 * trusted it entirely: an all-zero block -- a read that never landed -- has
 * rec_len 0, and the walk stayed on it for ever, holding the server's thread.
 * ext2fs_readdir stopped at 0 and answered success with what it had, so rmdir
 * found a damaged directory empty; dir_add_entry and dir_remove_entry checked
 * half the rule and skipped the rest of the block in silence.
 *
 * `room' is what is left from the record to the end of its block, or of the
 * directory if that comes first.  The rule is the on-disk format's, in the
 * order that keeps each test inside what the previous one proved: room for a
 * header before the header is read; a length that is at least the smallest
 * record, a multiple of four and inside the block; a name inside its record;
 * an inode number the filesystem has.  Tombstones (inode 0) keep their name
 * length and are held to it.
 *
 * Answers 0, or FS_CORRUPT after one line that names the directory, the
 * offset and the values -- a damaged directory is an error its caller sees,
 * never an empty or a shorter one.
 */
static const char *
ext2_dirent_verdict(
	const struct ext2_dir_entry	*dp,
	vm_size_t			room,
	unsigned int			inodes_count)
{
	unsigned int	rec_len, name_len;

	if (room < EXT2_DIR_REC_LEN(1))
		return "less room than the smallest record";

	rec_len = le16_to_cpu(dp->rec_len);
	name_len = dp->name_len;
	if (rec_len < EXT2_DIR_REC_LEN(1))
		return "a record shorter than the smallest";
	if ((rec_len & EXT2_DIR_ROUND) != 0)
		return "a record length that is not a multiple of four";
	if (rec_len > room)
		return "a record that crosses the end of its block";
	if (EXT2_DIR_REC_LEN(name_len) > rec_len)
		return "a name longer than its record";
	if (le32_to_cpu(dp->inode) > inodes_count)
		return "an inode number the filesystem does not have";
	return 0;
}

static int
ext2_dirent_check(
	const struct ext2fs_file	*dir_fp,
	const struct ext2_dir_entry	*dp,
	vm_size_t			room,
	vm_offset_t			offset)
{
	const char	*why;

	why = ext2_dirent_verdict(dp, room, dir_fp->f_fs->s_inodes_count);
	if (why == 0)
		return 0;

	if (room < EXT2_DIR_REC_LEN(1))
		printf("ext2: directory inode %u, offset %lu: %s (room %lu) — "
		       "refused as damaged\n", (unsigned)dir_fp->f_ino,
		       (unsigned long)offset, why, (unsigned long)room);
	else
		printf("ext2: directory inode %u, offset %lu: %s (rec_len %u, "
		       "name_len %u, inode %u, room %lu) — refused as "
		       "damaged\n", (unsigned)dir_fp->f_ino,
		       (unsigned long)offset, why,
		       (unsigned)le16_to_cpu(dp->rec_len),
		       (unsigned)dp->name_len,
		       (unsigned)le32_to_cpu(dp->inode), (unsigned long)room);
	return FS_CORRUPT;
}

/*
 * #599: the check above, asked about records built to break each of its
 * rules and about well-formed ones, at every start of the server -- before
 * any disk is read, so a check that answers wrong is said before it has
 * decided anything.  *ran counts the questions, *wrong the wrong answers.
 */
void
ext2_dirent_selftest(unsigned int *ran, unsigned int *wrong)
{
	static const struct {
		unsigned int	inode, rec_len, name_len, room;
		int		damaged;
	} q[] = {
		{   2,   12, 1, 4096, 0 },	/* the smallest record */
		{   0, 4096, 3, 4096, 0 },	/* a tombstone filling the block */
		{  11, 4084, 9, 4084, 0 },	/* the last record, exactly */
		{ 100,   16, 5,   16, 0 },	/* the highest inode, a longer name */
		{   0,    0, 0, 4096, 1 },	/* an unread block: all zero */
		{   2,    8, 0, 4096, 1 },	/* shorter than the smallest */
		{   2,   13, 1, 4096, 1 },	/* not a multiple of four */
		{   2, 4100, 1, 4096, 1 },	/* crosses the block */
		{   2,   12, 5, 4096, 1 },	/* a name longer than its record */
		{ 101,   12, 1, 4096, 1 },	/* an inode the filesystem lacks */
		{   2,   12, 1,    8, 1 },	/* no room for a header */
	};
	struct ext2_dir_entry	e;
	unsigned int		i;

	*ran = 0;
	*wrong = 0;
	for (i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
		memset(&e, 0, sizeof(e));
		e.inode = cpu_to_le32(q[i].inode);
		e.rec_len = cpu_to_le16(q[i].rec_len);
		e.name_len = (unsigned char)q[i].name_len;
		(*ran)++;
		if ((ext2_dirent_verdict(&e, q[i].room, 100) != 0) !=
		    q[i].damaged)
			(*wrong)++;
	}
}

/*
 * #599: the block-I/O paths, asked at every start of ext_server, on a file
 * whose device answers nothing (dev_port MACH_PORT_NULL, no block layer): any
 * transfer fails, so a path that answers success without one is reading
 * something that is not the disk.  1 KiB blocks, block 0 a hole, block 1
 * mapped.  The page-cache work of #599 adds its arms here, one per defect,
 * each written to fail when its fix is taken out.  *ran counts the cases,
 * *wrong the wrong answers.
 */
/* The self-test's caches never hold a dirty block; a writeback is wrong. */
static int
ext2_selftest_writeback(void *ctx, daddr_t block, vm_offset_t data,
			vm_size_t size, vm_offset_t phys)
{
	(void)ctx;
	(void)block;
	(void)data;
	(void)size;
	(void)phys;
	return KERN_FAILURE;
}

/*
 * E5: the buffered path holds a cache slot for as long as its bref and no
 * longer, and a buffered read that fails leaves nothing cached.  Block 1
 * (disk 20) is put in the cache clean and read unaligned: answered from the
 * slot, pinned while the bref holds it, unpinned once it is released.  Then
 * block 2, mapped to disk 21 and not held, read unaligned on the device that
 * answers nothing: an error, 21 not held, and no slot pinned.
 */
static void
ext2_selftest_bref(struct ext2fs_file *f, struct ext2_bref *br,
		   struct page_cache *pc, unsigned int *ran,
		   unsigned int *wrong)
{
	unsigned char			 blk[1024];
	struct page_cache_entry		*e;
	vm_offset_t			 buf = 0;
	vm_size_t			 size = 0;
	unsigned int			 i, pinned;
	int				 rc;

	memset(blk, 0x5a, sizeof(blk));
	(void) page_cache_install(pc, 20, (vm_offset_t)blk, sizeof(blk),
				  page_cache_ticket(pc));
	f->f_ra_last_block = (daddr_t)-2;
	rc = buf_read_file(f, br, 1024 + 5, &buf, &size);
	e = br->br_entry;
	(*ran)++;
	if (rc != 0 || e == NULL || e->pc_refs != 1 || size != 1024 - 5 ||
	    *(const unsigned char *)buf != 0x5a) {
		(*wrong)++;
		e = NULL;
	}
	bref_release(f, br);
	if (e != NULL && e->pc_refs != 0)
		(*wrong)++;

	f->f_ic->i_block[2] = 21;
	f->f_ra_last_block = (daddr_t)-2;
	buf = 0;
	size = 0;
	rc = buf_read_file(f, br, 2048 + 5, &buf, &size);
	pinned = 0;
	for (i = 0; i < pc->pc_max_entries; i++)
		if (pc->pc_pool[i].pc_refs != 0)
			pinned++;
	(*ran)++;
	if (rc == 0 || br->br_entry != NULL ||
	    page_cache_contains(pc, 21) || pinned != 0)
		(*wrong)++;
	bref_release(f, br);
	f->f_ic->i_block[2] = 0;
}

/*
 * E4: part of a block the file already has (block 3, disk 22, not held), on
 * the device that answers nothing: the write fails with the read's error and
 * leaves nothing in the cache -- no block of zeros with the chunk in it,
 * dirty.  write_file_locked marks the vnode dirty on success, so the case
 * lends the handle one; the write must never get that far.
 */
static void
ext2_selftest_write(struct ext2fs_file *f, struct page_cache *pc,
		    unsigned int *ran, unsigned int *wrong)
{
	struct ext2_vnode	vn;
	unsigned char		chunk[10];
	unsigned int		i, dirty;
	int			rc;

	memset(&vn, 0, sizeof(vn));
	if (pthread_mutex_init(&vn.v_lock, NULL) != 0) {
		(*ran)++;
		(*wrong)++;
		return;
	}
	memset(chunk, 0x3c, sizeof(chunk));
	f->f_ic->i_block[3] = 22;
	f->f_vnode = &vn;
	rc = write_file_locked(f, 3 * 1024 + 5, (vm_offset_t)chunk,
			       sizeof(chunk));
	f->f_vnode = NULL;
	f->f_ic->i_block[3] = 0;
	dirty = 0;
	for (i = 0; i < pc->pc_max_entries; i++)
		if (pc->pc_pool[i].pc_state != PC_FREE &&
		    pc->pc_pool[i].pc_dirty)
			dirty++;
	(*ran)++;
	if (rc == 0 || page_cache_contains(pc, 22) || dirty != 0 ||
	    vn.v_inode_dirty)
		(*wrong)++;
	(void) pthread_mutex_destroy(&vn.v_lock);
}

void
ext2_blockio_selftest(unsigned int *ran, unsigned int *wrong)
{
	struct ext2_super_block	sb;
	struct ext2fs_file	f;
	struct ext2_bref	br;
	vm_offset_t		buf;
	vm_size_t		size;
	unsigned int		i, nonzero;
	int			rc;

	*ran = 0;
	*wrong = 0;
	ext2_selftest_quiet = 1;
	bref_init(&br);
	memset(&sb, 0, sizeof(sb));
	memset(&f, 0, sizeof(f));
	sb.s_log_block_size = 0;		/* 1 KiB */
	sb.s_first_data_block = 1;
	sb.s_blocks_count = 64;
	sb.s_inodes_count = 16;
	f.f_fs = &sb;
	f.f_ic = &f.f_ic_scratch;
	f.f_ic->i_size = 4 * 1024;
	f.f_ic->i_block[0] = 0;			/* a hole */
	f.f_ic->i_block[1] = 20;		/* mapped */
	f.f_dev.dev_port = MACH_PORT_NULL;
	f.f_dev.rec_size = 512;
	f.f_nindir[0] = 1024 / 4;
	f.f_nindir[1] = (1024 / 4) * (1024 / 4);

	/* E0a: a hole reads as zeros, and the device is never asked. */
	buf = 0;
	size = 0;
	rc = buf_read_file(&f, &br, 0, &buf, &size);
	nonzero = 0;
	for (i = 0; rc == 0 && buf != 0 && i < size; i++)
		if (((const unsigned char *)buf)[i] != 0)
			nonzero++;
	(*ran)++;
	if (rc != 0 || buf == 0 || size != 1024 || nonzero != 0)
		(*wrong)++;

	/* E0b: a mapped block on a device that answers nothing is an error. */
	buf = 0;
	size = 0;
	rc = buf_read_file(&f, &br, 1024, &buf, &size);
	(*ran)++;
	if (rc == 0)
		(*wrong)++;

	/*
	 * E1: a pointer outside the filesystem (64 blocks) is refused as
	 * damaged, not read: a direct one, and the single-indirect root that
	 * file block 12 goes through.  Exactly FS_CORRUPT -- the device would
	 * answer something else.
	 */
	{
		daddr_t	b = 0;

		f.f_ic->i_block[2] = 80;
		(*ran)++;
		if (block_map(&f, 2, &b) != FS_CORRUPT)
			(*wrong)++;
		f.f_ic->i_block[2] = 0;
		f.f_ic->i_size = 16 * 1024;
		f.f_ic->i_block[NDADDR] = 80;
		(*ran)++;
		if (block_map(&f, NDADDR, &b) != FS_CORRUPT)
			(*wrong)++;
		f.f_ic->i_block[NDADDR] = 0;
		f.f_ic->i_size = 4 * 1024;
	}

	/*
	 * E2: a read that fails lets the bref's buffer go with its block
	 * number.  Hold block 0 (the hole), fail a read of block 1 through the
	 * same bref, then read offset 5 of block 0 again: it must be read
	 * afresh -- zeros, a real buffer -- not answered from a released one
	 * as 0 + 5.
	 */
	bref_release(&f, &br);
	buf = 0;
	size = 0;
	(void) buf_read_file(&f, &br, 0, &buf, &size);
	buf = 0;
	size = 0;
	(void) buf_read_file(&f, &br, 1024, &buf, &size);
	buf = 0;
	size = 0;
	rc = buf_read_file(&f, &br, 5, &buf, &size);
	(*ran)++;
	if (rc != 0 || buf < 4096 || size != 1024 - 5)
		(*wrong)++;
	bref_release(&f, &br);

	/*
	 * E3: an aligned read goes through the page cache's get, and a read
	 * that fails leaves nothing cached: after it the block is not held and
	 * the cache counted one miss.
	 */
	{
		struct page_cache	*pc;
		vm_offset_t		 page = 0;
		vm_size_t		 got = 1024;

		pc = page_cache_create(4, 1024, ext2_selftest_writeback, 0);
		if (pc != NULL &&
		    vm_allocate(mach_task_self(), &page, 4096, TRUE) ==
		    KERN_SUCCESS) {
			f.f_dev.cache = pc;
			f.f_ra_last_block = (daddr_t)-2; /* not sequential */
			buf = page;
			rc = buf_read_file(&f, &br, 1024, &buf, &got);
			(*ran)++;
			if (rc == 0 || page_cache_contains(pc, 20) ||
			    pc->pc_misses != 1)
				(*wrong)++;
			ext2_selftest_bref(&f, &br, pc, ran, wrong);
			ext2_selftest_write(&f, pc, ran, wrong);
			ext2_selftest_dir(&f, pc, ran, wrong);
			f.f_dev.cache = NULL;
			(void) vm_deallocate(mach_task_self(), page, 4096);
		} else {
			(*ran)++;
			(*wrong)++;
		}
		if (pc != NULL)
			(void) page_cache_destroy(pc);
	}

	bref_release(&f, &br);
	free_file_buffers(&f);
	ext2_selftest_quiet = 0;
}

/*
 * Search a directory for a name and return its
 * i_number.
 */
static int
search_directory_impl(
	char *name,
	register struct ext2fs_file *fp,
	struct ext2_bref *br,
	ino_t *inumber_p)
{
	vm_offset_t	buf;
	vm_size_t	buf_size;
	vm_offset_t	offset;
	register struct ext2_dir_entry *dp;
	int		length;
	kern_return_t	rc;
	char		tmp_name[256];
	ino_t		cached_ino;
	int		cache_rc = DCACHE_MISS;

#ifdef	DEBUG
	if(debug)
		printf("search_directory(%s)\n", name);
#endif

	/* Check the directory entry cache first */
	{
	struct ext2_mount *m = (struct ext2_mount *)fp->f_dev.mount_data;
	if (m)
		cache_rc = dcache_lookup(m, fp->f_ino, name, &cached_ino);
	}
	if (cache_rc == DCACHE_HIT) {
		*inumber_p = cached_ino;
		return (0);
	}
	if (cache_rc == DCACHE_NEG)
		return (FS_NO_ENTRY);

	length = strlen(name);

	offset = 0;
	while (offset < fp->f_ic->i_size) {
	    buf = 0;
	    buf_size = 0;
	    rc = buf_read_file(fp, br, offset, &buf, &buf_size);
	    if (rc != KERN_SUCCESS)
		return (rc);

	    dp = (struct ext2_dir_entry *)buf;
	    rc = ext2_dirent_check(fp, dp, buf_size, offset);	/* #599 */
	    if (rc != 0)
		return (rc);
	    if (le32_to_cpu(dp->inode) != 0) {
		strncpy (tmp_name, dp->name, le16_to_cpu(dp->name_len));
		tmp_name[le16_to_cpu(dp->name_len)] = '\0';
		if (le16_to_cpu(dp->name_len) == length &&
		    !strcmp(name, tmp_name))
	    	{
		    /* found entry — cache it */
		    *inumber_p = le32_to_cpu(dp->inode);
		    {
		    struct ext2_mount *m =
			(struct ext2_mount *)fp->f_dev.mount_data;
		    if (m)
			dcache_insert(m, fp->f_ino, name, *inumber_p);
		    }
		    return (0);
		}
	    }
	    offset += le16_to_cpu(dp->rec_len);
	}

	/* Cache the negative result */
	{
	struct ext2_mount *m = (struct ext2_mount *)fp->f_dev.mount_data;
	if (m)
		dcache_insert(m, fp->f_ino, name, DCACHE_NEGATIVE);
	}
	return (FS_NO_ENTRY);
}

static int
search_directory(
	char *name,
	struct ext2fs_file *fp,
	ino_t *inumber_p)
{
	struct ext2_bref br;
	int rc;

	bref_init(&br);
	rc = search_directory_impl(name, fp, &br, inumber_p);
	bref_release(fp, &br);
	return (rc);
}

static int
read_fs(
	struct device *dev,
	struct ext2_super_block **fsp,
	struct ext2_group_desc  **gdp,
	vm_size_t		*gd_size_p)
{
	register
	struct ext2_super_block *fs, raw_fs;
	vm_offset_t		buf;
	vm_offset_t		buf2;
	vm_size_t		buf_size;
	vm_size_t		buf2_size;
	int			error;
	int			gd_count;
	int			gd_blocks;
	int			gd_size;
	int			gd_location;
	int			gd_sector;

#ifdef	DEBUG
	if(debug)
		printf("read_fs()\n");
#endif 
	/*
	 * Read the super block
	 */
	error = ext2_dev_read(dev,
			    (recnum_t) dbtorec(dev, SBLOCK), SBSIZE,
			    (char **) &buf, &buf_size);
	if (error)
	    return (error);

	/*
	 * Check the superblock
	 */
	raw_fs = *(struct ext2_super_block *)buf;
	if (le16_to_cpu(raw_fs.s_magic) != EXT2_SUPER_MAGIC) {
		(void) vm_deallocate(mach_task_self(), buf, buf_size);
		return (FS_INVALID_FS);
	}

	fs = (struct ext2_super_block *) buf;
	*fsp = fs;

	fs->s_inodes_count = le32_to_cpu(raw_fs.s_inodes_count);
	fs->s_blocks_count = le32_to_cpu(raw_fs.s_blocks_count);
	fs->s_r_blocks_count = le32_to_cpu(raw_fs.s_r_blocks_count);
	fs->s_free_blocks_count = le32_to_cpu(raw_fs.s_free_blocks_count);
	fs->s_free_inodes_count = le32_to_cpu(raw_fs.s_free_inodes_count);
	fs->s_first_data_block = le32_to_cpu(raw_fs.s_first_data_block);
	fs->s_log_block_size = le32_to_cpu(raw_fs.s_log_block_size);
	fs->s_log_frag_size = le32_to_cpu(raw_fs.s_log_frag_size);
	fs->s_blocks_per_group = le32_to_cpu(raw_fs.s_blocks_per_group);
	fs->s_frags_per_group = le32_to_cpu(raw_fs.s_frags_per_group);
	fs->s_inodes_per_group = le32_to_cpu(raw_fs.s_inodes_per_group);
	fs->s_mtime = le32_to_cpu(raw_fs.s_mtime);
	fs->s_wtime = le32_to_cpu(raw_fs.s_wtime);
	fs->s_mnt_count = le16_to_cpu(raw_fs.s_mnt_count);
	fs->s_max_mnt_count = le16_to_cpu(raw_fs.s_max_mnt_count);
	fs->s_magic = le16_to_cpu(raw_fs.s_magic);
	fs->s_state = le16_to_cpu(raw_fs.s_state);
	fs->s_errors = le16_to_cpu(raw_fs.s_errors);
	fs->s_minor_rev_level = le16_to_cpu(raw_fs.s_minor_rev_level);
	fs->s_lastcheck = le32_to_cpu(raw_fs.s_lastcheck);
	fs->s_checkinterval = le32_to_cpu(raw_fs.s_checkinterval);
	fs->s_creator_os = le32_to_cpu(raw_fs.s_creator_os);
	fs->s_rev_level = le32_to_cpu(raw_fs.s_rev_level);
	fs->s_def_resuid = le16_to_cpu(raw_fs.s_def_resuid);
	fs->s_def_resgid = le16_to_cpu(raw_fs.s_def_resgid);

	/*
	 * Byte-swap dynamic revision fields.
	 * For GOOD_OLD_REV these fields don't exist on disk but
	 * EXT2_INODE_SIZE() / EXT2_FIRST_INO() will return the
	 * old defaults (128 / 11) regardless.
	 */
	if (fs->s_rev_level >= EXT2_DYNAMIC_REV) {
		fs->s_first_ino = le32_to_cpu(raw_fs.s_first_ino);
		fs->s_inode_size = le16_to_cpu(raw_fs.s_inode_size);
		fs->s_block_group_nr = le16_to_cpu(raw_fs.s_block_group_nr);
		fs->s_feature_compat = le32_to_cpu(raw_fs.s_feature_compat);
		fs->s_feature_incompat = le32_to_cpu(raw_fs.s_feature_incompat);
		fs->s_feature_ro_compat = le32_to_cpu(raw_fs.s_feature_ro_compat);
	} else {
		fs->s_first_ino = EXT2_GOOD_OLD_FIRST_INO;
		fs->s_inode_size = EXT2_GOOD_OLD_INODE_SIZE;
		fs->s_feature_compat = 0;
		fs->s_feature_incompat = 0;
		fs->s_feature_ro_compat = 0;
	}

	/*
	 * Validate inode size: must be a power of 2, at least 128 bytes,
	 * and no larger than the block size.
	 */
	{
		unsigned short isz = EXT2_INODE_SIZE(fs);
		if (isz < EXT2_GOOD_OLD_INODE_SIZE ||
		    isz > EXT2_BLOCK_SIZE(fs) ||
		    (isz & (isz - 1)) != 0) {
			printf("ext2: invalid inode size %d\n", isz);
			(void) vm_deallocate(mach_task_self(), buf, buf_size);
			return (FS_INVALID_FS);
		}
	}

	/*
	 * Reject incompatible features we do not understand.
	 * Since this is a read-only filesystem, compatible and
	 * read-only compatible features can be safely ignored.
	 */
	if (fs->s_feature_incompat & ~EXT2_FEATURE_INCOMPAT_SUPP) {
		printf("ext2: unsupported incompat features 0x%x\n",
		       fs->s_feature_incompat & ~EXT2_FEATURE_INCOMPAT_SUPP);
		(void) vm_deallocate(mach_task_self(), buf, buf_size);
		return (FS_INVALID_FS);
	}

#ifdef	DEBUG
	printf("ext2: rev %ld, inode size %d, block size %ld\n",
	       fs->s_rev_level,
	       (int)EXT2_INODE_SIZE(fs),
	       (long)EXT2_BLOCK_SIZE(fs));
#endif

	/*
	 * Compute the groups informations
	 */
	gd_count = (fs->s_blocks_count - fs->s_first_data_block +
		    fs->s_blocks_per_group - 1) / fs->s_blocks_per_group;
	gd_blocks = (gd_count + EXT2_DESC_PER_BLOCK(fs) - 1) /
		    EXT2_DESC_PER_BLOCK(fs);
	gd_size = gd_blocks * EXT2_BLOCK_SIZE(fs);
	gd_location = fs->s_first_data_block + 1;
	gd_sector = (gd_location * EXT2_BLOCK_SIZE(fs)) / DEV_BSIZE;

	/*
	 * Read the groups descriptors
	 */
	error = ext2_dev_read(dev,
			    (recnum_t) dbtorec(dev, gd_sector), gd_size,
			    (char **) &buf2, &buf2_size);
	if (error) {
		(void) vm_deallocate(mach_task_self(), buf, buf_size);
#ifdef	DEBUG
		if (debug)
			printf("ext2 read_fs: device_read(group_descs)"
			       " returned 0x%x\n", error);
#endif
		return error;
	}

	*gdp = (struct ext2_group_desc *) buf2;
	*gd_size_p = gd_size;

	return 0;
}

static int
mount_fs(register struct ext2fs_file	*fp)
{
	int error;
	struct ext2_mount *m = ext2_get_mount(&fp->f_dev);

	if (m && m->m_fs) {
		/* Reuse cached superblock and group descriptors */
		fp->f_fs = m->m_fs;
		fp->f_gd = m->m_gd;
		fp->f_gd_size = m->m_gd_size;
		{
		    register int level;
		    for (level = 0; level < NIADDR; level++)
			fp->f_nindir[level] = m->m_nindir[level];
		}
		return (0);
	}

	error = read_fs(&fp->f_dev, &fp->f_fs, &fp->f_gd, &fp->f_gd_size);
	if (error)
	    return (error);

	/*
	 * #284 phase 1 (Linux-compliant): mark the volume "in use" so an
	 * unclean shutdown forces e2fsck on the next boot.  Linux clears
	 * EXT2_VALID_FS and bumps the mount count on every RW mount; the
	 * symmetric "mark clean" is ext2fs_mark_clean_dev() at unmount.
	 * Only the first (uncached) mount reaches here.
	 */
	if (fp->f_fs->s_state & EXT2_VALID_FS) {
		fp->f_fs->s_state &= ~EXT2_VALID_FS;
		fp->f_fs->s_mnt_count++;
		(void) write_super(fp);
	}

	/* Cache for subsequent opens in per-mount state */
	if (m) {
		m->m_fs = fp->f_fs;
		m->m_gd = fp->f_gd;
		m->m_gd_size = fp->f_gd_size;
	}

	{
	    register struct ext2_super_block *fs = fp->f_fs;
	    register int	mult;
	    register int	level;

	    mult = 1;
	    for (level = 0; level < NIADDR; level++) {
		mult *= NINDIR(fs);
		fp->f_nindir[level] = mult;
		if (m)
			m->m_nindir[level] = mult;
	    }
	}

	return (0);
}

static void
unmount_fs(register struct ext2fs_file *fp)
{
	if (file_is_structured(fp)) {
	    /* Don't free cached superblock/GD — they're shared */
	    struct ext2_mount *m = (struct ext2_mount *)fp->f_dev.mount_data;
	    if (!m || fp->f_fs != m->m_fs) {
		(void) vm_deallocate(mach_task_self(),
				     (vm_offset_t) fp->f_fs,
				     SBSIZE);
		(void) vm_deallocate(mach_task_self(),
				     (vm_offset_t) fp->f_gd,
				     fp->f_gd_size);
	    }
	    fp->f_fs = 0;
	}
}

/*
 * Open a file into a caller-supplied ext2fs_file struct.
 * The caller is responsible for allocation and deallocation of fp.
 */
int
ext2fs_open_file_into(
	struct device *dev,
	const char *path,
	fs_private_t *private,
	struct ext2fs_file *fp)
{
#define	RETURN(code)	{ rc = (code); goto exit; }

	register char	*cp, *component;
	register int	c;	/* char */
	register int	rc;
	ino_t		inumber, parent_inumber;
	int		nlinks = 0;
	char	        *namebuf;

#ifdef	DEBUG
	if(debug)
		printf("ext2fs_open_file(%s, %s)\n", dev, path);
#endif
	if (path == 0 || *path == '\0') {
	    return FS_NO_ENTRY;
	}

	namebuf = (char*)malloc((size_t)(PATH_MAX+1));
	memset((void *)fp, '\0', sizeof(struct ext2fs_file));
	fp->f_dev = *dev;
	fp->f_ic = &fp->f_ic_scratch;	/* path walk uses scratch */
	/*
	 * Copy name into buffer to allow modifying it.
	 */
	strcpy(namebuf, path);

	cp = namebuf;

	rc = mount_fs(fp);
	if (rc)
	    return rc;

	/*
	 * Propagate mount_data back to the original device struct.
	 * ext2_get_mount() stores the new ext2_mount on fp->f_dev
	 * (a copy), so the caller's device never gets updated.
	 */
	if (fp->f_dev.mount_data && !dev->mount_data)
		dev->mount_data = fp->f_dev.mount_data;

	inumber = (ino_t) ROOTINO;
	if ((rc = read_inode(inumber, fp)) != 0)
	    goto exit;

	while (*cp) {

	    /*
	     * Check that current node is a directory.
	     */
	    if ((fp->f_ic->i_mode & IFMT) != IFDIR)
		RETURN (FS_NOT_DIRECTORY);

	    /*
	     * Remove extra separators
	     */
	    while (*cp == '/')
		cp++;

	    /*
	     * Trailing separators (or the root path "/") leave nothing to
	     * look up — the current inode is the answer.
	     */
	    if (*cp == '\0')
		break;

	    /*
	     * Get next component of path name.
	     */
	    component = cp;
	    {
		register int	len = 0;

		while ((c = *cp) != '\0' && c != '/') {
		    if (len++ > MAXNAMLEN)
			RETURN (FS_NAME_TOO_LONG);
		    if (c & 0200)
			RETURN (FS_INVALID_PARAMETER);
		    cp++;
		}
		*cp = 0;
	    }

	    /*
	     * Look up component in current directory.
	     * Save directory inumber in case we find a
	     * symbolic link.
	     */
	    parent_inumber = inumber;
	    rc = search_directory(component, fp, &inumber);
	    if (rc)
	        goto exit;

	    *cp = c;

	    /*
	     * Open next component.
	     */
	    if ((rc = read_inode(inumber, fp)) != 0)
		goto exit;

	    /*
	     * Check for symbolic link.
	     */
	    if ((fp->f_ic->i_mode & IFMT) == IFLNK) {

		int	link_len = fp->f_ic->i_size;
		int	len;

		len = strlen(cp) + 1;

		if (link_len + len >= MAXPATHLEN - 1)
		    RETURN (FS_NAME_TOO_LONG);

		if (++nlinks > MAXSYMLINKS)
		    RETURN (FS_SYMLINK_LOOP);

		ovbcopy(cp, &namebuf[link_len], len);

#ifdef	IC_FASTLINK
		if (fp->f_ic->i_blocks == 0) {
		    memcpy(namebuf,
			   (char *)fp->f_ic->i_block, (unsigned)link_len);
		}
		else
#endif	/* IC_FASTLINK */
		{
		    /*
		     * Read file for symbolic link
		     */
		    vm_offset_t	buf;
		    vm_size_t		buf_size;
		    daddr_t	disk_block;
		    register struct ext2_super_block *fs = fp->f_fs;

		    /*
		     * #599: the map's answer is read, not discarded, and a
		     * slow symlink with no block is damaged -- block 0 is the
		     * boot block, not the link's body.
		     */
		    rc = block_map(fp, (daddr_t)0, &disk_block);
		    if (rc == 0 && disk_block == 0)
			rc = FS_CORRUPT;
		    if (rc == 0)
			rc = ext2_dev_read(&fp->f_dev,
				     (recnum_t) dbtorec(&fp->f_dev,
							ext2_fsbtodb(fs,
								disk_block)),
				     (int) ext2_blksize(fs, fp, 0),
				     (char **) &buf,
				     &buf_size);
		    if (rc)
			goto exit;

		    memcpy(namebuf, (char *)buf, (unsigned)link_len);
		    (void) vm_deallocate(mach_task_self(), buf, buf_size);
		}

		/*
		 * If relative pathname, restart at parent directory.
		 * If absolute pathname, restart at root.
		 * If pathname begins '/dev/<device>/',
		 *	restart at root of that device.
		 */
		cp = namebuf;
		if (*cp != '/') {
		    inumber = parent_inumber;
		}
#ifndef	MACH_DEV_LINK
		else
		    inumber = (ino_t)ROOTINO;
#else	/* MACH_DEV_LINK */
		else if (!strprefix(cp, "/dev/")) {
		    inumber = (ino_t)ROOTINO;
		}
		else {
		    cp += 5;
		    component = cp;
		    while ((c = *cp) != '\0' && c != '/') {
			cp++;
		    }
		    *cp = '\0';

		    /*
		     * Unmount current file system and free buffers.
		     */
		    close_file(fp);

		    /*
		     * Open new root device.
		     */
		    rc = device_open(master_device_port,
				D_READ,
				component,
				&fp->f_dev.dev_port);
		    if (rc)
			return (rc);

		    fp->f_dev.rec_size = dev_rec_size(fp->f_dev.dev_port);

		    if (c == 0) {
			fp->f_fs = 0;
			goto out_ok;
		    }

		    *cp = c;

		    rc = mount_fs(fp);
		    if (rc)
			return (rc);

		    inumber = (ino_t)ROOTINO;
		}
#endif	/* MACH_DEV_LINK */
		if ((rc = read_inode(inumber, fp)) != 0)
		    goto exit;
	    }
	}

	/*
	 * Found terminal component — attach shared vnode.
	 */
#if MACH_DEV_LINK
    out_ok:
#endif	/* MACH_DEV_LINK */
	{
		struct ext2_mount *m =
			(struct ext2_mount *)fp->f_dev.mount_data;
		struct ext2_vnode *vn = m ? vnode_get(m, fp->f_ino) : NULL;
		if (!vn) {
			/* #283: vnode table exhausted — a resource limit, not a
			 * missing name.  Returning FS_NO_ENTRY here used to make
			 * a perfectly-present file look absent (the original 5001
			 * symptom). */
			free(namebuf);
			ext2fs_close_file((fs_private_t)fp);
			return FS_NO_RESOURCES;
		}
		if (vn->v_refcount == 1) {
			/* New vnode — populate from scratch */
			vn->v_ic = fp->f_ic_scratch;
			vn->v_inode_blk = fp->f_inode_blk;
			vn->v_inode_blk_size = fp->f_inode_blk_size;
			fp->f_inode_blk = 0;
			fp->f_inode_blk_size = 0;
		} else {
			/* Existing vnode — discard scratch data */
			if (fp->f_inode_blk) {
				vm_deallocate(mach_task_self(),
					      fp->f_inode_blk,
					      fp->f_inode_blk_size);
				fp->f_inode_blk = 0;
				fp->f_inode_blk_size = 0;
			}
		}
		fp->f_vnode = vn;
		fp->f_ic = &vn->v_ic;
		fp->f_gen = vn->v_gen;
	}
	mutex_init(&fp->f_lock);
	free(namebuf);
	*private = (fs_private_t) fp;
	return 0;

	/*
	 * At error exit, close file to free storage.
	 */
    exit:
	free(namebuf);
	ext2fs_close_file((fs_private_t)fp);
	return rc;
}

/*
 * Open a file.  Allocates ext2fs_file via malloc.
 * Caller must free() the returned private after ext2fs_close_file().
 */
int
ext2fs_open_file(
	struct device *dev,
	const char *path,
	fs_private_t *private)
{
	struct ext2fs_file *fp;

	fp = (struct ext2fs_file *)malloc(sizeof(struct ext2fs_file));
	if (!fp)
		return FS_NO_ENTRY;

	return ext2fs_open_file_into(dev, path, private, fp);
}

/*
 * Clone an already-open file into a new ext2fs_file struct.
 * Shares the source's vnode (refcount bump) — changes by either
 * opener are immediately visible to the other.  No disk I/O.
 */
void
ext2fs_clone_file(struct ext2fs_file *dst, const struct ext2fs_file *src)
{
	int level;

	memset(dst, 0, sizeof(*dst));
	dst->f_dev = src->f_dev;

	/* Shared filesystem metadata (cached — no allocation needed) */
	dst->f_fs = src->f_fs;
	dst->f_gd = src->f_gd;
	dst->f_gd_size = src->f_gd_size;
	for (level = 0; level < NIADDR; level++)
		dst->f_nindir[level] = src->f_nindir[level];

	dst->f_ino = src->f_ino;

	/* Share the vnode — inode data is shared, not copied */
	dst->f_vnode = src->f_vnode;
	if (dst->f_vnode) {
		pthread_mutex_lock(&ext2_vnode_table_lock);
		dst->f_vnode->v_refcount++;
		pthread_mutex_unlock(&ext2_vnode_table_lock);
		dst->f_ic = &dst->f_vnode->v_ic;
		dst->f_gen = dst->f_vnode->v_gen;
	} else {
		dst->f_ic = &dst->f_ic_scratch;
	}

	/* Per-opener state: fresh */
	dst->f_ra_last_block = -1;
}

/*
 * Close file - free all storage used.
 */
void
ext2fs_close_file(
	fs_private_t private)
{
	register struct ext2fs_file *fp = (struct ext2fs_file *)private;

	/*
	 * Flush dirty metadata before closing.
	 */
	if (fp->f_vnode)
		ext2fs_flush_metadata(private);

	/*
	 * Free the disk super-block.
	 */
	unmount_fs(fp);

	/*
	 * Free the inode and data buffers.
	 */
	free_file_buffers(fp);

	/*
	 * Release vnode reference.
	 */
	if (fp->f_vnode) {
		vnode_put(fp->f_vnode);
		fp->f_vnode = NULL;
		fp->f_ic = &fp->f_ic_scratch;
	}
}

struct ext2_vnode *
ext2fs_file_vnode(fs_private_t private)
{
	struct ext2fs_file *fp = (struct ext2fs_file *)private;
	return fp->f_vnode;
}

/*
 * Copy a portion of a file into kernel memory.
 * Cross block boundaries when necessary.
 */
static int
ext2fs_read_file_impl(
	struct ext2fs_file	*fp,
	struct ext2_bref	*br,
	vm_offset_t		offset,
	vm_offset_t		start,
	vm_size_t		size)
{
	int			rc;
	register vm_size_t	csize;
	vm_offset_t		buf;
	vm_size_t		buf_size;

#ifdef	DEBUG
	if(debug)
		printf("ext2fs_read_file(%d, %d)\n", offset, size);
#endif 
	while (size != 0) {
	    buf = start;
	    buf_size = size;
	    rc = buf_read_file(fp, br, offset, &buf, &buf_size);
	    if (rc)
		return (rc);

	    csize = size;
	    if (csize > buf_size)
		csize = buf_size;
	    if (csize == 0)
		break;

	    if (buf != start)
		memcpy((char *)start, (const char *)buf, csize);

	    offset += csize;
	    start  += csize;
	    size   -= csize;
	}
#if 0
	if (resid)
	    *resid = size;
#endif
	return (0);
}

int
ext2fs_read_file(
	fs_private_t private,
	vm_offset_t		offset,
	vm_offset_t		start,
	vm_size_t		size)
{
	struct ext2fs_file	*fp = (struct ext2fs_file *)private;
	struct ext2_bref	br;
	int			rc;

	bref_init(&br);
	rc = ext2fs_read_file_impl(fp, &br, offset, start, size);
	bref_release(fp, &br);
	return (rc);
}

boolean_t
ext2fs_file_is_directory(fs_private_t private)
{
  	register struct ext2fs_file	*fp = (struct ext2fs_file *)private;
	return((fp->f_ic->i_mode & IFMT) == IFDIR);
}

/*
 * ext2fs_readdir — iterate every directory entry and fill an array of
 * fs_dirent.  Used by the bootstrap to enumerate module directories.
 *
 * Mirrors the block-walking loop in search_directory() but emits names
 * instead of matching one.  Stops when max entries are written; the
 * caller can detect truncation by comparing *out_count to what it
 * expected or by re-reading with a larger buffer.
 */
static int
ext2fs_readdir_impl(struct ext2fs_file *fp,
		    struct ext2_bref *br,
		    struct fs_dirent *out,
		    unsigned int max,
		    unsigned int *out_count)
{
	vm_offset_t		buf;
	vm_size_t		buf_size;
	vm_offset_t		offset;
	struct ext2_dir_entry	*dp;
	unsigned int		n = 0;
	int			rc;
	unsigned int		nlen;

	if ((fp->f_ic->i_mode & IFMT) != IFDIR)
		return FS_NOT_DIRECTORY;

	offset = 0;
	while (offset < fp->f_ic->i_size && n < max) {
		buf = 0;
		buf_size = 0;
		rc = buf_read_file(fp, br, offset, &buf, &buf_size);
		if (rc != KERN_SUCCESS)
			return rc;
		if (buf_size == 0)
			break;

		dp = (struct ext2_dir_entry *)buf;
		rc = ext2_dirent_check(fp, dp, buf_size, offset); /* #599 */
		if (rc != 0)
			return rc;

		if (le32_to_cpu(dp->inode) != 0) {
			nlen = dp->name_len;
			if (nlen > FS_DIRENT_NAME_MAX)
				nlen = FS_DIRENT_NAME_MAX;

			out[n].ino  = le32_to_cpu(dp->inode);
			out[n].type = dp->file_type;
			memcpy(out[n].name, dp->name, nlen);
			out[n].name[nlen] = '\0';
			n++;
		}

		offset += le16_to_cpu(dp->rec_len);
	}

	*out_count = n;
	return KERN_SUCCESS;
}

int
ext2fs_readdir(fs_private_t private,
	       struct fs_dirent *out,
	       unsigned int max,
	       unsigned int *out_count)
{
	struct ext2fs_file	*fp = (struct ext2fs_file *)private;
	struct ext2_bref	br;
	int			rc;

	bref_init(&br);
	rc = ext2fs_readdir_impl(fp, &br, out, max, out_count);
	bref_release(fp, &br);
	return rc;
}

size_t
ext2fs_file_size(fs_private_t private)
{
  	register struct ext2fs_file	*fp = (struct ext2fs_file *)private;
	return(fp->f_ic->i_size);
}

int
ext2fs_is_dirty(fs_private_t private)
{
	register struct ext2fs_file	*fp = (struct ext2fs_file *)private;
	int dirty;

	if (!fp->f_vnode)
		return 0;
	/*
	 * #599: under v_lock.  A flush takes the three flags when it starts
	 * and holds v_lock until it has written them or raised them again, so
	 * read here without the lock they were clear while one was in flight,
	 * and the writeback thread dropped a handle whose flush then failed
	 * (found in review, twice: a count of flushes in flight still left a
	 * window at each end).  No caller holds v_lock; the writeback thread
	 * and ds_ext2_sync hold of_lock, the order their own flushes take the
	 * two in, and the write paths' failure branch holds neither.
	 */
	vnode_mutex_lock(fp);
	dirty = fp->f_vnode->v_inode_dirty || fp->f_vnode->v_gd_dirty ||
		fp->f_vnode->v_super_dirty;
	vnode_mutex_unlock(fp);
	return dirty;
}

boolean_t
ext2fs_file_is_executable(fs_private_t private)
{
  	register struct ext2fs_file	*fp = (struct ext2fs_file *)private;
	if ((fp->f_ic->i_mode & IFMT) != IFREG
#if 0
	    || (fp->f_ic->i_mode & (IEXEC|IEXEC>>3|IEXEC>>6)) == 0
#endif
	    ) {
		return (FALSE);
	}
	return(TRUE);
}

/* ================================================================
 * Write support
 * ================================================================ */

/*
 * Write a raw disk block (in fs block units) to the device.
 */
static int
write_disk_block(
	struct ext2fs_file	*fp,
	daddr_t			disk_block,
	vm_offset_t		data,
	vm_size_t		size)
{
	io_buf_len_t bytes_written;
	kern_return_t rc;

	rc = ext2_dev_write(&fp->f_dev,
			  (recnum_t) dbtorec(&fp->f_dev,
					     ext2_fsbtodb(fp->f_fs,
							 disk_block)),
			  (io_buf_ptr_t) data,
			  (mach_msg_type_number_t) size,
			  &bytes_written);
	if (rc != KERN_SUCCESS)
		return rc;
	if ((vm_size_t)bytes_written != size)
		return KERN_FAILURE;
	return 0;
}

/*
 * Read a raw disk block (in fs block units) from the device.
 */
static int
read_disk_block(
	struct ext2fs_file	*fp,
	daddr_t			disk_block,
	vm_offset_t		*data_out,
	vm_size_t		*size_out)
{
	return ext2_dev_read(&fp->f_dev,
			   (recnum_t) dbtorec(&fp->f_dev,
					      ext2_fsbtodb(fp->f_fs,
							  disk_block)),
			   (int) EXT2_BLOCK_SIZE(fp->f_fs),
			   (char **) data_out,
			   size_out);
}

/*
 * Allocate a free block from the filesystem.
 * Prefers the block group 'goal_group' (or 0 for any).
 * Returns the allocated disk block number, or 0 on failure.
 *
 * Updates: block bitmap on disk, group descriptor free count,
 * superblock free count.
 */
static daddr_t
block_alloc_impl(struct ext2fs_file *fp, int goal_group)
{
	struct ext2_super_block *fs = fp->f_fs;
	struct ext2_group_desc *gd = fp->f_gd;
	int ngroups;
	int group, g;
	vm_offset_t bitmap;
	vm_size_t bitmap_size;
	int block_size = EXT2_BLOCK_SIZE(fs);
	int bits_per_group = fs->s_blocks_per_group;
	int rc;
	daddr_t alloc_block;

	ngroups = (fs->s_blocks_count - fs->s_first_data_block +
		   fs->s_blocks_per_group - 1) / fs->s_blocks_per_group;

	/* Search from goal_group, wrapping around */
	for (g = 0; g < ngroups; g++) {
		group = (goal_group + g) % ngroups;

		if (le16_to_cpu(gd[group].bg_free_blocks_count) == 0)
			continue;

		/* Read block bitmap for this group */
		rc = read_disk_block(fp, le32_to_cpu(gd[group].bg_block_bitmap),
				     &bitmap, &bitmap_size);
		if (rc != 0)
			continue;

		/* Scan bitmap for a free bit */
		{
			unsigned char *bm = (unsigned char *)bitmap;
			int bit;
			int max_bit = bits_per_group;

			/* Last group may have fewer blocks */
			if (group == ngroups - 1) {
				int remaining = fs->s_blocks_count -
					fs->s_first_data_block -
					(unsigned long)group * bits_per_group;
				if (remaining < max_bit)
					max_bit = remaining;
			}

			for (bit = 0; bit < max_bit; bit++) {
				if (!(bm[bit >> 3] & (1 << (bit & 7)))) {
					/* Found free block — mark as used */
					bm[bit >> 3] |= (1 << (bit & 7));

					/* Write bitmap back */
					rc = write_disk_block(fp,
						le32_to_cpu(gd[group].bg_block_bitmap),
						bitmap, block_size);
					if (rc != 0) {
						vm_deallocate(mach_task_self(),
							      bitmap, bitmap_size);
						return 0;
					}

					/* Update group descriptor */
					gd[group].bg_free_blocks_count =
						cpu_to_le16(
						le16_to_cpu(gd[group].bg_free_blocks_count) - 1);
					fp->f_vnode->v_gd_dirty = 1;

					/* Update superblock */
					fs->s_free_blocks_count--;
					fp->f_vnode->v_super_dirty = 1;

					alloc_block = fs->s_first_data_block +
						(unsigned long)group * bits_per_group + bit;

					vm_deallocate(mach_task_self(),
						      bitmap, bitmap_size);
					return alloc_block;
				}
			}
		}

		vm_deallocate(mach_task_self(), bitmap, bitmap_size);
	}

	return 0;	/* no space */
}

/*
 * Free a previously allocated data block: clear its bit in the block
 * bitmap and bump the group + superblock free counts.  Inverse of
 * block_alloc().  Dirty flags are raised on fp's vnode so the next
 * ext2fs_flush_metadata() writes the descriptors and superblock back.
 */
static void
block_free_impl(struct ext2fs_file *fp, daddr_t block)
{
	struct ext2_super_block *fs = fp->f_fs;
	struct ext2_group_desc *gd = fp->f_gd;
	int bits_per_group = fs->s_blocks_per_group;
	int block_size = EXT2_BLOCK_SIZE(fs);
	vm_offset_t bitmap;
	vm_size_t bitmap_size;
	unsigned char *bm;
	int group, bit, rc;

	if (block < (daddr_t)fs->s_first_data_block ||
	    block >= (daddr_t)fs->s_blocks_count)
		return;

	group = (block - fs->s_first_data_block) / bits_per_group;
	bit   = (block - fs->s_first_data_block) % bits_per_group;

	rc = read_disk_block(fp, le32_to_cpu(gd[group].bg_block_bitmap),
			     &bitmap, &bitmap_size);
	if (rc != 0)
		return;

	bm = (unsigned char *)bitmap;
	if (bm[bit >> 3] & (1 << (bit & 7))) {
		bm[bit >> 3] &= ~(1 << (bit & 7));
		rc = write_disk_block(fp, le32_to_cpu(gd[group].bg_block_bitmap),
				      bitmap, block_size);
		if (rc == 0) {
			gd[group].bg_free_blocks_count = cpu_to_le16(
				le16_to_cpu(gd[group].bg_free_blocks_count) + 1);
			fs->s_free_blocks_count++;
			if (fp->f_vnode) {
				fp->f_vnode->v_gd_dirty = 1;
				fp->f_vnode->v_super_dirty = 1;
			}
		}
	}
	vm_deallocate(mach_task_self(), bitmap, bitmap_size);
}

/*
 * #384: allocator entry points — the bitmap read-modify-write, group
 * descriptor counts and superblock counts are shared by every file on
 * the mount; concurrent allocators used to lose each other's bitmap
 * updates and hand the same block to two writers.  ext2_alloc_lock is
 * a leaf lock (nothing else is acquired while holding it).
 */
/*
 * #599: a block that changes owner leaves nothing of its old owner in the
 * page cache.  A file removed with dirty blocks still cached left them there:
 * the next file or directory to take one of those blocks read the old owner's
 * bytes back through the cache, and the next sync wrote them over the new
 * owner's.  block_free discards before the block goes back in the bitmap --
 * every caller has already decided its content is dead, and until the bit is
 * clear nobody else can be handed it -- and block_alloc discards again for
 * the new owner, since a reader with a stale map may have cached it in
 * between.  Both outside ext2_alloc_lock, which stays a leaf: a discard can
 * wait for a fill or a writeback of the block.
 */
static daddr_t
block_alloc(struct ext2fs_file *fp, int goal_group)
{
	daddr_t b;

	pthread_mutex_lock(&ext2_alloc_lock);
	b = block_alloc_impl(fp, goal_group);
	pthread_mutex_unlock(&ext2_alloc_lock);
	if (b != 0 && fp->f_dev.cache)
		page_cache_discard(fp->f_dev.cache, b);
	return b;
}

static void
block_free(struct ext2fs_file *fp, daddr_t block)
{
	if (fp->f_dev.cache)
		page_cache_discard(fp->f_dev.cache, block);
	pthread_mutex_lock(&ext2_alloc_lock);
	block_free_impl(fp, block);
	pthread_mutex_unlock(&ext2_alloc_lock);
}

/*
 * Allocate a free inode, mirroring block_alloc() against the inode
 * bitmap.  Reserved inodes (< EXT2_FIRST_INO) are skipped — they are
 * already marked used in group 0's bitmap, but we guard anyway.  When
 * is_dir is set the group's used-directory count is bumped (ext2 uses it
 * for the Orlov-style allocator; we just keep it consistent).
 *
 * Returns the 1-based inode number, or 0 on failure.  Raises the gd +
 * superblock dirty flags on fp's vnode.
 */
static ino_t
inode_alloc_impl(struct ext2fs_file *fp, int goal_group, int is_dir)
{
	struct ext2_super_block *fs = fp->f_fs;
	struct ext2_group_desc *gd = fp->f_gd;
	int ipg = fs->s_inodes_per_group;
	int block_size = EXT2_BLOCK_SIZE(fs);
	int first_ino = EXT2_FIRST_INO(fs);
	int ngroups, group, g, bit, rc;
	vm_offset_t bitmap;
	vm_size_t bitmap_size;

	ngroups = (fs->s_inodes_count + ipg - 1) / ipg;

	for (g = 0; g < ngroups; g++) {
		group = (goal_group + g) % ngroups;

		if (le16_to_cpu(gd[group].bg_free_inodes_count) == 0)
			continue;

		rc = read_disk_block(fp, le32_to_cpu(gd[group].bg_inode_bitmap),
				     &bitmap, &bitmap_size);
		if (rc != 0)
			continue;

		{
			unsigned char *bm = (unsigned char *)bitmap;

			for (bit = 0; bit < ipg; bit++) {
				ino_t ino = (ino_t)group * ipg + bit + 1;
				if (ino < (ino_t)first_ino)
					continue;
				if (bm[bit >> 3] & (1 << (bit & 7)))
					continue;

				bm[bit >> 3] |= (1 << (bit & 7));
				rc = write_disk_block(fp,
					le32_to_cpu(gd[group].bg_inode_bitmap),
					bitmap, block_size);
				if (rc != 0) {
					vm_deallocate(mach_task_self(),
						      bitmap, bitmap_size);
					return 0;
				}

				gd[group].bg_free_inodes_count = cpu_to_le16(
					le16_to_cpu(gd[group].bg_free_inodes_count) - 1);
				if (is_dir)
					gd[group].bg_used_dirs_count = cpu_to_le16(
						le16_to_cpu(gd[group].bg_used_dirs_count) + 1);
				fs->s_free_inodes_count--;
				if (fp->f_vnode) {
					fp->f_vnode->v_gd_dirty = 1;
					fp->f_vnode->v_super_dirty = 1;
				}

				vm_deallocate(mach_task_self(),
					      bitmap, bitmap_size);
				return ino;
			}
		}

		vm_deallocate(mach_task_self(), bitmap, bitmap_size);
	}

	return 0;	/* no free inode */
}

/*
 * Release an inode: clear its bit in the inode bitmap, bump free counts
 * (and drop the group's used-dirs count when freeing a directory).
 * Inverse of inode_alloc().  The caller is responsible for having freed
 * the inode's data blocks first.
 */
static void
inode_free_impl(struct ext2fs_file *fp, ino_t ino, int is_dir)
{
	struct ext2_super_block *fs = fp->f_fs;
	struct ext2_group_desc *gd = fp->f_gd;
	int ipg = fs->s_inodes_per_group;
	int block_size = EXT2_BLOCK_SIZE(fs);
	vm_offset_t bitmap;
	vm_size_t bitmap_size;
	unsigned char *bm;
	int group, bit, rc;

	if (ino < (ino_t)EXT2_FIRST_INO(fs) || ino > (ino_t)fs->s_inodes_count)
		return;

	group = (ino - 1) / ipg;
	bit   = (ino - 1) % ipg;

	rc = read_disk_block(fp, le32_to_cpu(gd[group].bg_inode_bitmap),
			     &bitmap, &bitmap_size);
	if (rc != 0)
		return;

	bm = (unsigned char *)bitmap;
	if (bm[bit >> 3] & (1 << (bit & 7))) {
		bm[bit >> 3] &= ~(1 << (bit & 7));
		rc = write_disk_block(fp, le32_to_cpu(gd[group].bg_inode_bitmap),
				      bitmap, block_size);
		if (rc == 0) {
			gd[group].bg_free_inodes_count = cpu_to_le16(
				le16_to_cpu(gd[group].bg_free_inodes_count) + 1);
			if (is_dir)
				gd[group].bg_used_dirs_count = cpu_to_le16(
					le16_to_cpu(gd[group].bg_used_dirs_count) - 1);
			fs->s_free_inodes_count++;
			if (fp->f_vnode) {
				fp->f_vnode->v_gd_dirty = 1;
				fp->f_vnode->v_super_dirty = 1;
			}
		}
	}
	vm_deallocate(mach_task_self(), bitmap, bitmap_size);
}

/* #384: locked wrappers — same rationale as block_alloc/block_free. */
static ino_t
inode_alloc(struct ext2fs_file *fp, int goal_group, int is_dir)
{
	ino_t ino;

	pthread_mutex_lock(&ext2_alloc_lock);
	ino = inode_alloc_impl(fp, goal_group, is_dir);
	pthread_mutex_unlock(&ext2_alloc_lock);
	return ino;
}

static void
inode_free(struct ext2fs_file *fp, ino_t ino, int is_dir)
{
	pthread_mutex_lock(&ext2_alloc_lock);
	inode_free_impl(fp, ino, is_dir);
	pthread_mutex_unlock(&ext2_alloc_lock);
}

/* ext2 on-disk directory-entry file_type values (FILETYPE feature). */
#define EXT2_FT_REG_FILE	1
#define EXT2_FT_DIR		2

/*
 * #599: a directory block written straight to the disk, then given to the
 * cache -- only once the disk has it, so a write that fails leaves the cache
 * with what the disk still holds.  The directory edits used to happen in the
 * cached block itself: a failed write left the cache answering with a name
 * the disk never got, until the block was evicted.  `data' is the caller's
 * own buffer, never a page-cache slot: a DMA-pool page is not handed to the
 * copy path.
 */
static int
write_data_block(struct ext2fs_file *fp, daddr_t dblk, vm_offset_t data,
		 vm_size_t size)
{
	int rc = write_disk_block(fp, dblk, data, size);

	if (rc == 0 && fp->f_dev.cache)
		rc = page_cache_wrote(fp->f_dev.cache, dblk, data, size);
	return rc;
}

/*
 * A copy of the directory block at `buf', to be edited and written with
 * dir_write_block; NULL when there is no memory.  The caller frees it.
 */
static char *
dir_block_copy(vm_offset_t buf, int bs)
{
	char *copy = malloc(bs);

	if (copy != NULL)
		memcpy(copy, (void *)buf, bs);
	return copy;
}

/* Write logical directory block `lblk', edited in `copy'. */
static int
dir_write_block(struct ext2fs_file *dir_fp, daddr_t lblk, const char *copy)
{
	daddr_t dblk;
	int rc = block_map(dir_fp, lblk, &dblk);

	if (rc != 0)
		return rc;
	return write_data_block(dir_fp, dblk, (vm_offset_t)copy,
				EXT2_BLOCK_SIZE(dir_fp->f_fs));
}

/*
 * Append a fresh data block to a directory holding a single empty entry
 * that spans the whole block, then place (name -> ino) in it.  Only the
 * direct-block range is handled: directories deep enough to need an
 * indirect block (> 12 blocks ~= 48 KiB of entries) are far beyond what
 * this bootstrap fs is expected to grow, and we fail cleanly instead.
 */
static int
dir_grow_and_add(struct ext2fs_file *dir_fp, const char *name, ino_t ino,
		 int file_type, int name_len)
{
	struct ext2_super_block *fs = dir_fp->f_fs;
	int block_size = EXT2_BLOCK_SIZE(fs);
	daddr_t lblk = dir_fp->f_ic->i_size / block_size;
	int goal_group;
	daddr_t newblk;
	char *blk;
	struct ext2_dir_entry *dp;
	int rc;

	if (lblk >= NDADDR)
		return KERN_RESOURCE_SHORTAGE;

	goal_group = (dir_fp->f_ino - 1) / fs->s_inodes_per_group;
	newblk = block_alloc(dir_fp, goal_group);
	if (newblk == 0)
		return KERN_RESOURCE_SHORTAGE;

	blk = (char *)malloc(block_size);
	if (!blk) {
		block_free(dir_fp, newblk);
		return KERN_RESOURCE_SHORTAGE;
	}
	memset(blk, 0, block_size);

	dp = (struct ext2_dir_entry *)blk;
	dp->inode = cpu_to_le32((unsigned long)ino);
	dp->rec_len = cpu_to_le16(block_size);
	dp->name_len = (unsigned char)name_len;
	dp->file_type = (unsigned char)file_type;
	memcpy(dp->name, name, name_len);

	rc = write_data_block(dir_fp, newblk, (vm_offset_t)blk, block_size);
	free(blk);
	if (rc != 0) {
		block_free(dir_fp, newblk);
		return rc;
	}

	/* Link the new block into the inode and extend its size. */
	dir_fp->f_ic->i_block[lblk] = newblk;
	dir_fp->f_ic->i_size += block_size;
	dir_fp->f_ic->i_blocks += block_size / 512;
	if (dir_fp->f_vnode)
		dir_fp->f_vnode->v_inode_dirty = 1;

	{
		struct ext2_mount *m =
			(struct ext2_mount *)dir_fp->f_dev.mount_data;
		if (m)
			dcache_insert(m, dir_fp->f_ino, name, ino);
	}
	return 0;
}

/*
 * Insert (name -> ino) into directory dir_fp.  Walks each directory
 * block for a usable slot: a deleted entry (inode==0) whose rec_len is
 * large enough, or trailing slack in a live entry that can be split off.
 * Grows the directory by a block if nothing fits.  Returns 0 / error.
 *
 * dir_fp must have its inode loaded and a vnode (for dirty flags).  The
 * block is read through buf_read_file and edited in a copy (#599), which
 * dir_write_block puts on the disk and then in the cache.
 */
static int
dir_add_entry_impl(struct ext2fs_file *dir_fp, struct ext2_bref *br,
		   const char *name, ino_t ino, int file_type)
{
	struct ext2_super_block *fs = dir_fp->f_fs;
	int block_size = EXT2_BLOCK_SIZE(fs);
	int name_len = (int)strlen(name);
	int needed = EXT2_DIR_REC_LEN(name_len);
	struct ext2_mount *m = (struct ext2_mount *)dir_fp->f_dev.mount_data;
	vm_offset_t offset;

	if (name_len == 0 || name_len > EXT2_NAME_LEN)
		return FS_NAME_TOO_LONG;

	for (offset = 0; offset < dir_fp->f_ic->i_size; offset += block_size) {
		vm_offset_t buf = 0;
		vm_size_t buf_size = 0;
		daddr_t lblk = offset / block_size;
		int off, rc;

		rc = buf_read_file(dir_fp, br, offset, &buf, &buf_size);
		if (rc != 0)
			return rc;
		if (buf_size > (vm_size_t)block_size)
			buf_size = block_size;

		off = 0;
		while ((vm_size_t)off < buf_size) {
			struct ext2_dir_entry *dp =
				(struct ext2_dir_entry *)((char *)buf + off);
			int rec_len;
			int used;

			/* #599: a damaged block is refused, not skipped */
			rc = ext2_dirent_check(dir_fp, dp, buf_size - off,
					       offset + off);
			if (rc != 0)
				return rc;
			rec_len = le16_to_cpu(dp->rec_len);

			used = (le32_to_cpu(dp->inode) == 0)
				? 0 : EXT2_DIR_REC_LEN(dp->name_len);

			if (rec_len - used >= needed) {
				struct ext2_dir_entry *ne;
				char *copy = dir_block_copy(buf, block_size);

				if (copy == NULL)
					return KERN_RESOURCE_SHORTAGE;
				dp = (struct ext2_dir_entry *)(copy + off);
				if (used == 0) {
					ne = dp;	/* reuse deleted slot whole */
				} else {
					ne = (struct ext2_dir_entry *)
						((char *)dp + used);
					ne->rec_len = cpu_to_le16(rec_len - used);
					dp->rec_len = cpu_to_le16(used);
				}
				ne->inode = cpu_to_le32((unsigned long)ino);
				ne->name_len = (unsigned char)name_len;
				ne->file_type = (unsigned char)file_type;
				memcpy(ne->name, name, name_len);

				rc = dir_write_block(dir_fp, lblk, copy);
				free(copy);
				if (rc != 0)
					return rc;
				if (m)
					dcache_insert(m, dir_fp->f_ino, name, ino);
				return 0;
			}
			off += rec_len;
		}
	}

	return dir_grow_and_add(dir_fp, name, ino, file_type, name_len);
}

static int
dir_add_entry(struct ext2fs_file *dir_fp, const char *name, ino_t ino,
	      int file_type)
{
	struct ext2_bref br;
	int rc;

	bref_init(&br);
	rc = dir_add_entry_impl(dir_fp, &br, name, ino, file_type);
	bref_release(dir_fp, &br);
	return rc;
}

/*
 * Remove the entry 'name' from directory dir_fp.  On success the removed
 * inode number is returned through ino_out so the caller can drop link
 * counts / free the inode.  Removal is the classic ext2 tombstone: the
 * entry's rec_len is folded into the previous record, or inode is zeroed
 * when it is the first record in the block.  Returns 0 / FS_NO_ENTRY, or
 * FS_CORRUPT for a damaged block (#599).
 */
static int
dir_remove_entry_impl(struct ext2fs_file *dir_fp, struct ext2_bref *br,
		      const char *name, ino_t *ino_out)
{
	struct ext2_super_block *fs = dir_fp->f_fs;
	int block_size = EXT2_BLOCK_SIZE(fs);
	int name_len = (int)strlen(name);
	struct ext2_mount *m = (struct ext2_mount *)dir_fp->f_dev.mount_data;
	vm_offset_t offset;

	for (offset = 0; offset < dir_fp->f_ic->i_size; offset += block_size) {
		vm_offset_t buf = 0;
		vm_size_t buf_size = 0;
		daddr_t lblk = offset / block_size;
		int prev_off = -1;
		int off, rc;

		rc = buf_read_file(dir_fp, br, offset, &buf, &buf_size);
		if (rc != 0)
			return rc;
		if (buf_size > (vm_size_t)block_size)
			buf_size = block_size;

		off = 0;
		while ((vm_size_t)off < buf_size) {
			struct ext2_dir_entry *dp =
				(struct ext2_dir_entry *)((char *)buf + off);
			int rec_len;

			/*
			 * #599: refused, not skipped -- skipping answered
			 * FS_NO_ENTRY for a name after the damage.
			 */
			rc = ext2_dirent_check(dir_fp, dp, buf_size - off,
					       offset + off);
			if (rc != 0)
				return rc;
			rec_len = le16_to_cpu(dp->rec_len);

			if (le32_to_cpu(dp->inode) != 0 &&
			    dp->name_len == name_len &&
			    memcmp(dp->name, name, name_len) == 0) {
				char *copy = dir_block_copy(buf, block_size);

				if (copy == NULL)
					return KERN_RESOURCE_SHORTAGE;
				if (ino_out)
					*ino_out = (ino_t)le32_to_cpu(dp->inode);
				if (prev_off >= 0) {
					struct ext2_dir_entry *prev =
						(struct ext2_dir_entry *)
						(copy + prev_off);
					prev->rec_len = cpu_to_le16(
						le16_to_cpu(prev->rec_len) + rec_len);
				} else {
					((struct ext2_dir_entry *)(copy + off))
						->inode = cpu_to_le32(0);
				}

				rc = dir_write_block(dir_fp, lblk, copy);
				free(copy);
				if (rc != 0)
					return rc;
				if (m)
					dcache_insert(m, dir_fp->f_ino, name,
						      DCACHE_NEGATIVE);
				return 0;
			}
			prev_off = off;
			off += rec_len;
		}
	}
	return FS_NO_ENTRY;
}

static int
dir_remove_entry(struct ext2fs_file *dir_fp, const char *name, ino_t *ino_out)
{
	struct ext2_bref br;
	int rc;

	bref_init(&br);
	rc = dir_remove_entry_impl(dir_fp, &br, name, ino_out);
	bref_release(dir_fp, &br);
	return rc;
}

/*
 * #599 E6, for ext2_blockio_selftest: a directory edit reaches the cache only
 * once the disk has it.  A directory of one block (disk 23), held in the
 * cache with one record, is given a name on the device that answers nothing:
 * the write fails, and the cached block must still be the one the disk has.
 */
static void
ext2_selftest_dir(struct ext2fs_file *f, struct page_cache *pc,
		  unsigned int *ran, unsigned int *wrong)
{
	unsigned char		 blk[1024];
	struct ext2_dir_entry	*dp = (struct ext2_dir_entry *)blk;
	struct page_cache_entry	*e = NULL;
	unsigned short		 mode = f->f_ic->i_mode;
	unsigned long		 isize = f->f_ic->i_size;
	int			 rc, same = 0;

	memset(blk, 0, sizeof(blk));
	dp->inode = cpu_to_le32(2);
	dp->rec_len = cpu_to_le16(sizeof(blk));
	dp->name_len = 1;
	dp->file_type = EXT2_FT_DIR;
	dp->name[0] = '.';
	(void) page_cache_install(pc, 23, (vm_offset_t)blk, sizeof(blk),
				  page_cache_ticket(pc));
	f->f_ic->i_mode = IFDIR | 0755;
	f->f_ic->i_size = sizeof(blk);
	f->f_ic->i_block[0] = 23;
	f->f_ra_last_block = (daddr_t)-2;
	rc = dir_add_entry(f, "e6", 5, EXT2_FT_REG_FILE);
	if (page_cache_get(pc, 23, ext2_fill, f, &e) == 0 && e != NULL) {
		same = memcmp((void *)e->pc_data, blk, sizeof(blk)) == 0;
		page_cache_put(pc, e);
	}
	f->f_ic->i_block[0] = 0;
	f->f_ic->i_size = isize;
	f->f_ic->i_mode = mode;
	(*ran)++;
	if (rc == 0 || !same)
		(*wrong)++;
}

/*
 * Serialize the in-core inode into the vnode's inode block,
 * fp->f_vnode->v_inode_blk, which the caller has just read afresh under
 * ext2_itable_lock (vnode_inode_block, #599).
 */
static void
serialize_inode(struct ext2fs_file *fp)
{
	struct ext2_super_block *fs = fp->f_fs;
	struct ext2_inode *raw_inode;
	struct ext2_inode *inode = fp->f_ic;
	unsigned long block;

	raw_inode = (struct ext2_inode *)
		((char *)fp->f_vnode->v_inode_blk +
		 ext2_itoo(fs, fp->f_vnode->v_ino) * EXT2_INODE_SIZE(fs));

	raw_inode->i_mode = cpu_to_le16(inode->i_mode);
	raw_inode->i_uid = cpu_to_le16(inode->i_uid);
	raw_inode->i_size = cpu_to_le32(inode->i_size);
	raw_inode->i_atime = cpu_to_le32(inode->i_atime);
	raw_inode->i_ctime = cpu_to_le32(inode->i_ctime);
	raw_inode->i_mtime = cpu_to_le32(inode->i_mtime);
	raw_inode->i_dtime = cpu_to_le32(inode->i_dtime);
	raw_inode->i_gid = cpu_to_le16(inode->i_gid);
	raw_inode->i_links_count = cpu_to_le16(inode->i_links_count);
	raw_inode->i_blocks = cpu_to_le32(inode->i_blocks);
	raw_inode->i_flags = cpu_to_le32(inode->i_flags);
	for (block = 0; block < EXT2_N_BLOCKS; block++)
		raw_inode->i_block[block] = cpu_to_le32(inode->i_block[block]);
	raw_inode->i_file_acl = cpu_to_le32(inode->i_file_acl);
	raw_inode->i_dir_acl = cpu_to_le32(inode->i_dir_acl);
	raw_inode->i_faddr = cpu_to_le32(inode->i_faddr);
	raw_inode->i_frag = inode->i_frag;
	raw_inode->i_fsize = inode->i_fsize;
}

/*
 * The block that holds the vnode's inode, which both ways of flushing it
 * serialize into.
 *
 * #599: the batched flush kept the silent KERN_FAILURE that #483 took out of
 * write_inode below.  A create, mkdir or rmdir whose parent directory came
 * from the inode cache dirties the parent's inode, the group descriptors and
 * the superblock -- three items, so the batch -- and lost all three writes
 * without a word: ext2fs_close_file does not look at the answer.  The group
 * counts of the last such operation before a quiet period never reached the
 * disk, and e2fsck said so.  Both paths now ask here.
 */
static int
vnode_inode_block(struct ext2fs_file *fp, ino_t inumber, daddr_t disk_block)
{
	struct ext2_super_block *fs = fp->f_fs;
	struct ext2_vnode *vn = fp->f_vnode;

	/*
	 * 🔴 #483: READ THE BLOCK IF NOBODY HAS.  This used to return
	 * KERN_FAILURE here, silently, and it is why ext2_sync() had never
	 * once succeeded after a metadata-dirtying write -- in every captured
	 * log back to 2026-07-21.
	 *
	 * 🔑 The cause is that a READ optimisation disabled the WRITE-BACK.
	 * read_inode() checks the inode cache first and, on a hit, "copies the
	 * cached inode directly without any device I/O" -- which is its whole
	 * value.  But the first thing that path does is free_file_buffers(),
	 * which clears f_inode_blk, and it returns without ever reading the
	 * block that HOLDS the inode.  The vnode then inherits a zero, and
	 * every flush of that inode fails for the life of the mount.
	 *
	 * ⚠️ Read HERE and not on the cache hit.  Reading it there would undo
	 * exactly the I/O the cache exists to avoid, on every open of every
	 * file, to serve a write-back that most of them never do.  Here it is
	 * paid by the flush that needs it, beside a write it is already doing.
	 *
	 * 🔴 #599: and read EVERY time, under ext2_itable_lock, which the
	 * caller holds until the block is written.  The block holds other
	 * inodes, and write_new_inode writes them; a copy kept from an earlier
	 * read wrote them back as they were then -- a file made while another
	 * was open lost its inode at the other's next flush.
	 */
	{
		vm_offset_t		buf;
		vm_size_t		buf_size;
		int			rc;

		rc = ext2_dev_read(&fp->f_dev,
				   (recnum_t) dbtorec(&fp->f_dev,
					ext2_fsbtodb(fs, disk_block)),
				   (int) EXT2_BLOCK_SIZE(fs),
				   (char **)&buf, &buf_size);
		if (rc != KERN_SUCCESS) {
			printf("ext2: inode %u: its block could not be read "
			       "back for a flush (rc=%d)\n", (unsigned)inumber,
			       rc);
			return rc;
		}

		if (vn->v_inode_blk != 0)
			(void) vm_deallocate(mach_task_self(), vn->v_inode_blk,
					     vn->v_inode_blk_size);
		vn->v_inode_blk = buf;
		vn->v_inode_blk_size = buf_size;
	}
	return 0;
}

/*
 * #599: the inode cache answers read_inode when a vnode is made afresh, so
 * once a vnode has written its inode the cache holds what was written, or
 * nothing.  It kept the inode as first read: a directory whose vnode went
 * away between a mkdir and an rmdir inside it came back with its link count
 * from before the mkdir, and the rmdir wrote one link fewer than it had --
 * hidden for as long as both flushes were being lost in silence.
 */
static void
icache_follow(struct ext2fs_file *fp, int rc)
{
	struct ext2_mount *m = (struct ext2_mount *)fp->f_dev.mount_data;

	if (m == NULL)
		return;
	if (rc == 0)
		icache_insert(m, fp->f_vnode->v_ino, &fp->f_vnode->v_ic);
	else
		icache_invalidate(m, fp->f_vnode->v_ino);
}

/*
 * Write the inode back to disk: its block read afresh under ext2_itable_lock
 * (vnode_inode_block), this inode serialized into it, the block written back
 * while the lock is held (#599).
 */
static int
write_inode(ino_t inumber, struct ext2fs_file *fp)
{
	struct ext2_super_block *fs = fp->f_fs;
	struct ext2_vnode *vn = fp->f_vnode;
	daddr_t disk_block;
	int rc;

	if (!vn) {
		printf("ext2: write_inode %u: no vnode\n",
		       (unsigned)inumber);
		return KERN_FAILURE;
	}

	disk_block = ext2_ino2blk(fs, fp->f_gd, inumber);
	pthread_mutex_lock(&ext2_itable_lock);
	rc = vnode_inode_block(fp, inumber, disk_block);
	if (rc == 0) {
		serialize_inode(fp);
		rc = write_disk_block(fp, disk_block, vn->v_inode_blk,
				      EXT2_BLOCK_SIZE(fs));
	}
	pthread_mutex_unlock(&ext2_itable_lock);
	return rc;
}

/*
 * Write the group descriptors back to disk.
 */
static int
write_gd(struct ext2fs_file *fp)
{
	struct ext2_super_block *fs = fp->f_fs;
	int gd_location = fs->s_first_data_block + 1;
	int gd_sector = (gd_location * EXT2_BLOCK_SIZE(fs)) / DEV_BSIZE;
	io_buf_len_t bytes_written;

	return ext2_dev_write(&fp->f_dev,
			    (recnum_t) dbtorec(&fp->f_dev, gd_sector),
			    (io_buf_ptr_t) fp->f_gd,
			    (mach_msg_type_number_t) fp->f_gd_size,
			    &bytes_written);
}

/*
 * Write the superblock back to disk.
 */
static int
write_super(struct ext2fs_file *fp)
{
	struct ext2_super_block *fs = fp->f_fs;
	struct ext2_super_block raw;
	io_buf_len_t bytes_written;

	/* Start from the full in-core superblock so fields we don't touch
	 * (s_uuid, s_reserved_gdt_blocks, volume name, hash seed, ...) are
	 * preserved verbatim; zeroing them corrupted the resize inode's
	 * reserved-GDT blocks and dropped the UUID (#266).  The cpu_to_le
	 * overrides below re-encode the mutable/structural fields for BE. */
	raw = *fs;
	raw.s_inodes_count = cpu_to_le32(fs->s_inodes_count);
	raw.s_blocks_count = cpu_to_le32(fs->s_blocks_count);
	raw.s_r_blocks_count = cpu_to_le32(fs->s_r_blocks_count);
	raw.s_free_blocks_count = cpu_to_le32(fs->s_free_blocks_count);
	raw.s_free_inodes_count = cpu_to_le32(fs->s_free_inodes_count);
	raw.s_first_data_block = cpu_to_le32(fs->s_first_data_block);
	raw.s_log_block_size = cpu_to_le32(fs->s_log_block_size);
	raw.s_log_frag_size = cpu_to_le32(fs->s_log_frag_size);
	raw.s_blocks_per_group = cpu_to_le32(fs->s_blocks_per_group);
	raw.s_frags_per_group = cpu_to_le32(fs->s_frags_per_group);
	raw.s_inodes_per_group = cpu_to_le32(fs->s_inodes_per_group);
	raw.s_mtime = cpu_to_le32(fs->s_mtime);
	raw.s_wtime = cpu_to_le32(fs->s_wtime);
	raw.s_mnt_count = cpu_to_le16(fs->s_mnt_count);
	raw.s_max_mnt_count = cpu_to_le16(fs->s_max_mnt_count);
	raw.s_magic = cpu_to_le16(fs->s_magic);
	raw.s_state = cpu_to_le16(fs->s_state);
	raw.s_errors = cpu_to_le16(fs->s_errors);
	raw.s_minor_rev_level = cpu_to_le16(fs->s_minor_rev_level);
	raw.s_lastcheck = cpu_to_le32(fs->s_lastcheck);
	raw.s_checkinterval = cpu_to_le32(fs->s_checkinterval);
	raw.s_creator_os = cpu_to_le32(fs->s_creator_os);
	raw.s_rev_level = cpu_to_le32(fs->s_rev_level);
	raw.s_def_resuid = cpu_to_le16(fs->s_def_resuid);
	raw.s_def_resgid = cpu_to_le16(fs->s_def_resgid);
	if (fs->s_rev_level >= EXT2_DYNAMIC_REV) {
		raw.s_first_ino = cpu_to_le32(fs->s_first_ino);
		raw.s_inode_size = cpu_to_le16(fs->s_inode_size);
		raw.s_block_group_nr = cpu_to_le16(fs->s_block_group_nr);
		raw.s_feature_compat = cpu_to_le32(fs->s_feature_compat);
		raw.s_feature_incompat = cpu_to_le32(fs->s_feature_incompat);
		raw.s_feature_ro_compat = cpu_to_le32(fs->s_feature_ro_compat);
	}

	return ext2_dev_write(&fp->f_dev,
			    (recnum_t) dbtorec(&fp->f_dev, SBLOCK),
			    (io_buf_ptr_t) &raw,
			    (mach_msg_type_number_t) SBSIZE,
			    &bytes_written);
}

/*
 * #284 phase 2 (Linux-compliant): mark the volume cleanly unmounted —
 * set EXT2_VALID_FS in the cached superblock and flush it straight to
 * disk (ext2_dev_write bypasses the page cache).  Inverse of the
 * mount-time mark-dirty in mount_fs().  Called from the fs_unmount RPC
 * at shutdown, after the final data sync.  Operates on the per-device
 * cached superblock so no open file handle is required.
 */
int
ext2fs_mark_clean_dev(struct device *dev)
{
	struct ext2_mount *m = ext2_get_mount(dev);
	struct ext2fs_file tmp;

	if (!m || !m->m_fs)
		return FS_INVALID_FS;

	m->m_fs->s_state |= EXT2_VALID_FS;

	/* write_super() only touches f_dev + f_fs, so a stack file with
	 * just those two fields populated is enough to flush the sb. */
	memset(&tmp, 0, sizeof(tmp));
	tmp.f_dev = *dev;
	tmp.f_fs  = m->m_fs;
	return write_super(&tmp);
}

/*
 * Get or allocate an indirect block, then store 'value' at 'idx'.
 * *ind_blk_p is the indirect block number (0 = must allocate).
 * On success, *ind_blk_p is updated (if allocated), and the indirect
 * block is written back to disk.  fp->f_ic->i_blocks is updated for
 * newly allocated indirect blocks.
 */
static int
indirect_set(struct ext2fs_file *fp, daddr_t *ind_blk_p,
	     int idx, daddr_t value, int block_size)
{
	vm_offset_t ind_buf;
	vm_size_t ind_size;
	int rc;

	if (*ind_blk_p == 0) {
		/* Allocate the indirect block */
		*ind_blk_p = block_alloc(fp, 0);
		if (*ind_blk_p == 0)
			return KERN_RESOURCE_SHORTAGE;
		fp->f_ic->i_blocks += block_size / DEV_BSIZE;

		if (vm_allocate(mach_task_self(), &ind_buf,
				block_size, TRUE) != KERN_SUCCESS)
			return KERN_RESOURCE_SHORTAGE;
		memset((void *)ind_buf, 0, block_size);
		ind_size = block_size;
	} else {
		rc = read_disk_block(fp, *ind_blk_p, &ind_buf, &ind_size);
		if (rc != 0)
			return rc;
	}

	((daddr_t *)ind_buf)[idx] = cpu_to_le32(value);
	rc = write_disk_block(fp, *ind_blk_p, ind_buf, block_size);
	vm_deallocate(mach_task_self(), ind_buf, ind_size);
	return rc;
}

/*
 * Invalidate cached indirect block at the given level if it matches blk.
 */
static void
invalidate_ind_cache(struct ext2fs_file *fp, int level, daddr_t blk)
{
	if (level < NIADDR && fp->f_blkno[level] == blk) {
		fp->f_blkno[level] = -1;
		if (fp->f_blk[level]) {
			vm_deallocate(mach_task_self(),
				      fp->f_blk[level],
				      fp->f_blksize[level]);
			fp->f_blk[level] = 0;
			fp->f_blksize[level] = 0;
		}
	}
}

/* A whole data block: into the cache, or to the disk when there is none. */
static int
write_file_block(struct ext2fs_file *fp, daddr_t disk_block, vm_offset_t data)
{
	vm_size_t size = EXT2_BLOCK_SIZE(fp->f_fs);

	if (fp->f_dev.cache)
		return page_cache_write(fp->f_dev.cache, disk_block, data, size);
	return write_disk_block(fp, disk_block, data, size);
}

/*
 * #599: part of a block this write allocated.  It starts as zeros: its last
 * owner's bytes may still be on the disk or in the cache, and the
 * read-modify-write this used to be handed them to the new file.
 */
static int
write_fresh_part(struct ext2fs_file *fp, daddr_t disk_block, int off,
		 vm_offset_t data, vm_size_t chunk)
{
	vm_size_t block_size = EXT2_BLOCK_SIZE(fp->f_fs);
	vm_offset_t blkbuf;
	int rc;

	if (vm_allocate(mach_task_self(), &blkbuf, block_size, TRUE) !=
	    KERN_SUCCESS)
		return KERN_RESOURCE_SHORTAGE;
	memset((void *)blkbuf, 0, block_size);
	memcpy((void *)(blkbuf + off), (void *)data, chunk);
	rc = write_file_block(fp, disk_block, blkbuf);
	(void) vm_deallocate(mach_task_self(), blkbuf, block_size);
	return rc;
}

/*
 * #599: part of a block the file already had.  The rest of the block is what
 * it holds, so it is read first, or the write fails: a read that failed used
 * to be taken for a new block and zero-filled, and the write replaced the
 * block's other bytes with zeros and succeeded.  Through the cache the block
 * comes back pinned and is modified in its slot; with no cache, or no slot to
 * give, it is modified privately and written whole.
 *
 * No readahead here: the caller holds the vnode lock, which readahead's
 * block_map takes.
 */
static int
write_old_part(struct ext2fs_file *fp, daddr_t disk_block, int off,
	       vm_offset_t data, vm_size_t chunk)
{
	vm_offset_t blkbuf;
	vm_size_t blkbuf_size;
	int rc;

	if (fp->f_dev.cache) {
		struct page_cache_entry *e = NULL;

		rc = page_cache_get(fp->f_dev.cache, disk_block, ext2_fill,
				    fp, &e);
		if (rc != 0)
			return rc;
		if (e != NULL) {
			rc = page_cache_modify(fp->f_dev.cache, e,
					       (vm_size_t)off, chunk, data);
			page_cache_put(fp->f_dev.cache, e);
			return rc;
		}
	}
	rc = read_disk_block(fp, disk_block, &blkbuf, &blkbuf_size);
	if (rc != 0)
		return rc;
	memcpy((void *)(blkbuf + off), (void *)data, chunk);
	rc = write_file_block(fp, disk_block, blkbuf);
	(void) vm_deallocate(mach_task_self(), blkbuf, blkbuf_size);
	return rc;
}

static int link_fresh_block(struct ext2fs_file *, daddr_t, daddr_t);

/*
 * Write data to a file at the given offset.
 * Allocates new blocks as needed, extends file size.
 */
static int
write_file_locked(
	struct ext2fs_file	*fp,
	vm_offset_t		offset,
	vm_offset_t		data,
	vm_size_t		size)
{
	struct ext2_super_block *fs = fp->f_fs;
	int block_size = EXT2_BLOCK_SIZE(fs);
	int rc = 0, linked = 0;
	vm_offset_t start = offset;

	while (size > 0 && rc == 0) {
		daddr_t file_block = ext2_lblkno(fs, offset);
		int off = ext2_blkoff(fs, offset);
		vm_size_t chunk = block_size - off;
		daddr_t disk_block;

		if (chunk > size)
			chunk = size;

		/* Resolve file block → disk block */
		rc = block_map_locked(fp, file_block, &disk_block);
		if (rc != 0)
			break;

		if (disk_block == 0) {
			disk_block = block_alloc(fp, 0);
			if (disk_block == 0) {
				rc = KERN_RESOURCE_SHORTAGE;
				break;
			}

			/*
			 * #599: the block's bytes BEFORE the block is in the
			 * map.  Linked first, a failed write left the file
			 * mapping a block that still held its last owner's
			 * bytes, where it read zeros before (found in
			 * review).  Written first, a failed write frees the
			 * block and the map never saw it.
			 */
			if (off == 0 && chunk == (vm_size_t)block_size)
				rc = write_file_block(fp, disk_block, data);
			else
				rc = write_fresh_part(fp, disk_block, off,
						      data, chunk);
			if (rc != 0) {
				block_free(fp, disk_block);
				break;
			}
			rc = link_fresh_block(fp, file_block, disk_block);
			/*
			 * The in-core map may have changed either way (a fresh
			 * double- or triple-indirect block is linked as it is
			 * allocated), so the inode is dirty either way.  A
			 * failed link leaves the data block reachable from
			 * nothing on the disk -- the write that joins it to the
			 * reachable map comes last in link_fresh_block, with
			 * nothing after it to fail -- so it is freed (#599; kept, as the second
			 * review round had it, it was counted by no i_blocks).
			 */
			linked = 1;
			if (rc != 0) {
				block_free(fp, disk_block);
				break;
			}
		} else {
			/* #599: every answer is the device's or the cache's */
			if (off == 0 && chunk == (vm_size_t)block_size)
				rc = write_file_block(fp, disk_block, data);
			else
				rc = write_old_part(fp, disk_block, off, data,
						    chunk);
			if (rc != 0)
				break;
		}

		offset += chunk;
		data += chunk;
		size -= chunk;
	}

	/*
	 * #599: what this call linked is in the in-core map whether or not a
	 * later chunk failed, so the inode is dirty and the cached copy stale
	 * either way; the size moves only over what was written.
	 */
	if (offset > start && offset > fp->f_ic->i_size) {
		fp->f_ic->i_size = offset;	/* only over what was written */
		linked = 1;	/* the inode changed: dirty either way */
	}
	if (rc == 0 || linked) {
		fp->f_vnode->v_inode_dirty = 1;
		{
		struct ext2_mount *m = (struct ext2_mount *)fp->f_dev.mount_data;
		if (m)
			icache_invalidate(m, fp->f_ino);
		}
	}
	return rc;
}

/*
 * #599: a block for the double- or triple-indirect level, zeroed on the disk
 * before anything reads it as pointers.  block_alloc gives a block with its
 * last owner's bytes, and link_fresh_block read those as block numbers --
 * of other files, or out of range -- and wrote through them (found in review;
 * a fresh image, all zeros, hides it).  indirect_set zero-fills the single
 * level itself.  0 when there is no block, or it could not be zeroed.
 */
static daddr_t
fresh_indirect_block(struct ext2fs_file *fp)
{
	vm_size_t	size = EXT2_BLOCK_SIZE(fp->f_fs);
	vm_offset_t	zeros;
	daddr_t		b;

	b = block_alloc(fp, 0);
	if (b == 0)
		return 0;
	if (vm_allocate(mach_task_self(), &zeros, size, TRUE) != KERN_SUCCESS) {
		block_free(fp, b);
		return 0;
	}
	if (write_disk_block(fp, b, zeros, size) != 0) {
		(void) vm_deallocate(mach_task_self(), zeros, size);
		block_free(fp, b);
		return 0;
	}
	(void) vm_deallocate(mach_task_self(), zeros, size);
	return b;
}

/*
 * #599: put a block whose bytes are already written -- to the page cache,
 * or to the disk where there is none -- into the file's map at `file_block',
 * directly or through the indirect blocks, allocating those as it goes.  The
 * order holds in the in-core map; on the disk an indirect block written here
 * can name a data block whose bytes are still in the cache, until the next
 * sync.  The write that makes the data block reachable from the disk's map
 * comes last, with nothing after it that can fail -- the entry naming it in
 * an existing single-indirect block, or the parent entry that joins a new
 * chain holding it (a parent is updated only when its child was just
 * allocated) -- so a failure leaves the data block reachable from nothing on
 * the disk and the caller frees it; an indirect block allocated
 * before the failure is left allocated, as it always was.
 */
static int
link_fresh_block(struct ext2fs_file *fp, daddr_t file_block, daddr_t disk_block)
{
	struct ext2_super_block *fs = fp->f_fs;
	int block_size = EXT2_BLOCK_SIZE(fs);
	int nindir = NINDIR(fs);
	int rc;

	if (file_block < NDADDR) {
		/* Direct block */
		fp->f_ic->i_block[file_block] = disk_block;

	} else if (file_block < NDADDR + nindir) {
		/* Single indirect */
		int idx = file_block - NDADDR;
		daddr_t ind = fp->f_ic->i_block[EXT2_IND_BLOCK];

		rc = indirect_set(fp, &ind, idx,
				  disk_block, block_size);
		if (rc != 0) return rc;
		fp->f_ic->i_block[EXT2_IND_BLOCK] = ind;
		invalidate_ind_cache(fp, 0, ind);

	} else if (file_block < NDADDR + nindir +
		   nindir * nindir) {
		/* Double indirect */
		int rem = file_block - NDADDR - nindir;
		int idx1 = rem / nindir;
		int idx2 = rem % nindir;
		daddr_t dind =
			fp->f_ic->i_block[EXT2_DIND_BLOCK];
		daddr_t sind, sind_was;
		vm_offset_t dind_buf;
		vm_size_t dind_size;

		/* Get/alloc double-indirect block */
		if (dind == 0) {
			dind = fresh_indirect_block(fp);
			if (dind == 0)
				return KERN_RESOURCE_SHORTAGE;
			fp->f_ic->i_block[EXT2_DIND_BLOCK] =
				dind;
			fp->f_ic->i_blocks +=
				block_size / DEV_BSIZE;
		}

		/* Read it to find single-indirect pointer */
		rc = read_disk_block(fp, dind,
				     &dind_buf, &dind_size);
		if (rc != 0) return rc;
		sind = le32_to_cpu(
			((daddr_t *)dind_buf)[idx1]);
		vm_deallocate(mach_task_self(),
			      dind_buf, dind_size);
		sind_was = sind;

		/* Set data block in single-indirect */
		rc = indirect_set(fp, &sind, idx2,
				  disk_block, block_size);
		if (rc != 0) return rc;

		/*
		 * Update the double-indirect entry only if sind was just
		 * allocated.  #599: written also when it named sind already,
		 * a failure of that redundant write came after the disk
		 * already reached the data block (found in review); now the
		 * data block is reachable from the disk only once nothing
		 * is left to fail, and a failure frees it safely.
		 */
		if (sind_was == 0) {
			rc = indirect_set(fp, &dind, idx1,
					  sind, block_size);
			if (rc != 0) return rc;
		}
		fp->f_ic->i_block[EXT2_DIND_BLOCK] = dind;
		invalidate_ind_cache(fp, 0, sind);
		invalidate_ind_cache(fp, 1, dind);

	} else {
		/* Triple indirect */
		long rem = file_block - NDADDR - nindir -
			   (long)nindir * nindir;
		int idx1 = rem / ((long)nindir * nindir);
		int idx2 = (rem / nindir) % nindir;
		int idx3 = rem % nindir;
		daddr_t tind =
			fp->f_ic->i_block[EXT2_TIND_BLOCK];
		daddr_t dind, sind, dind_was, sind_was;
		vm_offset_t tbuf, dbuf;
		vm_size_t tsize, dsize;

		/* Get/alloc triple-indirect block */
		if (tind == 0) {
			tind = fresh_indirect_block(fp);
			if (tind == 0)
				return KERN_RESOURCE_SHORTAGE;
			fp->f_ic->i_block[EXT2_TIND_BLOCK] =
				tind;
			fp->f_ic->i_blocks +=
				block_size / DEV_BSIZE;
		}

		/* Read triple to find double pointer */
		rc = read_disk_block(fp, tind,
				     &tbuf, &tsize);
		if (rc != 0) return rc;
		dind = le32_to_cpu(
			((daddr_t *)tbuf)[idx1]);
		vm_deallocate(mach_task_self(),
			      tbuf, tsize);
		dind_was = dind;

		/* Get/alloc double-indirect */
		if (dind == 0) {
			dind = fresh_indirect_block(fp);
			if (dind == 0)
				return KERN_RESOURCE_SHORTAGE;
			fp->f_ic->i_blocks +=
				block_size / DEV_BSIZE;
		}

		/* Read double to find single pointer */
		rc = read_disk_block(fp, dind,
				     &dbuf, &dsize);
		if (rc != 0) return rc;
		sind = le32_to_cpu(
			((daddr_t *)dbuf)[idx2]);
		vm_deallocate(mach_task_self(),
			      dbuf, dsize);
		sind_was = sind;

		/* Set data block in single-indirect */
		rc = indirect_set(fp, &sind, idx3,
				  disk_block, block_size);
		if (rc != 0) return rc;

		/* Update double → single, triple → double: only where the
		 * child was just allocated (#599, as the double path) */
		if (sind_was == 0) {
			rc = indirect_set(fp, &dind, idx2,
					  sind, block_size);
			if (rc != 0) return rc;
		}
		if (dind_was == 0) {
			rc = indirect_set(fp, &tind, idx1,
					  dind, block_size);
			if (rc != 0) return rc;
		}
		fp->f_ic->i_block[EXT2_TIND_BLOCK] = tind;
		invalidate_ind_cache(fp, 0, sind);
		invalidate_ind_cache(fp, 1, dind);
		invalidate_ind_cache(fp, 2, tind);
	}
	fp->f_ic->i_blocks += block_size / DEV_BSIZE;

	/* #384: the shared block map changed — invalidate
	 * every other opener's private indirect caches. */
	vnode_gen_bump(fp);
	return 0;
}

/*
 * #384: public write — the whole call runs under the vnode lock, so
 * concurrent writers to the same file (each mutating the shared block
 * map through allocation) are fully serialized, and no reader walks a
 * half-updated map.
 */
int
ext2fs_write_file(
	fs_private_t		private,
	vm_offset_t		offset,
	vm_offset_t		data,
	vm_size_t		size)
{
	struct ext2fs_file *fp = (struct ext2fs_file *)private;
	int rc;

	vnode_mutex_lock(fp);
	vnode_gen_check(fp);
	/*
	 * #599: a directory is written by its own operations, which keep its
	 * records well-formed and its flags theirs.  A write through a handle
	 * put raw bytes in its blocks, and put its vnode on the writeback
	 * thread's dirty list, where a flush of its flags ran beside the
	 * namespace operations that set them (found in review).
	 */
	if ((fp->f_ic->i_mode & IFMT) == IFDIR)
		rc = FS_IS_DIRECTORY;
	else
		rc = write_file_locked(fp, offset, data, size);
	vnode_mutex_unlock(fp);
	return rc;
}

/*
 * Flush dirty metadata to disk.  When 2+ items are dirty, batch
 * them into a single device_write_batch IPC to save round-trips.
 * #384: body; runs under the vnode lock (see wrapper below) so the
 * inode snapshot it serializes is consistent with concurrent writers.
 */
/* #599: what a failed flush did not write, dirty again for the next one */
static void
flush_reraise(struct ext2_vnode *vn, int inode, int gd, int super)
{
	if (inode)
		vn->v_inode_dirty = 1;
	if (gd)
		vn->v_gd_dirty = 1;
	if (super)
		vn->v_super_dirty = 1;
}

static int flush_metadata_taken(struct ext2fs_file *, int, int, int, int);

static int
flush_metadata_locked(struct ext2fs_file *fp)
{
	struct ext2_vnode *vn = fp->f_vnode;
	int n_dirty = 0;
	int w_inode, w_gd, w_super;

	if (!vn)
		return 0;

	/*
	 * #599: the three flags taken once, and cleared as they are taken.
	 * Read twice -- once to count the records and once to fill them --
	 * and cleared all three at the end, a flag another operation raised
	 * in between put the wrong buffers under the records, and was cleared
	 * with nothing written for it (found in review).  What this flush
	 * does not write is raised again below.
	 */
	w_inode = vn->v_inode_dirty;
	w_gd = vn->v_gd_dirty;
	w_super = vn->v_super_dirty;
	vn->v_inode_dirty = vn->v_gd_dirty = vn->v_super_dirty = 0;

	if (w_inode) n_dirty++;
	if (w_gd) n_dirty++;
	if (w_super) n_dirty++;

	if (n_dirty == 0)
		return 0;

	return flush_metadata_taken(fp, w_inode, w_gd, w_super, n_dirty);
}

/* #599: the flush proper, over the flags flush_metadata_locked took */
static int
flush_metadata_taken(struct ext2fs_file *fp, int w_inode, int w_gd,
		     int w_super, int n_dirty)
{
	struct ext2_super_block *fs = fp->f_fs;
	struct ext2_vnode *vn = fp->f_vnode;
	int rc;

	/* Single dirty item or no batch stub: unbatched path */
	if (n_dirty == 1 || !ext2_dev_has_batch(&fp->f_dev)) {
		if (w_inode) {
			rc = write_inode(vn->v_ino, fp);
			icache_follow(fp, rc);		/* #599 */
			if (rc != 0) {
				printf("ext2: flush: inode %lu not written "
				       "(rc=%d)\n",
				       (unsigned long) vn->v_ino, rc);
				flush_reraise(vn, w_inode, w_gd, w_super);
				return rc;
			}
		}
		if (w_gd) {
			rc = write_gd(fp);
			if (rc != 0) {
				printf("ext2: flush: group descriptors not "
				       "written (rc=%d)\n", rc);
				flush_reraise(vn, 0, w_gd, w_super);
				return rc;
			}
		}
		if (w_super) {
			rc = write_super(fp);
			if (rc != 0) {
				printf("ext2: flush: superblock not written "
				       "(rc=%d)\n", rc);
				flush_reraise(vn, 0, 0, w_super);
				return rc;
			}
		}
		return 0;
	}

	/*
	 * Multiple dirty items — batch the writes into one IPC.  #599: the
	 * inode block is read afresh first (vnode_inode_block), under
	 * ext2_itable_lock and v_lock, and the lock is held until the batch
	 * is written.
	 */
	{
		recnum_t recnums[3];
		unsigned int sizes[3];
		unsigned int n = 0;
		unsigned int total_size = 0;
		vm_offset_t concat;
		unsigned int off;
		io_buf_len_t bytes_written;
		unsigned int inode_blk_size = EXT2_BLOCK_SIZE(fs);
		int itable = 0;		/* #599: ext2_itable_lock held */

		/* --- Prepare inode block (serialize in-place) --- */
		if (w_inode) {
			daddr_t inode_disk_block = ext2_ino2blk(fs,
						fp->f_gd, vn->v_ino);

			/* #599: held until the batch is written */
			pthread_mutex_lock(&ext2_itable_lock);
			itable = 1;
			rc = vnode_inode_block(fp, vn->v_ino,
					       inode_disk_block);	/* #599 */
			if (rc != 0) {
				pthread_mutex_unlock(&ext2_itable_lock);
				flush_reraise(vn, w_inode, w_gd, w_super);
				return rc;
			}
			serialize_inode(fp);
			recnums[n] = (recnum_t)dbtorec(&fp->f_dev,
				ext2_fsbtodb(fs, inode_disk_block));
			sizes[n] = inode_blk_size;
			total_size += inode_blk_size;
			n++;
		}

		/* --- Prepare group descriptors --- */
		if (w_gd) {
			int gd_loc = fs->s_first_data_block + 1;
			int gd_sec = (gd_loc * EXT2_BLOCK_SIZE(fs))
				     / DEV_BSIZE;
			recnums[n] = (recnum_t)dbtorec(&fp->f_dev, gd_sec);
			sizes[n] = fp->f_gd_size;
			total_size += fp->f_gd_size;
			n++;
		}

		/* --- Prepare superblock (little-endian raw copy) --- */
		struct ext2_super_block raw_sb;
		if (w_super) {
			/* Full copy first so untouched fields (uuid,
			 * reserved_gdt_blocks, ...) survive — see write_super
			 * (#266). */
			raw_sb = *fs;
			raw_sb.s_inodes_count =
				cpu_to_le32(fs->s_inodes_count);
			raw_sb.s_blocks_count =
				cpu_to_le32(fs->s_blocks_count);
			raw_sb.s_r_blocks_count =
				cpu_to_le32(fs->s_r_blocks_count);
			raw_sb.s_free_blocks_count =
				cpu_to_le32(fs->s_free_blocks_count);
			raw_sb.s_free_inodes_count =
				cpu_to_le32(fs->s_free_inodes_count);
			raw_sb.s_first_data_block =
				cpu_to_le32(fs->s_first_data_block);
			raw_sb.s_log_block_size =
				cpu_to_le32(fs->s_log_block_size);
			raw_sb.s_log_frag_size =
				cpu_to_le32(fs->s_log_frag_size);
			raw_sb.s_blocks_per_group =
				cpu_to_le32(fs->s_blocks_per_group);
			raw_sb.s_frags_per_group =
				cpu_to_le32(fs->s_frags_per_group);
			raw_sb.s_inodes_per_group =
				cpu_to_le32(fs->s_inodes_per_group);
			raw_sb.s_mtime =
				cpu_to_le32(fs->s_mtime);
			raw_sb.s_wtime =
				cpu_to_le32(fs->s_wtime);
			raw_sb.s_mnt_count =
				cpu_to_le16(fs->s_mnt_count);
			raw_sb.s_max_mnt_count =
				cpu_to_le16(fs->s_max_mnt_count);
			raw_sb.s_magic =
				cpu_to_le16(fs->s_magic);
			raw_sb.s_state =
				cpu_to_le16(fs->s_state);
			raw_sb.s_errors =
				cpu_to_le16(fs->s_errors);
			raw_sb.s_minor_rev_level =
				cpu_to_le16(fs->s_minor_rev_level);
			raw_sb.s_lastcheck =
				cpu_to_le32(fs->s_lastcheck);
			raw_sb.s_checkinterval =
				cpu_to_le32(fs->s_checkinterval);
			raw_sb.s_creator_os =
				cpu_to_le32(fs->s_creator_os);
			raw_sb.s_rev_level =
				cpu_to_le32(fs->s_rev_level);
			raw_sb.s_def_resuid =
				cpu_to_le16(fs->s_def_resuid);
			raw_sb.s_def_resgid =
				cpu_to_le16(fs->s_def_resgid);
			if (fs->s_rev_level >= EXT2_DYNAMIC_REV) {
				raw_sb.s_first_ino =
					cpu_to_le32(fs->s_first_ino);
				raw_sb.s_inode_size =
					cpu_to_le16(fs->s_inode_size);
				raw_sb.s_block_group_nr =
					cpu_to_le16(fs->s_block_group_nr);
				raw_sb.s_feature_compat =
					cpu_to_le32(fs->s_feature_compat);
				raw_sb.s_feature_incompat =
					cpu_to_le32(fs->s_feature_incompat);
				raw_sb.s_feature_ro_compat =
					cpu_to_le32(fs->s_feature_ro_compat);
			}

			recnums[n] = (recnum_t)dbtorec(&fp->f_dev, SBLOCK);
			sizes[n] = SBSIZE;
			total_size += SBSIZE;
			n++;
		}

		/* --- Send batched write --- */
		if (fp->f_dev.blk) {
			/* libblk path: pass separate buffers */
			io_buf_ptr_t data_bufs[3];
			mach_msg_type_number_t data_sizes[3];
			unsigned int bi = 0;

			if (w_inode) {
				data_bufs[bi] = (io_buf_ptr_t)vn->v_inode_blk;
				data_sizes[bi] = inode_blk_size;
				bi++;
			}
			if (w_gd) {
				data_bufs[bi] = (io_buf_ptr_t)fp->f_gd;
				data_sizes[bi] = fp->f_gd_size;
				bi++;
			}
			if (w_super) {
				data_bufs[bi] = (io_buf_ptr_t)&raw_sb;
				data_sizes[bi] = SBSIZE;
				bi++;
			}

			rc = blk_write_batch(fp->f_dev.blk,
					     recnums, data_bufs,
					     data_sizes, n,
					     &bytes_written);
		} else {
			/* Direct device_write_batch path:
			 * concatenate into a single OOL buffer */
			rc = vm_allocate(mach_task_self(), &concat,
					 total_size, TRUE);
			if (rc != KERN_SUCCESS) {
				if (itable)
					pthread_mutex_unlock(
						&ext2_itable_lock);
				flush_reraise(vn, w_inode, w_gd, w_super);
				return rc;
			}

			off = 0;
			if (w_inode) {
				memcpy((void *)(concat + off),
				       (void *)vn->v_inode_blk,
				       inode_blk_size);
				off += inode_blk_size;
			}
			if (w_gd) {
				memcpy((void *)(concat + off),
				       (void *)fp->f_gd, fp->f_gd_size);
				off += fp->f_gd_size;
			}
			if (w_super) {
				memcpy((void *)(concat + off),
				       (void *)&raw_sb, SBSIZE);
				off += SBSIZE;
			}

			rc = device_write_batch(fp->f_dev.dev_port, 0,
						recnums, n, sizes, n,
						(io_buf_ptr_t)concat,
						total_size,
						&bytes_written);

			vm_deallocate(mach_task_self(), concat, total_size);
		}

		if (itable) {
			pthread_mutex_unlock(&ext2_itable_lock);
			icache_follow(fp, rc);		/* #599 */
		}
		if (rc != KERN_SUCCESS) {
			flush_reraise(vn, w_inode, w_gd, w_super);
			/*
			 * 🔴 #483: it used to return here saying nothing.  A
			 * caller learned that "a sync failed" and could not
			 * learn what had not reached the disk -- and this
			 * branch has never once succeeded, in every captured
			 * log back to 2026-07-21.
			 */
			printf("ext2: flush: batch of %u block(s) for inode %lu "
			       "not written via %s (rc=%d)%s%s%s\n",
			       n, (unsigned long) vn->v_ino,
			       fp->f_dev.blk ? "libblk" : "device_write_batch",
			       rc,
			       w_inode ? " [inode]" : "",
			       w_gd ? " [group desc]" : "",
			       w_super ? " [superblock]" : "");
		}
		return rc;
	}
}

int
ext2fs_flush_metadata(fs_private_t private)
{
	struct ext2fs_file *fp = (struct ext2fs_file *)private;
	int rc;

	vnode_mutex_lock(fp);
	rc = flush_metadata_locked(fp);
	vnode_mutex_unlock(fp);
	return rc;
}

int
ext2fs_sync(fs_private_t private)
{
	struct ext2fs_file *fp = (struct ext2fs_file *)private;
	int rc;

	rc = ext2fs_flush_metadata(private);
	if (rc != 0)
		return rc;

	if (fp->f_dev.cache)
		return page_cache_sync(fp->f_dev.cache);
	return 0;
}

/* ================================================================
 * Writable namespace (#264): create / unlink / truncate / rename.
 *
 * These operate on the PARENT directory: the path is split into a
 * parent directory and a leaf name, the parent is opened via
 * ext2fs_open_file_into() (sharing its vnode like any other opener),
 * the directory-entry and allocation helpers above do the work, and
 * ext2fs_close_file() flushes the parent's dirty inode / group
 * descriptors / superblock.  Freed blocks are accounted through an fp
 * that owns a vnode (the parent) so the dirty flags are raised.
 * ================================================================ */

/*
 * Split an absolute path into its parent directory and leaf name.
 * parent_out must hold at least PATH_MAX+1 bytes.  "/a/b/c" -> "/a/b"
 * + "c"; "/x" -> "/" + "x".  *leaf_out points inside parent_out.
 */
static void
split_parent_leaf(const char *path, char *parent_out, const char **leaf_out)
{
	char *slash;

	strncpy(parent_out, path, PATH_MAX);
	parent_out[PATH_MAX] = '\0';

	slash = strrchr(parent_out, '/');
	if (slash == NULL) {
		/* Relative leaf, no directory part — treat parent as root. */
		*leaf_out = path;
		strcpy(parent_out, "/");
		return;
	}
	*leaf_out = path + (slash - parent_out) + 1;
	if (slash == parent_out)
		parent_out[1] = '\0';	/* parent is root "/" */
	else
		*slash = '\0';
}

/*
 * Write a fresh on-disk inode at 'ino'.  Only this inode's slot inside
 * the shared inode-table block is touched.  iblock (EXT2_N_BLOCKS long,
 * may be NULL) seeds the block map; the inode cache is invalidated so a
 * later read_inode() picks up the new contents.
 */
static int
write_new_inode(struct ext2fs_file *ctx, ino_t ino, int mode,
		const unsigned int *iblock, unsigned long size,
		unsigned long blocks, int links)
{
	struct ext2_super_block *fs = ctx->f_fs;
	daddr_t itblk = ext2_ino2blk(fs, ctx->f_gd, ino);
	vm_offset_t buf;
	vm_size_t bsz;
	struct ext2_inode *raw;
	int rc, k;

	pthread_mutex_lock(&ext2_itable_lock);		/* #599 */
	rc = read_disk_block(ctx, itblk, &buf, &bsz);
	if (rc != 0) {
		pthread_mutex_unlock(&ext2_itable_lock);
		return rc;
	}

	raw = (struct ext2_inode *)((char *)buf +
		ext2_itoo(fs, ino) * EXT2_INODE_SIZE(fs));
	memset(raw, 0, EXT2_INODE_SIZE(fs));
	raw->i_mode = cpu_to_le16(mode);
	raw->i_links_count = cpu_to_le16(links);
	raw->i_size = cpu_to_le32(size);
	raw->i_blocks = cpu_to_le32(blocks);
	if (iblock)
		for (k = 0; k < EXT2_N_BLOCKS; k++)
			raw->i_block[k] = cpu_to_le32(iblock[k]);

	rc = write_disk_block(ctx, itblk, buf, EXT2_BLOCK_SIZE(fs));
	pthread_mutex_unlock(&ext2_itable_lock);
	vm_deallocate(mach_task_self(), buf, bsz);

	{
		struct ext2_mount *m =
			(struct ext2_mount *)ctx->f_dev.mount_data;
		if (m)
			icache_invalidate(m, ino);
	}
	return rc;
}

/*
 * Recursively free an indirect block tree.  level 0 = single indirect
 * (entries are data blocks), 1 = double, 2 = triple.  Frees every block
 * it points at and then the indirect block itself.  Returns the number
 * of freed blocks through *freed (for i_blocks accounting).
 */
static void
free_indirect_tree(struct ext2fs_file *acct, daddr_t ind_block, int level,
		   int *freed)
{
	struct ext2_super_block *fs = acct->f_fs;
	int per = EXT2_ADDR_PER_BLOCK(fs);
	vm_offset_t buf;
	vm_size_t bsz;
	daddr_t *ptr;
	int i;

	if (ind_block == 0)
		return;
	if (read_disk_block(acct, ind_block, &buf, &bsz) != 0)
		return;

	/*
	 * 🔥 daddr_t and not `unsigned long' (#415).  The entries on the disk
	 * are four bytes each; an `unsigned long *' strides eight of them on
	 * x86-64, so this walked one entry in two -- and le32_to_cpu() then
	 * took the low half of a pair, which on a little-endian machine is the
	 * first of the two, so it looked like it was working.  Half the blocks
	 * of every indirectly-addressed file would have been left allocated.
	 */
	ptr = (daddr_t *)buf;
	for (i = 0; i < per; i++) {
		daddr_t blk = le32_to_cpu(ptr[i]);
		if (blk == 0)
			continue;
		if (level > 0)
			free_indirect_tree(acct, blk, level - 1, freed);
		else {
			block_free(acct, blk);
			(*freed)++;
		}
	}
	vm_deallocate(mach_task_self(), buf, bsz);

	block_free(acct, ind_block);
	(*freed)++;
}

/*
 * Free the data blocks of an inode whose block map is 'iblock', from
 * logical block 'from' to the end.  Partial truncation that would have
 * to descend into an indirect tree (from > NDADDR) is not supported and
 * returns an error rather than risk leaking blocks.  'acct' must own a
 * vnode so block_free() can raise the gd/superblock dirty flags.  The
 * number of freed disk blocks is returned through *freed_sectors as a
 * count of fs blocks (caller scales to 512-byte units for i_blocks).
 */
static int
free_file_blocks(struct ext2fs_file *acct, unsigned int *iblock,
		 daddr_t from, int *freed_blocks)
{
	int i;

	*freed_blocks = 0;

	if (from > NDADDR)
		return KERN_FAILURE;	/* partial indirect truncate unsupported */

	/* Direct blocks at or beyond 'from'. */
	for (i = (int)from; i < NDADDR; i++) {
		if (iblock[i]) {
			block_free(acct, (daddr_t)iblock[i]);
			(*freed_blocks)++;
			iblock[i] = 0;
		}
	}

	/* Everything from 'from' onward includes the whole indirect set. */
	free_indirect_tree(acct, (daddr_t)iblock[NDADDR],     0, freed_blocks);
	free_indirect_tree(acct, (daddr_t)iblock[NDADDR + 1], 1, freed_blocks);
	free_indirect_tree(acct, (daddr_t)iblock[NDADDR + 2], 2, freed_blocks);
	iblock[NDADDR]     = 0;
	iblock[NDADDR + 1] = 0;
	iblock[NDADDR + 2] = 0;
	return 0;
}

/*
 * Open the parent directory of 'path' into the caller-provided fp.
 * Returns 0 with *leaf pointing at the leaf name inside leafbuf
 * (PATH_MAX+1), or an FS_* error.  The caller must ext2fs_close_file(fp)
 * on success.
 */
static int
open_parent_dir(struct device *dev, const char *path, char *leafbuf,
		const char **leaf, struct ext2fs_file *fp)
{
	fs_private_t pp;
	int rc;

	split_parent_leaf(path, leafbuf, leaf);
	if ((*leaf)[0] == '\0')
		return FS_INVALID_PARAMETER;

	rc = ext2fs_open_file_into(dev, leafbuf, &pp, fp);
	if (rc != 0)
		return rc;
	if ((fp->f_ic->i_mode & IFMT) != IFDIR) {
		ext2fs_close_file(pp);
		return FS_NOT_DIRECTORY;
	}
	return 0;
}

int
ext2fs_create(struct device *dev, const char *path, int mode)
{
	struct ext2fs_file parent;
	char leafbuf[PATH_MAX + 1];
	const char *leaf;
	ino_t ino, existing;
	int rc, goal;

	rc = open_parent_dir(dev, path, leafbuf, &leaf, &parent);
	if (rc != 0)
		return rc;

	/*
	 * #599: only FS_NO_ENTRY means the name is free.  Any other failure --
	 * a damaged directory, a read that did not land -- used to be taken for
	 * "absent", and a second entry of the same name was added.
	 */
	rc = search_directory((char *)leaf, &parent, &existing);
	if (rc != FS_NO_ENTRY) {
		ext2fs_close_file((fs_private_t)&parent);
		return rc == 0 ? FS_INVALID_PARAMETER	/* already exists */
			       : rc;
	}

	goal = (parent.f_ino - 1) / parent.f_fs->s_inodes_per_group;
	ino = inode_alloc(&parent, goal, 0);
	if (ino == 0) {
		ext2fs_close_file((fs_private_t)&parent);
		return KERN_RESOURCE_SHORTAGE;
	}

	rc = write_new_inode(&parent, ino, IFREG | (mode & 0777), NULL, 0, 0, 1);
	if (rc == 0)
		rc = dir_add_entry(&parent, leaf, ino, EXT2_FT_REG_FILE);
	if (rc != 0)
		inode_free(&parent, ino, 0);

	ext2fs_close_file((fs_private_t)&parent);
	return rc;
}

/*
 * Drop one link of `ino', whose name has just been removed from `parent'.
 * At zero links its data blocks and the inode itself are freed -- accounted
 * on the parent fp, so the group-descriptor and superblock dirty flags are
 * flushed when it is closed.  The inode is read through a scratch fp that
 * borrows the parent's fs, gd and device.
 *
 * #599: shared by unlink and by rename's overwrite, which removed the
 * destination's name and never dropped its link, so the file it replaced
 * kept its inode and its blocks for ever.  The name is already gone when
 * this runs, so a failure here cannot be undone by the caller; it is said,
 * with the inode number, instead of being ignored.
 */
static void
inode_drop_link(struct ext2fs_file *parent, ino_t ino, const char *name)
{
	struct ext2fs_file target;
	int links, freed = 0, rc;

	memset(&target, 0, sizeof(target));
	target.f_dev = parent->f_dev;
	target.f_fs  = parent->f_fs;
	target.f_gd  = parent->f_gd;
	target.f_ic  = &target.f_ic_scratch;

	rc = read_inode(ino, &target);
	if (rc != 0) {
		printf("ext2: \"%s\" was removed, and its inode %lu could not "
		       "be read (rc=%d) — its link count is left as it was\n",
		       name, (unsigned long)ino, rc);
		free_file_buffers(&target);
		return;
	}

	links = (int)target.f_ic->i_links_count - 1;
	if (links <= 0) {
		free_file_blocks(parent, target.f_ic->i_block, 0, &freed);
		(void)write_new_inode(parent, ino, 0, NULL, 0, 0, 0);
		inode_free(parent, ino, 0);
	} else {
		(void)write_new_inode(parent, ino,
			target.f_ic->i_mode, target.f_ic->i_block,
			target.f_ic->i_size, target.f_ic->i_blocks,
			links);
	}
	free_file_buffers(&target);
}

int
ext2fs_unlink(struct device *dev, const char *path)
{
	struct ext2fs_file parent;
	char leafbuf[PATH_MAX + 1];
	const char *leaf;
	ino_t ino = 0;
	int rc;

	rc = open_parent_dir(dev, path, leafbuf, &leaf, &parent);
	if (rc != 0)
		return rc;

	rc = dir_remove_entry(&parent, leaf, &ino);
	if (rc != 0) {
		ext2fs_close_file((fs_private_t)&parent);
		return rc;
	}

	inode_drop_link(&parent, ino, leaf);

	ext2fs_close_file((fs_private_t)&parent);
	return 0;
}

/*
 * Truncate an open file to 'length'.  Only truncation to zero, or to a
 * length still within the direct blocks, is supported; anything that
 * would require partial indirect-tree truncation returns an error.
 */
int
ext2fs_truncate_file(fs_private_t private, vm_size_t length)
{
	struct ext2fs_file *fp = (struct ext2fs_file *)private;
	struct ext2_super_block *fs = fp->f_fs;
	int block_size = EXT2_BLOCK_SIZE(fs);
	daddr_t from = (length + block_size - 1) / block_size;
	int freed = 0, rc;

	/*
	 * #384: truncation frees the shared block map — the single most
	 * destructive mutation.  Serialize it against every walker and
	 * writer of this inode, and bump the generation so their private
	 * indirect caches (now pointing at freed, soon re-allocated
	 * blocks) are dropped before reuse.
	 */
	vnode_mutex_lock(fp);
	vnode_gen_check(fp);

	/*
	 * #599: a directory's blocks are its records, freed only by rmdir
	 * (found in review: a truncate through a directory handle left a
	 * linked directory with no '.' and no entries).
	 */
	if ((fp->f_ic->i_mode & IFMT) == IFDIR) {
		vnode_mutex_unlock(fp);
		return FS_IS_DIRECTORY;
	}

	if (fp->f_ic->i_size <= length) {
		vnode_mutex_unlock(fp);
		return 0;	/* grow-on-truncate not handled here */
	}

	rc = free_file_blocks(fp, fp->f_ic->i_block, from, &freed);
	if (rc != 0) {
		vnode_mutex_unlock(fp);
		return rc;
	}

	fp->f_ic->i_size = length;
	if ((unsigned long)(freed * (block_size / 512)) <= fp->f_ic->i_blocks)
		fp->f_ic->i_blocks -= freed * (block_size / 512);
	else
		fp->f_ic->i_blocks = 0;
	if (fp->f_vnode)
		fp->f_vnode->v_inode_dirty = 1;
	/* Our own f_blk[] caches point into the freed tree too. */
	handle_caches_drop(fp);
	vnode_gen_bump(fp);
	vnode_mutex_unlock(fp);
	return ext2fs_flush_metadata(private);
}

int
ext2fs_rename(struct device *dev, const char *oldpath, const char *newpath)
{
	struct ext2fs_file oldp, newp;
	char oldleafbuf[PATH_MAX + 1], newleafbuf[PATH_MAX + 1];
	const char *oldleaf, *newleaf;
	ino_t ino = 0, dummy = 0, victim = 0;
	int rc, file_type;
	struct ext2fs_file tmp;

	/* Resolve the source: parent dir + the inode being moved. */
	rc = open_parent_dir(dev, oldpath, oldleafbuf, &oldleaf, &oldp);
	if (rc != 0)
		return rc;
	rc = search_directory((char *)oldleaf, &oldp, &ino);
	if (rc != 0) {			/* #599: the real error, not FS_NO_ENTRY */
		ext2fs_close_file((fs_private_t)&oldp);
		return rc;
	}

	/* Determine the entry's file_type from the inode mode. */
	memset(&tmp, 0, sizeof(tmp));
	tmp.f_dev = oldp.f_dev; tmp.f_fs = oldp.f_fs; tmp.f_gd = oldp.f_gd;
	tmp.f_ic = &tmp.f_ic_scratch;
	file_type = EXT2_FT_REG_FILE;
	if (read_inode(ino, &tmp) == 0 &&
	    (tmp.f_ic->i_mode & IFMT) == IFDIR)
		file_type = EXT2_FT_DIR;
	free_file_buffers(&tmp);

	/* Add the new name (overwriting any existing target). */
	rc = open_parent_dir(dev, newpath, newleafbuf, &newleaf, &newp);
	if (rc != 0) {
		ext2fs_close_file((fs_private_t)&oldp);
		return rc;
	}
	/*
	 * #599: the destination's answer is read, not assumed.  Any failure
	 * but FS_NO_ENTRY stops here; an existing destination is removed, and
	 * a removal that fails stops here too -- both were ignored, and a
	 * second entry of the name was added beside the first.
	 */
	rc = search_directory((char *)newleaf, &newp, &victim);
	if (rc == 0 && victim == ino) {
		/* Both names already reach this inode: nothing moves. */
		ext2fs_close_file((fs_private_t)&newp);
		ext2fs_close_file((fs_private_t)&oldp);
		return 0;
	}
	if (rc == 0) {
		/*
		 * Destination exists — remove it first (POSIX overwrite).
		 * Not a directory: its blocks and its ".." link on the parent
		 * need what rmdir does, which this does not, so that is
		 * refused rather than half done.
		 */
		memset(&tmp, 0, sizeof(tmp));
		tmp.f_dev = newp.f_dev; tmp.f_fs = newp.f_fs;
		tmp.f_gd = newp.f_gd; tmp.f_ic = &tmp.f_ic_scratch;
		rc = read_inode(victim, &tmp);
		if (rc == 0 && (tmp.f_ic->i_mode & IFMT) == IFDIR)
			rc = FS_INVALID_PARAMETER;
		free_file_buffers(&tmp);
		if (rc == 0)
			rc = dir_remove_entry(&newp, newleaf, &dummy);
		if (rc == 0)
			inode_drop_link(&newp, victim, newleaf);
	} else if (rc == FS_NO_ENTRY)
		rc = 0;
	if (rc == 0)
		rc = dir_add_entry(&newp, newleaf, ino, file_type);
	ext2fs_close_file((fs_private_t)&newp);
	if (rc != 0) {
		ext2fs_close_file((fs_private_t)&oldp);
		return rc;
	}

	/*
	 * Drop the old name.  #599: its failure is returned -- the file then
	 * has both names, which is what the caller has to be told.
	 */
	rc = dir_remove_entry(&oldp, oldleaf, &dummy);
	ext2fs_close_file((fs_private_t)&oldp);
	return rc;
}

int
ext2fs_mkdir(struct device *dev, const char *path, int mode)
{
	struct ext2fs_file parent;
	char leafbuf[PATH_MAX + 1];
	const char *leaf;
	ino_t ino, existing;
	daddr_t dblk;
	char *blk;
	struct ext2_dir_entry *dot, *dotdot;
	unsigned int iblock[EXT2_N_BLOCKS];	/* on-disk block pointers (#415) */
	int rc, goal, block_size;

	rc = open_parent_dir(dev, path, leafbuf, &leaf, &parent);
	if (rc != 0)
		return rc;

	rc = search_directory((char *)leaf, &parent, &existing); /* #599 */
	if (rc != FS_NO_ENTRY) {
		ext2fs_close_file((fs_private_t)&parent);
		return rc == 0 ? FS_INVALID_PARAMETER	/* already exists */
			       : rc;
	}

	block_size = EXT2_BLOCK_SIZE(parent.f_fs);
	goal = (parent.f_ino - 1) / parent.f_fs->s_inodes_per_group;

	ino = inode_alloc(&parent, goal, 1 /* is_dir */);
	if (ino == 0) {
		ext2fs_close_file((fs_private_t)&parent);
		return KERN_RESOURCE_SHORTAGE;
	}
	dblk = block_alloc(&parent, goal);
	if (dblk == 0) {
		inode_free(&parent, ino, 1);
		ext2fs_close_file((fs_private_t)&parent);
		return KERN_RESOURCE_SHORTAGE;
	}

	/* Build the initial directory block: "." then ".." spanning the
	 * rest of the block. */
	blk = (char *)malloc(block_size);
	if (!blk) {
		block_free(&parent, dblk);
		inode_free(&parent, ino, 1);
		ext2fs_close_file((fs_private_t)&parent);
		return KERN_RESOURCE_SHORTAGE;
	}
	memset(blk, 0, block_size);
	dot = (struct ext2_dir_entry *)blk;
	dot->inode = cpu_to_le32((unsigned long)ino);
	dot->rec_len = cpu_to_le16(EXT2_DIR_REC_LEN(1));
	dot->name_len = 1;
	dot->file_type = EXT2_FT_DIR;
	dot->name[0] = '.';
	dotdot = (struct ext2_dir_entry *)(blk + EXT2_DIR_REC_LEN(1));
	dotdot->inode = cpu_to_le32((unsigned long)parent.f_ino);
	dotdot->rec_len = cpu_to_le16(block_size - EXT2_DIR_REC_LEN(1));
	dotdot->name_len = 2;
	dotdot->file_type = EXT2_FT_DIR;
	dotdot->name[0] = '.';
	dotdot->name[1] = '.';
	rc = write_data_block(&parent, dblk, (vm_offset_t)blk, block_size);
	free(blk);
	if (rc != 0) {
		block_free(&parent, dblk);
		inode_free(&parent, ino, 1);
		ext2fs_close_file((fs_private_t)&parent);
		return rc;
	}

	/* The new directory inode: links_count 2 (itself via "." and the
	 * name in the parent), one data block. */
	memset(iblock, 0, sizeof(iblock));
	iblock[0] = (unsigned int)dblk;
	rc = write_new_inode(&parent, ino, IFDIR | (mode & 0777), iblock,
			     block_size, block_size / 512, 2);
	if (rc == 0)
		rc = dir_add_entry(&parent, leaf, ino, EXT2_FT_DIR);
	if (rc != 0) {
		block_free(&parent, dblk);
		inode_free(&parent, ino, 1);
		ext2fs_close_file((fs_private_t)&parent);
		return rc;
	}

	/* The parent gains a link from the child's "..". */
	parent.f_ic->i_links_count++;
	if (parent.f_vnode)
		parent.f_vnode->v_inode_dirty = 1;

	ext2fs_close_file((fs_private_t)&parent);
	return 0;
}

/*
 * Remove an empty directory.  Refuses non-empty directories (anything
 * beyond "." and "..").  Frees the directory's data block and inode and
 * drops the link the child contributed to the parent.
 */
int
ext2fs_rmdir(struct device *dev, const char *path)
{
	struct ext2fs_file parent, target;
	char leafbuf[PATH_MAX + 1];
	const char *leaf;
	ino_t ino = 0;
	int rc, freed = 0;
	struct fs_dirent ents[4];
	unsigned int got = 0;

	rc = open_parent_dir(dev, path, leafbuf, &leaf, &parent);
	if (rc != 0)
		return rc;

	rc = search_directory((char *)leaf, &parent, &ino);
	if (rc != 0) {			/* #599: the real error, not FS_NO_ENTRY */
		ext2fs_close_file((fs_private_t)&parent);
		return rc;
	}

	/* Open the target and verify it is an empty directory. */
	memset(&target, 0, sizeof(target));
	target.f_dev = parent.f_dev;
	target.f_fs  = parent.f_fs;
	target.f_gd  = parent.f_gd;
	target.f_ic  = &target.f_ic_scratch;
	rc = read_inode(ino, &target);
	if (rc != 0) {
		free_file_buffers(&target);
		ext2fs_close_file((fs_private_t)&parent);
		return rc;
	}
	if ((target.f_ic->i_mode & IFMT) != IFDIR) {
		free_file_buffers(&target);
		ext2fs_close_file((fs_private_t)&parent);
		return FS_NOT_DIRECTORY;
	}
	rc = ext2fs_readdir((fs_private_t)&target, ents, 4, &got);
	if (rc != 0 || got > 2) {
		free_file_buffers(&target);
		ext2fs_close_file((fs_private_t)&parent);
		return rc != 0 ? rc : FS_INVALID_PARAMETER; /* not empty */
	}

	/*
	 * Remove the name, then free the directory's blocks and inode.
	 * #599: only if the name went.  The answer was discarded and the
	 * directory freed anyway, which left a name reaching a freed inode.
	 */
	rc = dir_remove_entry(&parent, leaf, &ino);
	if (rc != 0) {
		free_file_buffers(&target);
		ext2fs_close_file((fs_private_t)&parent);
		return rc;
	}
	free_file_blocks(&parent, target.f_ic->i_block, 0, &freed);
	(void)write_new_inode(&parent, ino, 0, NULL, 0, 0, 0);
	inode_free(&parent, ino, 1);
	free_file_buffers(&target);

	/* The parent loses the link the child's ".." held. */
	if (parent.f_ic->i_links_count > 0)
		parent.f_ic->i_links_count--;
	if (parent.f_vnode)
		parent.f_vnode->v_inode_dirty = 1;

	ext2fs_close_file((fs_private_t)&parent);
	return 0;
}
