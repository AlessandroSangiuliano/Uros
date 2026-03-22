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
	unsigned int *phys_addrs, mach_msg_type_number_t phys_addrsCnt,
	io_buf_len_t *bytes_read) __attribute__((weak));

extern kern_return_t device_write_phys(
	mach_port_t device,
	dev_mode_t mode, recnum_t recnum,
	io_buf_len_t bytes_to_write,
	unsigned int *phys_addrs, mach_msg_type_number_t phys_addrsCnt,
	io_buf_len_t *bytes_written) __attribute__((weak));

#define mutex_lock(a)
#define mutex_unlock(a)
#define mutex_init(a)

security_token_t null_security_token;

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
		vm_offset_t,
		vm_offset_t *,
		vm_size_t *);

static int search_directory(
		char *,
	        struct ext2fs_file *,
		ino_t *);

static int read_fs(
		struct device *,
		struct ext2_super_block **,
		struct ext2_group_desc  **,
		vm_size_t *);

static int mount_fs(
		struct ext2fs_file *);

static void unmount_fs(
		struct ext2fs_file *);

struct fs_ops ext2fs_ops = {
	ext2fs_open_file,
	ext2fs_close_file,
	ext2fs_read_file,
	ext2fs_file_size,
	ext2fs_file_is_directory,
	ext2fs_file_is_executable
};

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

	/*
	 * Free the data block (skip if borrowed from page cache)
	 */
	if (fp->f_buf != 0) {
	    if (!fp->f_buf_borrowed)
		(void) vm_deallocate(mach_task_self(),
				     fp->f_buf,
				     fp->f_buf_size);
	    fp->f_buf = 0;
	    fp->f_buf_borrowed = 0;
	}
	fp->f_buf_blkno = -1;
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
 */
static int
read_inode(ino_t inumber, register struct ext2fs_file *fp)
{
	vm_offset_t		buf;
	mach_msg_type_number_t	buf_size;
	register
	struct ext2_super_block	*fs;
	daddr_t			disk_block;
	kern_return_t		rc;

#ifdef	DEBUG
	int	i = inumber;
	if(debug)
		printf("read_inode(%d)\n", i);
#endif 
	fs = fp->f_fs;
	fp->f_ino = inumber;
	disk_block = ext2_ino2blk(fs, fp->f_gd, inumber);

	rc = device_read(fp->f_dev.dev_port,
			 0,
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
	    inode = &fp->i_ic;

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

	/*
	 * Clear out the old buffers (this also frees the previous
	 * f_inode_blk, so we must cache the new block afterwards).
	 */
	free_file_buffers(fp);

	/*
	 * Cache the raw inode block for write-back without re-reading.
	 */
	fp->f_inode_blk = buf;
	fp->f_inode_blk_size = buf_size;

	return (0);
}

/*
 * Given an offset in a file, find the disk block number that
 * contains that block.
 */
static int
block_map(
	struct ext2fs_file	*fp,
	daddr_t			file_block,
	daddr_t			*disk_block_p)	/* out */
{
	int		level;
	int		idx;
	daddr_t		ind_block_num;
	kern_return_t	rc;

	vm_offset_t	olddata[NIADDR+1];
	vm_size_t	oldsize[NIADDR+1];

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
	 */

	mutex_lock(&fp->f_lock);

	if (file_block < NDADDR) {
	    /* Direct block. */
	    *disk_block_p = fp->i_ic.i_block[file_block];
	    mutex_unlock(&fp->f_lock);
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
	    mutex_unlock(&fp->f_lock);
	    return (FS_NOT_IN_FILE);
	}

	ind_block_num = fp->i_ic.i_block[level + NDADDR];

	/*
	 * Initialize array of blocks to free.
	 */
	for (idx = 0; idx < NIADDR; idx++)
	    oldsize[idx] = 0;

	for (; level >= 0; level--) {

	    vm_offset_t	data;
	    mach_msg_type_number_t	size;

	    if (ind_block_num == 0)
		break;

	    if (fp->f_blkno[level] == ind_block_num) {
		/*
		 *	Cache hit.  Just pick up the data.
		 */

		data = fp->f_blk[level];
	    }
	    else {
		/*
		 *	Drop our lock while doing the read.
		 *	(The f_dev and f_fs fields don`t change.)
		 */
		mutex_unlock(&fp->f_lock);

		rc = device_read(fp->f_dev.dev_port,
				 0,
				 (recnum_t) dbtorec(&fp->f_dev,
					    ext2_fsbtodb(fp->f_fs, ind_block_num)),
				 EXT2_BLOCK_SIZE(fp->f_fs),
				 (char **)&data,
				 &size);
		if (rc != KERN_SUCCESS)
		    return (rc);

		/*
		 *	See if we can cache the data.  Need a write lock to
		 *	do this.  While we hold the write lock, we can`t do
		 *	*anything* which might block for memory.  Otherwise
		 *	a non-privileged thread might deadlock with the
		 *	privileged threads.  We can`t block while taking the
		 *	write lock.  Otherwise a non-privileged thread
		 *	blocked in the vm_deallocate (while holding a read
		 *	lock) will block a privileged thread.  For the same
		 *	reason, we can`t take a read lock and then use
		 *	lock_read_to_write.
		 */

		mutex_lock(&fp->f_lock);

		olddata[level] = fp->f_blk[level];
		oldsize[level] = fp->f_blksize[level];

		fp->f_blkno[level] = ind_block_num;
		fp->f_blk[level] = data;
		fp->f_blksize[level] = size;

		/*
		 *	Return to holding a read lock, and
		 *	dispose of old data.
		 */

	    }

	    if (level > 0) {
		idx = file_block / fp->f_nindir[level-1];
		file_block %= fp->f_nindir[level-1];
	    }
	    else
		idx = file_block;

	    ind_block_num = le32_to_cpu(((daddr_t *)data)[idx]);
	}

	mutex_unlock(&fp->f_lock);

	/*
	 * After unlocking the file, free any blocks that
	 * we need to free.
	 */
	for (idx = 0; idx < NIADDR; idx++)
	    if (oldsize[idx] != 0)
		(void) vm_deallocate(mach_task_self(),
				     olddata[idx],
				     oldsize[idx]);

	*disk_block_p = ind_block_num;
	return (0);
}

/*
 * Readahead: on sequential cache miss, prefetch up to RA_BLOCKS
 * contiguous disk blocks in a single device_read IPC and insert
 * them all into the page cache.
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
	vm_offset_t cached;
	vm_size_t cached_size;

	if (!fp->f_dev.cache)
		return;

	max_file_block = (fp->i_ic.i_size + block_size - 1) / block_size;

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

		/* Stop if already cached */
		if (page_cache_lookup(fp->f_dev.cache, db,
				      &cached, &cached_size) == 0)
			break;

		n_contig++;
	}

	if (n_contig <= 1)
		return;

	/* Single large device_read for the whole run */
	rc = device_read(fp->f_dev.dev_port, 0,
			 (recnum_t) dbtorec(&fp->f_dev,
					    ext2_fsbtodb(fs, disk_block)),
			 n_contig * block_size,
			 (char **)&ra_buf, (unsigned int *)&ra_buf_size);
	if (rc != 0)
		return;

	/* Insert each block into page cache */
	for (i = 0; i < n_contig; i++) {
		page_cache_insert(fp->f_dev.cache, disk_block + i,
				  ra_buf + i * block_size,
				  (vm_size_t)block_size);
	}

	(void)vm_deallocate(mach_task_self(), ra_buf, ra_buf_size);
}

/*
 * Read a portion of a file into an internal buffer.  Return
 * the location in the buffer and the amount in the buffer.
 */
static int
buf_read_file(
	register struct ext2fs_file	*fp,
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
	if (offset >= fp->i_ic.i_size)
	    return (FS_NOT_IN_FILE);

	fs = fp->f_fs;

	off = ext2_blkoff(fs, offset);
	file_block = ext2_lblkno(fs, offset);
	block_size = ext2_blksize(fs, fp, file_block);

	if (off || (!*buf_p) || *size_p < block_size ||
	    ((*buf_p) & (fp->f_dev.rec_size-1))) {
	    if (file_block != fp->f_buf_blkno) {
	        rc = block_map(fp, file_block, &disk_block);
		if (rc != 0)
		    return (rc);

		if (fp->f_buf) {
		    if (!fp->f_buf_borrowed)
			(void)vm_deallocate(mach_task_self(),
					    fp->f_buf,
					    fp->f_buf_size);
		    fp->f_buf = 0;
		    fp->f_buf_borrowed = 0;
		}

		if (disk_block == 0) {
		    (void)vm_allocate(mach_task_self(),
				      &fp->f_buf,
				      block_size,
				      TRUE);
		    memset((void *)fp->f_buf, 0, block_size);
		    fp->f_buf_size = block_size;
		} else if (fp->f_dev.cache) {
		    vm_offset_t cached;
		    vm_size_t   cached_size;

		    if (page_cache_lookup(fp->f_dev.cache, disk_block,
					  &cached, &cached_size) == 0) {
			/* Page cache hit — borrow pointer (zero-copy) */
			fp->f_buf = cached;
			fp->f_buf_size = block_size;
			fp->f_buf_borrowed = 1;
		    } else {
			/* Page cache miss — readahead if sequential */
			if (file_block == fp->f_ra_last_block + 1)
				ext2_readahead(fp, file_block, disk_block);

			/* Re-check cache (readahead may have populated it) */
			if (page_cache_lookup(fp->f_dev.cache, disk_block,
					      &cached, &cached_size) == 0) {
				fp->f_buf = cached;
				fp->f_buf_size = block_size;
				fp->f_buf_borrowed = 1;
			} else if (fp->f_dev.cache->pc_dma_pool &&
				   device_read_phys != NULL) {
				/* Zero-copy DMA path */
				struct page_cache_entry *e;
				e = page_cache_alloc_entry(fp->f_dev.cache,
							   disk_block);
				if (e) {
					unsigned int pa =
						(unsigned int)e->pc_phys;
					io_buf_len_t br;
					rc = device_read_phys(
						fp->f_dev.dev_port, 0,
						(recnum_t) dbtorec(&fp->f_dev,
							ext2_fsbtodb(fs,
								disk_block)),
						(io_buf_len_t) block_size,
						&pa, 1, &br);
					if (rc)
						return (rc);
					fp->f_buf = e->pc_data;
					fp->f_buf_size = block_size;
					fp->f_buf_borrowed = 1;
				} else {
					goto fallback_read;
				}
			} else {
fallback_read:
				/* Single block read (fallback) */
				rc = device_read(fp->f_dev.dev_port,
					     0,
					     (recnum_t) dbtorec(&fp->f_dev,
							ext2_fsbtodb(fs,
								disk_block)),
					     (int) block_size,
					     (char **) &fp->f_buf,
					     (unsigned int *)&fp->f_buf_size);
				if (rc)
				    return (rc);
				page_cache_insert(fp->f_dev.cache, disk_block,
						  fp->f_buf, fp->f_buf_size);
			}
		    }
		} else {
		    rc = device_read(fp->f_dev.dev_port,
				     0,
				     (recnum_t) dbtorec(&fp->f_dev,
							ext2_fsbtodb(fs,
								disk_block)),
				     (int) block_size,
				     (char **) &fp->f_buf,
				     (unsigned int *)&fp->f_buf_size);
	        }
		if (rc)
		    return (rc);

	        fp->f_buf_blkno = file_block;
	    }

	    /*
	     * Return address of byte in buffer corresponding to
	     * offset, and size of remainder of buffer after that
	     * byte.
	     */
	    *buf_p = fp->f_buf + off;
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
			vm_offset_t cached;
			vm_size_t   cached_size;

			if (page_cache_lookup(fp->f_dev.cache, disk_block,
					      &cached, &cached_size) == 0) {
				/* Page cache hit — copy to caller */
				memcpy((void *)*buf_p, (void *)cached,
				       block_size);
				*size_p = block_size;
			} else {
				/* Page cache miss — readahead if sequential */
				if (file_block == fp->f_ra_last_block + 1)
					ext2_readahead(fp, file_block,
						       disk_block);

				/* Re-check cache after readahead */
				if (page_cache_lookup(fp->f_dev.cache,
						disk_block,
						&cached, &cached_size) == 0) {
					memcpy((void *)*buf_p,
					       (void *)cached, block_size);
					*size_p = block_size;
				} else if (fp->f_dev.cache->pc_dma_pool &&
					   device_read_phys != NULL) {
					/* Zero-copy DMA into cache */
					struct page_cache_entry *e;
					e = page_cache_alloc_entry(
						fp->f_dev.cache, disk_block);
					if (e) {
						unsigned int pa =
							(unsigned int)e->pc_phys;
						io_buf_len_t br;
						rc = device_read_phys(
							fp->f_dev.dev_port, 0,
							(recnum_t) dbtorec(
								&fp->f_dev,
								ext2_fsbtodb(fs,
									disk_block)),
							(io_buf_len_t) block_size,
							&pa, 1, &br);
						if (rc)
							return (rc);
						memcpy((void *)*buf_p,
						       (void *)e->pc_data,
						       block_size);
						*size_p = block_size;
					} else {
						goto fallback_read_direct;
					}
				} else {
fallback_read_direct:
					/* Single block read (fallback) */
					rc = device_read_overwrite(
						fp->f_dev.dev_port, 0,
						(recnum_t) dbtorec(&fp->f_dev,
							ext2_fsbtodb(fs,
								disk_block)),
						(int) block_size,
						*buf_p,
						(unsigned int *)size_p);
					if (rc)
						return (rc);
					page_cache_insert(fp->f_dev.cache,
							  disk_block,
							  *buf_p, *size_p);
				}
			}
		} else {
			rc = device_read_overwrite(fp->f_dev.dev_port,
				     0,
				     (recnum_t) dbtorec(&fp->f_dev,
							ext2_fsbtodb(fs,
								disk_block)),
				     (int) block_size,
				     *buf_p,
				     (unsigned int *)size_p);
			if (rc)
				return (rc);
		}
	}

	/*
	 * Truncate buffer at end of file.
	 */
	if (*size_p > fp->i_ic.i_size - offset)
	    *size_p = fp->i_ic.i_size - offset;

	fp->f_ra_last_block = file_block;
	return (0);
}

/*
 * Search a directory for a name and return its
 * i_number.
 */
static int
search_directory(
	char *name,
	register struct ext2fs_file *fp,
	ino_t *inumber_p)
{
	vm_offset_t	buf;
	vm_size_t	buf_size;
	vm_offset_t	offset;
	register struct ext2_dir_entry *dp;
	int		length;
	kern_return_t	rc;
	char		tmp_name[256];

#ifdef	DEBUG
	if(debug)
		printf("search_directory(%s)\n", name);
#endif 
	length = strlen(name);

	offset = 0;
	while (offset < fp->i_ic.i_size) {
	    buf = 0;
	    buf_size = 0;
	    rc = buf_read_file(fp, offset, &buf, &buf_size);
	    if (rc != KERN_SUCCESS)
		return (rc);

	    dp = (struct ext2_dir_entry *)buf;
	    if (le32_to_cpu(dp->inode) != 0) {
		strncpy (tmp_name, dp->name, le16_to_cpu(dp->name_len));
		tmp_name[le16_to_cpu(dp->name_len)] = '\0';
		if (le16_to_cpu(dp->name_len) == length &&
		    !strcmp(name, tmp_name))
	    	{
		    /* found entry */
		    *inumber_p = le32_to_cpu(dp->inode);
		    return (0);
		}
	    }
	    offset += le16_to_cpu(dp->rec_len);
	}
	return (FS_NO_ENTRY);
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
	mach_msg_type_number_t	buf_size;
	mach_msg_type_number_t	buf2_size;
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
	error = device_read(dev->dev_port, 0, 
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
		printf("ext2: unsupported incompat features 0x%lx\n",
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
	error = device_read(dev->dev_port, 0, 
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
	register struct ext2_super_block *fs;
	int error;

	error = read_fs(&fp->f_dev, &fp->f_fs, &fp->f_gd, &fp->f_gd_size);
	if (error)
	    return (error);

	fs = fp->f_fs;

	/*
	 * Calculate indirect block levels.
	 */
	{
	    register int	mult;
	    register int	level;

	    mult = 1;
	    for (level = 0; level < NIADDR; level++) {
		mult *= NINDIR(fs);
		fp->f_nindir[level] = mult;
	    }
	}

	return (0);
}

static void
unmount_fs(register struct ext2fs_file *fp)
{
	if (file_is_structured(fp)) {
	    (void) vm_deallocate(mach_task_self(),
				 (vm_offset_t) fp->f_fs,
				 SBSIZE);
	    (void) vm_deallocate(mach_task_self(),
				 (vm_offset_t) fp->f_gd,
				 fp->f_gd_size);
	    fp->f_fs = 0;
	}
}

/*
 * Open a file.
 */
int
ext2fs_open_file(
	struct device *dev,
	const char *path,
	fs_private_t *private)
{
#define	RETURN(code)	{ rc = (code); goto exit; }

	register char	*cp, *component;
	register int	c;	/* char */
	register int	rc;
	ino_t		inumber, parent_inumber;
	int		nlinks = 0;
	char	        *namebuf;
	struct ext2fs_file *fp;

#ifdef	DEBUG
	if(debug)
		printf("ext2fs_open_file(%s, %s)\n", dev, path);
#endif 
	if (path == 0 || *path == '\0') {
	    return FS_NO_ENTRY;
	}

	namebuf = (char*)malloc((size_t)(PATH_MAX+1));
	fp = (struct ext2fs_file *)malloc(sizeof(struct ext2fs_file));
	memset((void *)fp, '\0', sizeof(struct ext2fs_file));
	fp->f_dev = *dev;
	/*
	 * Copy name into buffer to allow modifying it.
	 */
	strcpy(namebuf, path);

	cp = namebuf;

	rc = mount_fs(fp);
	if (rc)
	    return rc;

	inumber = (ino_t) ROOTINO;
	if ((rc = read_inode(inumber, fp)) != 0)
	    goto exit;

	while (*cp) {

	    /*
	     * Check that current node is a directory.
	     */
	    if ((fp->i_ic.i_mode & IFMT) != IFDIR)
		RETURN (FS_NOT_DIRECTORY);

	    /*
	     * Remove extra separators
	     */
	    while (*cp == '/')
		cp++;

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
	    if ((fp->i_ic.i_mode & IFMT) == IFLNK) {

		int	link_len = fp->i_ic.i_size;
		int	len;

		len = strlen(cp) + 1;

		if (link_len + len >= MAXPATHLEN - 1)
		    RETURN (FS_NAME_TOO_LONG);

		if (++nlinks > MAXSYMLINKS)
		    RETURN (FS_SYMLINK_LOOP);

		ovbcopy(cp, &namebuf[link_len], len);

#ifdef	IC_FASTLINK
		if (fp->i_ic.i_blocks == 0) {
		    memcpy(namebuf,
			   (char *)fp->i_ic.i_block, (unsigned)link_len);
		}
		else
#endif	/* IC_FASTLINK */
		{
		    /*
		     * Read file for symbolic link
		     */
		    vm_offset_t	buf;
		    mach_msg_type_number_t	buf_size;
		    daddr_t	disk_block;
		    register struct ext2_super_block *fs = fp->f_fs;

		    (void) block_map(fp, (daddr_t)0, &disk_block);
		    rc = device_read(fp->f_dev.dev_port,
				     0,
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
	 * Found terminal component.
	 */
#if MACH_DEV_LINK
    out_ok:
#endif	/* MACH_DEV_LINK */
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
	ext2fs_flush_metadata(private);

	/*
	 * Free the disk super-block.
	 */
	unmount_fs(fp);

	/*
	 * Free the inode and data buffers.
	 */
	free_file_buffers(fp);
}

/*
 * Copy a portion of a file into kernel memory.
 * Cross block boundaries when necessary.
 */
int
ext2fs_read_file(
	fs_private_t private,
	vm_offset_t		offset,
	vm_offset_t		start,
	vm_size_t		size)
{
  	register struct ext2fs_file	*fp = (struct ext2fs_file *)private;
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
	    rc = buf_read_file(fp, offset, &buf, &buf_size);
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

boolean_t
ext2fs_file_is_directory(fs_private_t private)
{
  	register struct ext2fs_file	*fp = (struct ext2fs_file *)private;
	return((fp->i_ic.i_mode & IFMT) == IFDIR);
}

size_t
ext2fs_file_size(fs_private_t private)
{
  	register struct ext2fs_file	*fp = (struct ext2fs_file *)private;
	return(fp->i_ic.i_size);
}

boolean_t
ext2fs_file_is_executable(fs_private_t private)
{
  	register struct ext2fs_file	*fp = (struct ext2fs_file *)private;
	if ((fp->i_ic.i_mode & IFMT) != IFREG
#if 0
	    || (fp->i_ic.i_mode & (IEXEC|IEXEC>>3|IEXEC>>6)) == 0
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

	rc = device_write(fp->f_dev.dev_port, 0,
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
	return device_read(fp->f_dev.dev_port, 0,
			   (recnum_t) dbtorec(&fp->f_dev,
					      ext2_fsbtodb(fp->f_fs,
							  disk_block)),
			   (int) EXT2_BLOCK_SIZE(fp->f_fs),
			   (char **) data_out,
			   (unsigned int *) size_out);
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
block_alloc(struct ext2fs_file *fp, int goal_group)
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
					fp->f_gd_dirty = 1;

					/* Update superblock */
					fs->s_free_blocks_count--;
					fp->f_super_dirty = 1;

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
 * Serialize the in-core inode into the cached raw inode block.
 * The block must already be in fp->f_inode_blk (populated by read_inode).
 */
static void
serialize_inode(struct ext2fs_file *fp)
{
	struct ext2_super_block *fs = fp->f_fs;
	struct ext2_inode *raw_inode;
	struct ext2_inode *inode = &fp->i_ic;
	unsigned long block;

	raw_inode = (struct ext2_inode *)
		((char *)fp->f_inode_blk +
		 ext2_itoo(fs, fp->f_ino) * EXT2_INODE_SIZE(fs));

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
 * Write the inode back to disk using the cached inode block.
 * No device_read needed — the block was cached by read_inode().
 */
static int
write_inode(ino_t inumber, struct ext2fs_file *fp)
{
	struct ext2_super_block *fs = fp->f_fs;
	daddr_t disk_block;

	if (fp->f_inode_blk == 0)
		return KERN_FAILURE;

	serialize_inode(fp);

	disk_block = ext2_ino2blk(fs, fp->f_gd, inumber);
	return write_disk_block(fp, disk_block,
				fp->f_inode_blk, EXT2_BLOCK_SIZE(fs));
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

	return device_write(fp->f_dev.dev_port, 0,
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

	/* Convert to little-endian for on-disk format */
	memset(&raw, 0, sizeof(raw));
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

	return device_write(fp->f_dev.dev_port, 0,
			    (recnum_t) dbtorec(&fp->f_dev, SBLOCK),
			    (io_buf_ptr_t) &raw,
			    (mach_msg_type_number_t) SBSIZE,
			    &bytes_written);
}

/*
 * Get or allocate an indirect block, then store 'value' at 'idx'.
 * *ind_blk_p is the indirect block number (0 = must allocate).
 * On success, *ind_blk_p is updated (if allocated), and the indirect
 * block is written back to disk.  fp->i_ic.i_blocks is updated for
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
		fp->i_ic.i_blocks += block_size / DEV_BSIZE;

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

/*
 * Write data to a file at the given offset.
 * Allocates new blocks as needed, extends file size.
 */
int
ext2fs_write_file(
	fs_private_t		private,
	vm_offset_t		offset,
	vm_offset_t		data,
	vm_size_t		size)
{
	struct ext2fs_file *fp = (struct ext2fs_file *)private;
	struct ext2_super_block *fs = fp->f_fs;
	int block_size = EXT2_BLOCK_SIZE(fs);
	int rc;

	while (size > 0) {
		daddr_t file_block = ext2_lblkno(fs, offset);
		int off = ext2_blkoff(fs, offset);
		vm_size_t chunk = block_size - off;
		daddr_t disk_block;

		if (chunk > size)
			chunk = size;

		/* Resolve file block → disk block */
		rc = block_map(fp, file_block, &disk_block);
		if (rc != 0)
			return rc;

		if (disk_block == 0) {
			int nindir = NINDIR(fs);

			disk_block = block_alloc(fp, 0);
			if (disk_block == 0)
				return KERN_RESOURCE_SHORTAGE;

			if (file_block < NDADDR) {
				/* Direct block */
				fp->i_ic.i_block[file_block] = disk_block;

			} else if (file_block < NDADDR + nindir) {
				/* Single indirect */
				int idx = file_block - NDADDR;
				daddr_t ind = fp->i_ic.i_block[EXT2_IND_BLOCK];

				rc = indirect_set(fp, &ind, idx,
						  disk_block, block_size);
				if (rc != 0) return rc;
				fp->i_ic.i_block[EXT2_IND_BLOCK] = ind;
				invalidate_ind_cache(fp, 0, ind);

			} else if (file_block < NDADDR + nindir +
				   nindir * nindir) {
				/* Double indirect */
				int rem = file_block - NDADDR - nindir;
				int idx1 = rem / nindir;
				int idx2 = rem % nindir;
				daddr_t dind =
					fp->i_ic.i_block[EXT2_DIND_BLOCK];
				daddr_t sind;
				vm_offset_t dind_buf;
				vm_size_t dind_size;

				/* Get/alloc double-indirect block */
				if (dind == 0) {
					dind = block_alloc(fp, 0);
					if (dind == 0)
						return KERN_RESOURCE_SHORTAGE;
					fp->i_ic.i_block[EXT2_DIND_BLOCK] =
						dind;
					fp->i_ic.i_blocks +=
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

				/* Set data block in single-indirect */
				rc = indirect_set(fp, &sind, idx2,
						  disk_block, block_size);
				if (rc != 0) return rc;

				/* Update double-indirect entry if sind
				 * was just allocated */
				rc = indirect_set(fp, &dind, idx1,
						  sind, block_size);
				if (rc != 0) return rc;
				fp->i_ic.i_block[EXT2_DIND_BLOCK] = dind;
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
					fp->i_ic.i_block[EXT2_TIND_BLOCK];
				daddr_t dind, sind;
				vm_offset_t tbuf, dbuf;
				vm_size_t tsize, dsize;

				/* Get/alloc triple-indirect block */
				if (tind == 0) {
					tind = block_alloc(fp, 0);
					if (tind == 0)
						return KERN_RESOURCE_SHORTAGE;
					fp->i_ic.i_block[EXT2_TIND_BLOCK] =
						tind;
					fp->i_ic.i_blocks +=
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

				/* Get/alloc double-indirect */
				if (dind == 0) {
					dind = block_alloc(fp, 0);
					if (dind == 0)
						return KERN_RESOURCE_SHORTAGE;
					fp->i_ic.i_blocks +=
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

				/* Set data block in single-indirect */
				rc = indirect_set(fp, &sind, idx3,
						  disk_block, block_size);
				if (rc != 0) return rc;

				/* Update double → single */
				rc = indirect_set(fp, &dind, idx2,
						  sind, block_size);
				if (rc != 0) return rc;

				/* Update triple → double */
				rc = indirect_set(fp, &tind, idx1,
						  dind, block_size);
				if (rc != 0) return rc;
				fp->i_ic.i_block[EXT2_TIND_BLOCK] = tind;
				invalidate_ind_cache(fp, 0, sind);
				invalidate_ind_cache(fp, 1, dind);
				invalidate_ind_cache(fp, 2, tind);
			}
			fp->i_ic.i_blocks += block_size / DEV_BSIZE;
		}

		/* Invalidate f_buf so read path re-fetches from cache */
		if (fp->f_buf_blkno == file_block) {
			if (fp->f_buf) {
				if (!fp->f_buf_borrowed)
					vm_deallocate(mach_task_self(),
						      fp->f_buf,
						      fp->f_buf_size);
				fp->f_buf = 0;
				fp->f_buf_borrowed = 0;
			}
			fp->f_buf_blkno = -1;
		}

		if (off == 0 && chunk == (vm_size_t)block_size) {
			/* Full block write — use page cache */
			if (fp->f_dev.cache)
				page_cache_update(fp->f_dev.cache,
						  disk_block, data, chunk);
			else {
				rc = write_disk_block(fp, disk_block,
						      data, chunk);
				if (rc != 0)
					return rc;
			}
		} else {
			/* Partial block — read-modify-write */
			vm_offset_t blkbuf;
			vm_size_t blkbuf_size;

			if (fp->f_dev.cache) {
				vm_offset_t cached;
				vm_size_t cached_size;

				if (page_cache_lookup(fp->f_dev.cache,
						      disk_block,
						      &cached,
						      &cached_size) == 0) {
					/* Modify in-place via update */
					vm_offset_t tmp;
					if (vm_allocate(mach_task_self(),
							&tmp, block_size,
							TRUE) != KERN_SUCCESS)
						return KERN_RESOURCE_SHORTAGE;
					memcpy((void *)tmp, (void *)cached,
					       block_size);
					memcpy((void *)(tmp + off),
					       (void *)data, chunk);
					page_cache_update(fp->f_dev.cache,
							  disk_block,
							  tmp, block_size);
					vm_deallocate(mach_task_self(),
						      tmp, block_size);
					goto next;
				}
			}

			/* Read existing block from disk */
			rc = read_disk_block(fp, disk_block,
					     &blkbuf, &blkbuf_size);
			if (rc != 0) {
				/* New block: zero-fill */
				if (vm_allocate(mach_task_self(), &blkbuf,
						block_size, TRUE) != KERN_SUCCESS)
					return KERN_RESOURCE_SHORTAGE;
				memset((void *)blkbuf, 0, block_size);
				blkbuf_size = block_size;
			}

			memcpy((void *)(blkbuf + off), (void *)data, chunk);

			if (fp->f_dev.cache)
				page_cache_update(fp->f_dev.cache,
						  disk_block,
						  blkbuf, block_size);
			else {
				rc = write_disk_block(fp, disk_block,
						      blkbuf, block_size);
			}
			vm_deallocate(mach_task_self(), blkbuf, blkbuf_size);
			if (rc != 0)
				return rc;
		}

	next:
		offset += chunk;
		data += chunk;
		size -= chunk;
	}

	/* Update file size if extended */
	if (offset > fp->i_ic.i_size)
		fp->i_ic.i_size = offset;

	/* Mark inode dirty — flushed on sync or close.
	 * gd and superblock are marked dirty in block_alloc() only
	 * when new blocks are actually allocated. */
	fp->f_inode_dirty = 1;

	return 0;
}

/*
 * Flush dirty metadata to disk.  When 2+ items are dirty, batch
 * them into a single device_write_batch IPC to save round-trips.
 */
int
ext2fs_flush_metadata(fs_private_t private)
{
	struct ext2fs_file *fp = (struct ext2fs_file *)private;
	struct ext2_super_block *fs = fp->f_fs;
	int n_dirty = 0;
	int rc;

	if (fp->f_inode_dirty) n_dirty++;
	if (fp->f_gd_dirty) n_dirty++;
	if (fp->f_super_dirty) n_dirty++;

	if (n_dirty == 0)
		return 0;

	/* Single dirty item or no batch stub: unbatched path */
	if (n_dirty == 1 || device_write_batch == NULL) {
		if (fp->f_inode_dirty) {
			rc = write_inode(fp->f_ino, fp);
			if (rc != 0) return rc;
			fp->f_inode_dirty = 0;
		}
		if (fp->f_gd_dirty) {
			rc = write_gd(fp);
			if (rc != 0) return rc;
			fp->f_gd_dirty = 0;
		}
		if (fp->f_super_dirty) {
			rc = write_super(fp);
			if (rc != 0) return rc;
			fp->f_super_dirty = 0;
		}
		return 0;
	}

	/*
	 * Multiple dirty items — batch the writes into one IPC.
	 * The inode block is cached in fp->f_inode_blk from read_inode(),
	 * so no device_read is needed here.
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

		/* --- Prepare inode block (serialize in-place) --- */
		if (fp->f_inode_dirty) {
			if (fp->f_inode_blk == 0)
				return KERN_FAILURE;
			serialize_inode(fp);

			daddr_t inode_disk_block = ext2_ino2blk(fs,
						fp->f_gd, fp->f_ino);
			recnums[n] = (recnum_t)dbtorec(&fp->f_dev,
				ext2_fsbtodb(fs, inode_disk_block));
			sizes[n] = inode_blk_size;
			total_size += inode_blk_size;
			n++;
		}

		/* --- Prepare group descriptors --- */
		if (fp->f_gd_dirty) {
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
		if (fp->f_super_dirty) {
			memset(&raw_sb, 0, sizeof(raw_sb));
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

		/* --- Concatenate data into a single OOL buffer --- */
		rc = vm_allocate(mach_task_self(), &concat, total_size, TRUE);
		if (rc != KERN_SUCCESS)
			return rc;

		off = 0;
		if (fp->f_inode_dirty) {
			memcpy((void *)(concat + off),
			       (void *)fp->f_inode_blk, inode_blk_size);
			off += inode_blk_size;
		}
		if (fp->f_gd_dirty) {
			memcpy((void *)(concat + off),
			       (void *)fp->f_gd, fp->f_gd_size);
			off += fp->f_gd_size;
		}
		if (fp->f_super_dirty) {
			memcpy((void *)(concat + off),
			       (void *)&raw_sb, SBSIZE);
			off += SBSIZE;
		}

		/* --- Send batched write --- */
		rc = device_write_batch(fp->f_dev.dev_port, 0,
					recnums, n, sizes, n,
					(io_buf_ptr_t)concat, total_size,
					&bytes_written);

		/* --- Cleanup --- */
		vm_deallocate(mach_task_self(), concat, total_size);

		if (rc == KERN_SUCCESS) {
			fp->f_inode_dirty = 0;
			fp->f_gd_dirty = 0;
			fp->f_super_dirty = 0;
		}
		return rc;
	}
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
