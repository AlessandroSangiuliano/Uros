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
 * ext_server/ext_server.c — Userspace ext2/ext3/ext4 filesystem server for Uros
 *
 * Architecture:
 *   - Block I/O via ahci_driver (netname_look_up + device_read RPC)
 *   - ext2 parsing provided by libsa_fs (ext2fs.c, unchanged)
 *   - MIG server interface: ext2fs_server.defs (subsystem 2920)
 *   - Per-mount ports registered via netname_check_in("ext_server")
 *   - Multiple partitions supported via mount_context array
 *
 * Clients:
 *   netname_look_up(name_server_port, "", "ext_server", &fs_port);
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
#include <mach/cap_types.h>
#include <sa_mach.h>
#include <device/device.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <servers/netname.h>
#include <servers/netname_defs.h>

#include <pthread.h>
#include <ext2fs/ext2fs.h>
#include <ext2fs/defs.h>
#include <file_system.h>
#include <page_cache.h>
#include <blk.h>
#include <libcap.h>
#include <libfspager.h>                  /* file-backed mmap pager (#276) */
#include <gpu_console.h>

#include <flipc2.h>                      /* FLIPC v2 fast-path (#232) */
#include "ext2fs_server_server.h"
#include "vfs_server.h"
#include "vfs_types.h"

/*
 * Whether the ordinary opens are announced (-v).
 *
 * Off, because a boot printed 561 lines from one function -- 450 successful
 * opens and 111 misses, 103 of the misses from a benchmark measuring how fast
 * a lookup fails.  A log where the ordinary shouts as loudly as the broken
 * teaches its reader to skip lines, and the line skipped next is the one that
 * mattered.
 *
 * ⚠️ A switch and not a deletion, and the difference is that the switch can be
 * thrown without a rebuild: bootstrap.conf's second field onwards is argv and
 * it reaches the server -- name_server has read `-d' from it since it was
 * written.  This server was not refusing arguments, it was not looking at
 * them: `(void)argc; (void)argv;'.
 */
static int ext2_verbose = 0;
#include "vfs_flipc.h"                   /* fast-path protocol (#232) */
#include "ahci_batch.h"
#include "device_master.h"

/* MIG stub — generated from mach_port.defs */
extern kern_return_t mach_port_set_protected_payload(
	mach_port_t task, mach_port_t name, unsigned payload);

/* ================================================================
 * Global Mach ports
 * ================================================================ */

static mach_port_t	host_port;
static mach_port_t	device_port;
static mach_port_t	security_port;
static mach_port_t	root_ledger_wired;
static mach_port_t	root_ledger_paged;

/* ================================================================
 * Per-mount context — one per mounted partition
 * ================================================================ */

#define MAX_OPEN_FILES	16
#define MAX_MOUNTS	4

struct open_file {
	int		in_use;
	int		busy;		/* #388: in-flight ops pinning private
					 * (of_op_begin/of_op_end).  close waits
					 * for pinned slots to drain; the dead-
					 * client reclaim skips them. */
	mach_port_t	owner;		/* #385: send right to the client task
					 * that opened this fid via fs_open (0 for
					 * the raw ext2_open path).  A SIGKILL'd
					 * client never sends ext2_close; the send
					 * right turns into a dead name on its death,
					 * and ds_ext2_open reclaims the slot when the
					 * pool is full.  Deallocated on close/reclaim. */
	fs_private_t	private;	/* opaque ext2fs state */
	struct ext2fs_file file_data;	/* pre-allocated (object pool) */
	char		path[256];	/* path for clone matching */
	int		on_dirty_list;	/* non-zero if linked in dirty_head */
	int		dirty_next;	/* index of next dirty, -1 = end */
	int		dirty_prev;	/* index of prev dirty, -1 = head */
};

/*
 * Context for the page cache writeback callback.
 */
struct writeback_ctx {
	struct device	*dev;		/* device with blk handle */
	unsigned int	blk_to_sec;	/* EXT2_BLOCK_SIZE / DEV_BSIZE */
};

/*
 * Mount context: all per-partition state.
 * Protected payload on each mount port points here, so MIG handlers
 * receive it as fs_port_arg and can access the right partition.
 */
struct mount_context {
	int		active;			/* non-zero if mounted */
	struct device	dev;			/* block device */
	struct writeback_ctx wb;		/* writeback context */
	struct open_file open_files[MAX_OPEN_FILES];
	/*
	 * #385: serializes the fid table — the open_files[] pool slot
	 * scan/claim (open), slot free (close) and the dirty list
	 * (head + per-slot links).  This server is multithreaded (MIG
	 * pool + FLIPC fast path + writeback thread); without it two
	 * concurrent opens race the free-slot scan, both claim the same
	 * fid, and the second's ext2fs_open_file_into overwrites the
	 * first's file_data (block map).  The first opener's fid then
	 * reads the second file's blocks at its own offsets — an
	 * offset-preserving cross-file read that corrupted exec'd
	 * images (a fresh ush reading vt_server's .text block).  Order:
	 * of_lock -> ext2fs internal locks (v_lock/alloc); never reversed.
	 */
	pthread_mutex_t	of_lock;
	int		dirty_head;		/* dirty list head (-1 = empty) */
	mach_port_t	port;			/* receive port for this mount */
	char		driver_name[64];	/* block driver name */
	char		service_name[64];	/* netname registration */
	char		flipc_ep[80];		/* FLIPC v2 endpoint name (#232) */
};

static struct mount_context	mounts[MAX_MOUNTS];
static int			n_mounts;
static mach_port_t		port_set;	/* port set for all mounts */

static void
dirty_list_add(struct mount_context *mnt, int idx)
{
	if (mnt->open_files[idx].on_dirty_list)
		return;
	mnt->open_files[idx].on_dirty_list = 1;
	mnt->open_files[idx].dirty_prev = -1;
	mnt->open_files[idx].dirty_next = mnt->dirty_head;
	if (mnt->dirty_head >= 0)
		mnt->open_files[mnt->dirty_head].dirty_prev = idx;
	mnt->dirty_head = idx;
}

static void
dirty_list_remove(struct mount_context *mnt, int idx)
{
	if (!mnt->open_files[idx].on_dirty_list)
		return;
	mnt->open_files[idx].on_dirty_list = 0;
	if (mnt->open_files[idx].dirty_prev >= 0)
		mnt->open_files[mnt->open_files[idx].dirty_prev].dirty_next =
		    mnt->open_files[idx].dirty_next;
	else
		mnt->dirty_head = mnt->open_files[idx].dirty_next;
	if (mnt->open_files[idx].dirty_next >= 0)
		mnt->open_files[mnt->open_files[idx].dirty_next].dirty_prev =
		    mnt->open_files[idx].dirty_prev;
	mnt->open_files[idx].dirty_next = -1;
	mnt->open_files[idx].dirty_prev = -1;
}

/* ================================================================
 * Fid pinning (#388)
 *
 * A fid's ext2fs state (file_data) lives INSIDE its open_files slot,
 * so the slot must not be recycled while any thread still walks that
 * state.  Two rules enforce this:
 *
 *   1. Every handler that dereferences .private brackets the use with
 *      of_op_begin()/of_op_end(): the slot's busy count pins it.
 *      ds_ext2_close waits for busy to drain; the dead-client reclaim
 *      skips busy slots.
 *
 *   2. Whoever closes a fid claims the close by swapping .private to
 *      NULL under of_lock, but leaves .in_use set until
 *      ext2fs_close_file() has returned: a concurrent open must not
 *      repopulate the slot while the close still flushes and frees
 *      through pointers into it.  (Recycling mid-close is how flipc
 *      payload pages ended up walked as an indirect block map: the
 *      garbage writeback storm with stride 0x04040404.)
 * ================================================================ */

static fs_private_t
of_op_begin(struct mount_context *mnt, int idx)
{
	fs_private_t priv = (fs_private_t)0;

	if (idx < 0 || idx >= MAX_OPEN_FILES)
		return priv;
	pthread_mutex_lock(&mnt->of_lock);
	if (mnt->open_files[idx].in_use &&
	    mnt->open_files[idx].private != NULL) {
		priv = mnt->open_files[idx].private;
		mnt->open_files[idx].busy++;
	}
	pthread_mutex_unlock(&mnt->of_lock);
	return priv;
}

static void
of_op_end(struct mount_context *mnt, int idx)
{
	pthread_mutex_lock(&mnt->of_lock);
	mnt->open_files[idx].busy--;
	pthread_mutex_unlock(&mnt->of_lock);
}

/* ================================================================
 * Page cache writeback
 * ================================================================ */

/*
 * Flush a dirty page cache block to disk via libblk.
 * If phys != 0, use zero-copy DMA write.
 * Otherwise fall back to regular write with data copy.
 */
static int
ext2_writeback(void *ctx, daddr_t block, vm_offset_t data, vm_size_t size,
	       vm_offset_t phys)
{
	struct writeback_ctx *wb = (struct writeback_ctx *)ctx;
	io_buf_len_t bytes_written;
	recnum_t recnum = (recnum_t)(block * wb->blk_to_sec *
				     DEV_BSIZE / wb->dev->rec_size);
	kern_return_t rc;

	if (phys && blk_has_phys(wb->dev->blk)) {
		/*
		 * #520: whole.  `phys' is a vm_offset_t and this cast used to
		 * narrow it -- on i386 harmlessly, since the two are the same
		 * type there, and on x86-64 by dropping the top half of a page
		 * address into a DMA write.
		 */
		vm_address_t pa = phys;
		rc = blk_write_phys(wb->dev->blk, recnum,
				    (io_buf_len_t)size,
				    &pa, 1, &bytes_written);
	} else {
		rc = blk_write(wb->dev->blk, recnum,
			       (io_buf_ptr_t)data,
			       (mach_msg_type_number_t)size,
			       &bytes_written);
	}
	if (rc != KERN_SUCCESS) {
		printf("ext2: writeback block %ld failed: %d\n",
		       (long)block, rc);
		return -1;
	}
	return 0;
}

/* ================================================================
 * Background writeback thread
 * ================================================================ */

#define WRITEBACK_INTERVAL_MS	5000	/* flush dirty pages every 5 seconds */

/*
 * Background writeback thread.  Periodically flushes dirty metadata
 * and page cache blocks to disk.  Uses mach_msg with timeout as sleep.
 */
static void *
writeback_thread(void *arg)
{
	mach_port_t	sleep_port;
	kern_return_t	kr;
	mach_msg_header_t msg;
	int		mi;

	(void)arg;

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &sleep_port);
	if (kr != KERN_SUCCESS) {
		printf("ext2: writeback thread: port alloc failed\n");
		return NULL;
	}

	for (;;) {
		(void)mach_msg(&msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
			       0, sizeof(msg), sleep_port,
			       WRITEBACK_INTERVAL_MS, MACH_PORT_NULL);

		for (mi = 0; mi < n_mounts; mi++) {
			struct mount_context *mnt = &mounts[mi];
			if (!mnt->active)
				continue;

			/* Flush dirty metadata (#385: dirty list + slots are
			 * shared with open/close/write — walk under of_lock). */
			pthread_mutex_lock(&mnt->of_lock);
			{
				int i = mnt->dirty_head;
				while (i >= 0) {
					int next =
					    mnt->open_files[i].dirty_next;
					ext2fs_flush_metadata(
					    mnt->open_files[i].private);
					if (!ext2fs_is_dirty(
					    mnt->open_files[i].private))
						dirty_list_remove(mnt, i);
					i = next;
				}
			}
			pthread_mutex_unlock(&mnt->of_lock);

			/* Flush dirty page cache blocks */
			if (mnt->dev.cache)
				page_cache_sync(mnt->dev.cache);
		}
	}

	/* NOTREACHED */
	return NULL;
}

/* ================================================================
 * MIG server routines  (ds_ prefix from ext2fs_server.defs)
 * ================================================================ */

kern_return_t
ds_ext2_open(
	mach_port_t		fs_port_arg,
	ext2_path_t		path,
	natural_t		*fid_out)
{
	struct mount_context *mnt = (struct mount_context *)fs_port_arg;
	fs_private_t priv;
	int fid, rc, i, donor;

	/*
	 * #385: the whole slot allocation is one critical section.  Claim
	 * the free slot (set in_use) BEFORE dropping the lock for the slow
	 * path walk, so a concurrent open cannot pick the same slot or
	 * clone from a half-initialized one.
	 */
	pthread_mutex_lock(&mnt->of_lock);

	/* Find a free slot (pool allocation) */
	for (fid = 0; fid < MAX_OPEN_FILES; fid++)
		if (!mnt->open_files[fid].in_use)
			break;
	if (fid == MAX_OPEN_FILES) {
		/*
		 * #385: pool full — reclaim any fid whose owning client task
		 * has died.  A SIGKILL'd client never sends ext2_close, so its
		 * slot would leak forever; but its owner send right (handed to
		 * us at fs_open) turns into a dead name on task death, which we
		 * detect here with mach_port_type.
		 *
		 * #388: the slot stays in_use until ext2fs_close_file() has
		 * returned — file_data lives inside the slot, and freeing the
		 * slot first lets a concurrent open recycle that memory under
		 * the flush walk.  Slots with in-flight pinned ops (busy) are
		 * skipped: the dead client can't issue new ones.
		 */
		int kk;
		for (kk = 0; kk < MAX_OPEN_FILES; kk++) {
			mach_port_type_t t;
			fs_private_t rpriv;
			mach_port_t   rowner;
			if (!mnt->open_files[kk].in_use || !mnt->open_files[kk].owner)
				continue;
			if (mnt->open_files[kk].private == NULL ||
			    mnt->open_files[kk].busy > 0)
				continue;
			if (mach_port_type(mach_task_self(),
					   mnt->open_files[kk].owner, &t)
			    != KERN_SUCCESS || !(t & MACH_PORT_TYPE_DEAD_NAME))
				continue;
			dirty_list_remove(mnt, kk);
			rpriv  = mnt->open_files[kk].private;
			rowner = mnt->open_files[kk].owner;
			mnt->open_files[kk].private = NULL;
			mnt->open_files[kk].owner   = 0;
			pthread_mutex_unlock(&mnt->of_lock);
			ext2fs_close_file(rpriv);
			(void)mach_port_deallocate(mach_task_self(), rowner);
			printf("ext2: reclaimed fid=%u (dead client)\n",
			       (unsigned)kk + 1);
			pthread_mutex_lock(&mnt->of_lock);
			mnt->open_files[kk].in_use  = 0;
			mnt->open_files[kk].path[0] = '\0';
		}
		for (fid = 0; fid < MAX_OPEN_FILES; fid++)
			if (!mnt->open_files[fid].in_use)
				break;
		if (fid == MAX_OPEN_FILES) {
			pthread_mutex_unlock(&mnt->of_lock);
			return KERN_RESOURCE_SHORTAGE;
		}
	}

	/* Check for existing open with same path — clone inode data
	 * instead of full path walk + disk I/O.  Each opener gets its
	 * own fid with independent read state. */
	donor = -1;
	for (i = 0; i < MAX_OPEN_FILES; i++) {
		/* Skip slots still being populated (private == NULL): a
		 * reservation whose path walk hasn't finished has no valid
		 * file_data to clone yet (#385). */
		if (mnt->open_files[i].in_use &&
		    mnt->open_files[i].private != NULL &&
		    strcmp(mnt->open_files[i].path, path) == 0) {
			donor = i;
			break;
		}
	}

	if (donor >= 0) {
		/* Clone: copy inode, skip path walk */
		ext2fs_clone_file(&mnt->open_files[fid].file_data,
				  &mnt->open_files[donor].file_data);
		priv = (fs_private_t)&mnt->open_files[fid].file_data;
		mnt->open_files[fid].in_use  = 1;
		mnt->open_files[fid].owner   = 0;
		mnt->open_files[fid].private = priv;
		strncpy(mnt->open_files[fid].path, path,
			sizeof(mnt->open_files[fid].path) - 1);
		mnt->open_files[fid].path[
			sizeof(mnt->open_files[fid].path) - 1] = '\0';
		pthread_mutex_unlock(&mnt->of_lock);
		printf("ext2: cloned \"%s\" -> fid=%u (from fid=%u)\n",
		       path, fid + 1, donor + 1);
	} else {
		/*
		 * Full open with path walk (disk I/O).  Reserve the slot now
		 * and record the path, so a racing clone of the same path
		 * still finds us as donor once we finish; drop the lock across
		 * the slow walk, then re-check the outcome under the lock.
		 */
		mnt->open_files[fid].in_use = 1;
		mnt->open_files[fid].owner  = 0;
		strncpy(mnt->open_files[fid].path, path,
			sizeof(mnt->open_files[fid].path) - 1);
		mnt->open_files[fid].path[
			sizeof(mnt->open_files[fid].path) - 1] = '\0';
		mnt->open_files[fid].private = NULL;
		pthread_mutex_unlock(&mnt->of_lock);

		rc = ext2fs_open_file_into(&mnt->dev, path, &priv,
					   &mnt->open_files[fid].file_data);
		if (rc != 0) {
			pthread_mutex_lock(&mnt->of_lock);
			mnt->open_files[fid].in_use = 0;
			mnt->open_files[fid].path[0] = '\0';
			pthread_mutex_unlock(&mnt->of_lock);

			/*
			 * ⚠️ An absent file is an ANSWER, not a fault, and it
			 * used to be announced as one.
			 *
			 * A single boot printed 111 of these, and 103 of them
			 * came from one benchmark measuring how fast a lookup
			 * misses -- so the loudest thing in the log was a test
			 * getting exactly the result it asked for.  The caller
			 * already receives VFS_ERR_NOENT and every test that
			 * cares reports its own verdict; nothing was learning
			 * anything from the line.
			 *
			 * 🔑 What that costs is not tidiness.  A log where the
			 * ordinary shouts as loudly as the broken teaches its
			 * reader to skip lines, and the line skipped next is
			 * the one that mattered.  Real failures -- I/O,
			 * corruption, a mount gone -- still print here, and now
			 * they print alone.
			 */
			if (rc != FS_NO_ENTRY || ext2_verbose)
				printf("ext2: open \"%s\" failed (rc=%d)\n",
				       path, rc);
			return KERN_FAILURE;
		}
		pthread_mutex_lock(&mnt->of_lock);
		mnt->open_files[fid].private = priv;
		pthread_mutex_unlock(&mnt->of_lock);
		/*
		 * And the successful open, 450 lines in the same boot: bring-up
		 * scaffolding from when the question was whether this server
		 * could open a file at all.  The fid goes back to the caller,
		 * which is where it is used.
		 */
		if (ext2_verbose)
			printf("ext2: opened \"%s\" -> fid=%u\n",
			       path, fid + 1);
	}

	*fid_out = (natural_t)(fid + 1);
	return KERN_SUCCESS;
}

kern_return_t
ds_ext2_stat(
	mach_port_t	fs_port_arg,
	natural_t	fid,
	natural_t	*file_size_out)
{
	struct mount_context *mnt = (struct mount_context *)fs_port_arg;
	int idx = (int)fid - 1;
	fs_private_t priv = of_op_begin(mnt, idx);

	if (priv == (fs_private_t)0)
		return KERN_INVALID_ARGUMENT;

	*file_size_out = (natural_t)ext2fs_file_size(priv);
	of_op_end(mnt, idx);
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
	struct mount_context *mnt = (struct mount_context *)fs_port_arg;
	int idx = (int)fid - 1;
	fs_private_t priv;
	kern_return_t kr;
	vm_offset_t buf;
	size_t fsize;
	int rc;

	priv = of_op_begin(mnt, idx);
	if (priv == (fs_private_t)0)
		return KERN_INVALID_ARGUMENT;

	fsize = ext2fs_file_size(priv);

	/* Clamp to file size */
	if (offset >= fsize || count == 0) {
		of_op_end(mnt, idx);
		*data_out       = (pointer_t)0;
		*data_count_out = 0;
		return KERN_SUCCESS;
	}
	if (offset + count > fsize)
		count = (natural_t)(fsize - offset);

	kr = vm_allocate(mach_task_self(), &buf, (vm_size_t)count, TRUE);
	if (kr != KERN_SUCCESS) {
		of_op_end(mnt, idx);
		return kr;
	}

	rc = ext2fs_read_file(priv, (vm_offset_t)offset,
			      buf, (vm_size_t)count);
	of_op_end(mnt, idx);
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
	struct mount_context *mnt = (struct mount_context *)fs_port_arg;
	int idx = (int)fid - 1;
	fs_private_t priv;
	mach_port_t  owner;

	if (idx < 0 || idx >= MAX_OPEN_FILES)
		return KERN_INVALID_ARGUMENT;

	/*
	 * #385/#388: claim the close by swapping private to NULL under
	 * of_lock, after waiting out any in-flight pinned ops.  The slot
	 * keeps in_use set until ext2fs_close_file() (disk I/O, own locks)
	 * has returned: file_data lives inside the slot, and a concurrent
	 * open must not recycle it under the close's flush walk.
	 */
	pthread_mutex_lock(&mnt->of_lock);
	for (;;) {
		if (!mnt->open_files[idx].in_use ||
		    mnt->open_files[idx].private == NULL) {
			/* never opened, mid-open, or another close owns it */
			pthread_mutex_unlock(&mnt->of_lock);
			return KERN_INVALID_ARGUMENT;
		}
		if (mnt->open_files[idx].busy == 0)
			break;
		/* only a client racing close against its own in-flight ops
		 * gets here; drop the lock so they can drain, and re-check */
		pthread_mutex_unlock(&mnt->of_lock);
		pthread_mutex_lock(&mnt->of_lock);
	}
	dirty_list_remove(mnt, idx);
	priv  = mnt->open_files[idx].private;
	owner = mnt->open_files[idx].owner;	/* #385 */
	mnt->open_files[idx].private = NULL;
	mnt->open_files[idx].owner   = 0;
	pthread_mutex_unlock(&mnt->of_lock);

	ext2fs_close_file(priv);
	if (owner)				/* #385: drop the client-task ref */
		(void)mach_port_deallocate(mach_task_self(), owner);

	pthread_mutex_lock(&mnt->of_lock);
	mnt->open_files[idx].in_use  = 0;
	mnt->open_files[idx].path[0] = '\0';
	pthread_mutex_unlock(&mnt->of_lock);

	return KERN_SUCCESS;
}

kern_return_t
ds_ext2_write(
	mach_port_t		fs_port_arg,
	natural_t		fid,
	natural_t		offset,
	pointer_t		data,
	mach_msg_type_number_t	data_count)
{
	struct mount_context *mnt = (struct mount_context *)fs_port_arg;
	int idx = (int)fid - 1;
	fs_private_t priv;
	int rc;

	priv = of_op_begin(mnt, idx);
	if (priv == (fs_private_t)0) {
		/* MIG OOL data is the server's to free even on error */
		vm_deallocate(mach_task_self(), (vm_offset_t)data,
			      (vm_size_t)data_count);
		return KERN_INVALID_ARGUMENT;
	}

	rc = ext2fs_write_file(priv, (vm_offset_t)offset,
			       (vm_offset_t)data, (vm_size_t)data_count);

	/* MIG OOL data must be deallocated by the server */
	vm_deallocate(mach_task_self(), (vm_offset_t)data,
		      (vm_size_t)data_count);

	if (rc != 0) {
		of_op_end(mnt, idx);
		printf("ext2: write fid=%u offset=%u count=%u failed: %d\n",
		       fid, offset, data_count, rc);
		return KERN_FAILURE;
	}

	/* Write sets dirty flags — track for efficient sync (#385: the
	 * dirty list is shared with close and the writeback thread; the
	 * pin guarantees the slot is still open here). */
	pthread_mutex_lock(&mnt->of_lock);
	dirty_list_add(mnt, idx);
	pthread_mutex_unlock(&mnt->of_lock);
	of_op_end(mnt, idx);

	return KERN_SUCCESS;
}

kern_return_t
ds_ext2_sync(
	mach_port_t	fs_port_arg)
{
	struct mount_context *mnt = (struct mount_context *)fs_port_arg;
	int rc;

	/* Flush dirty metadata — iterate only dirty files (#385: under
	 * of_lock, shared with open/close/write/writeback). */
	/*
	 * 🔴 #483: THE WHOLE LIST IS ATTEMPTED, and a failure no longer stops
	 * the walk.
	 *
	 * It used to return on the first entry that failed.  So a failed sync
	 * did not mean "this file did not make it" -- it meant an unknown
	 * prefix made it, one file did not, and an unknown SUFFIX was never
	 * tried.  That is a durability claim that is not true, and it is not
	 * only a benchmark's problem: `proc: shutdown — syncing N mount(s)'
	 * comes through here.
	 *
	 * 🔑 Every entry gets its turn and the failures are counted, so what a
	 * caller learns is "n of m files did not reach the disk" rather than
	 * "something went wrong somewhere".  A file that failed stays on the
	 * dirty list, which is what makes the next sync try it again.
	 */
	pthread_mutex_lock(&mnt->of_lock);
	{
		int i = mnt->dirty_head;
		int failed = 0, attempted = 0;

		while (i >= 0) {
			int next = mnt->open_files[i].dirty_next;

			attempted++;
			rc = ext2fs_flush_metadata(
				mnt->open_files[i].private);
			if (rc != 0)
				failed++;
			else if (!ext2fs_is_dirty(mnt->open_files[i].private))
				dirty_list_remove(mnt, i);
			i = next;
		}

		if (failed != 0) {
			pthread_mutex_unlock(&mnt->of_lock);
			printf("ext2: sync: %d of %d file(s) did not reach the "
			       "disk — they stay dirty and will be tried "
			       "again\n", failed, attempted);
			return KERN_FAILURE;
		}
	}
	pthread_mutex_unlock(&mnt->of_lock);

	/* Sync page cache to disk */
	if (mnt->dev.cache) {
		rc = page_cache_sync(mnt->dev.cache);
		if (rc != 0) {
			printf("ext2: sync failed, %d blocks not written\n",
			       rc);
			return KERN_FAILURE;
		}
	}

	return KERN_SUCCESS;
}

kern_return_t
ds_ext2_open_read(
	mach_port_t		fs_port_arg,
	ext2_path_t		path,
	natural_t		offset,
	natural_t		count,
	natural_t		*fid_out,
	pointer_t		*data_out,
	mach_msg_type_number_t	*data_count_out)
{
	kern_return_t kr;
	natural_t fid;

	/* Open */
	kr = ds_ext2_open(fs_port_arg, path, &fid);
	if (kr != KERN_SUCCESS) {
		*data_out       = (pointer_t)0;
		*data_count_out = 0;
		return kr;
	}

	/* Read */
	kr = ds_ext2_read(fs_port_arg, fid, offset, count,
			  data_out, data_count_out);
	if (kr != KERN_SUCCESS) {
		ds_ext2_close(fs_port_arg, fid);
		return kr;
	}

	*fid_out = fid;
	return KERN_SUCCESS;
}

kern_return_t
ds_ext2_read_close(
	mach_port_t		fs_port_arg,
	natural_t		fid,
	natural_t		offset,
	natural_t		count,
	pointer_t		*data_out,
	mach_msg_type_number_t	*data_count_out)
{
	kern_return_t kr;

	/* Read */
	kr = ds_ext2_read(fs_port_arg, fid, offset, count,
			  data_out, data_count_out);

	/* Close regardless of read result */
	ds_ext2_close(fs_port_arg, fid);

	return kr;
}

/* ================================================================
 * VFS adapter (#220 sub-step B) — implement vfs.defs (subsystem 3000)
 * on top of the existing ds_ext2_* handlers and the ext2fs_* library.
 *
 * This is the first fs_server to expose vfs.defs.  Routines we cannot
 * back yet (truncate, readdir, namespace ops) return KERN_FAILURE; they
 * become real once the ext2fs library grows the matching primitives
 * (tracked under #168 / future ext_server work).  libvfs (#220 sub-step
 * C) is the only intended consumer.
 *
 * Handle convention: vfs_handle_t is the existing 1-based ext2 fid
 * widened to 64 bits.  Zero is reserved on the wire (never returned
 * on success).
 * ================================================================ */

/* #388: pin-based resolver — the returned private stays valid until the
 * matching vfs_op_end().  NULL = bad/half-open/closing handle. */
static fs_private_t
vfs_op_begin(struct mount_context *mnt, vfs_u64_t handle)
{
	if (handle == 0 || handle > MAX_OPEN_FILES)
		return (fs_private_t)0;
	return of_op_begin(mnt, (int)handle - 1);
}

static void
vfs_op_end(struct mount_context *mnt, vfs_u64_t handle)
{
	of_op_end(mnt, (int)handle - 1);
}

static void
vfs_fill_stat(fs_private_t priv, vfs_stat_t *st)
{
	struct ext2_vnode *vn = ext2fs_file_vnode(priv);
	int is_dir = ext2fs_file_is_directory(priv);
	int is_exec = ext2fs_file_is_executable(priv);

	memset(st, 0, sizeof(*st));
	st->st_size     = (vfs_u64_t)ext2fs_file_size(priv);
	st->st_blksize  = 4096;
	st->st_blocks   = (st->st_size + 511) / 512;
	st->st_nlink    = 1;
	st->st_type     = is_dir ? VFS_FT_DIR : VFS_FT_REG;
	st->st_mode     = is_dir  ? 0755 :
	                  is_exec ? 0755 : 0644;
	if (vn) {
		st->st_ino  = (vfs_u64_t)vn->v_ino;
	}
}

kern_return_t
vfs_open(
	mach_port_t	fs_port,
	mach_port_t	client_task,
	vfs_path_t	path,
	int		flags,
	int		mode,
	vfs_u64_t	*handle_out,
	vfs_u32_t	*type_out)
{
	struct mount_context *mnt = (struct mount_context *)fs_port;
	natural_t fid;
	kern_return_t kr;
	fs_private_t priv;

	kr = ds_ext2_open(fs_port, path, &fid);

	/* O_EXCL: a successful open of an existing file is an error. */
	if (kr == KERN_SUCCESS &&
	    (flags & VFS_O_CREAT) && (flags & VFS_O_EXCL)) {
		(void)ds_ext2_close(fs_port, fid);
		*handle_out = 0;
		*type_out   = VFS_FT_UNKNOWN;
		(void)mach_port_deallocate(mach_task_self(), client_task);
		return KERN_FAILURE;
	}

	/* O_CREAT: create the file then reopen if it didn't exist. */
	if (kr != KERN_SUCCESS && (flags & VFS_O_CREAT)) {
		int rc = ext2fs_create(&mnt->dev, path, mode ? mode : 0644);
		if (rc != 0) {
			*handle_out = 0;
			*type_out   = VFS_FT_UNKNOWN;
			(void)mach_port_deallocate(mach_task_self(), client_task);
			return KERN_FAILURE;
		}
		kr = ds_ext2_open(fs_port, path, &fid);
	}

	if (kr != KERN_SUCCESS) {
		*handle_out = 0;
		*type_out   = VFS_FT_UNKNOWN;
		(void)mach_port_deallocate(mach_task_self(), client_task);
		return kr;
	}

	/* #385: record the client task so a dead-name reclaim can free this
	 * fid if the client is killed before it calls close().  We keep the
	 * send right (dropped in ds_ext2_close / the reclaim scan). */
	{
		int stored = 0;
		pthread_mutex_lock(&mnt->of_lock);
		if (fid >= 1 && fid <= MAX_OPEN_FILES &&
		    mnt->open_files[fid - 1].in_use) {
			mnt->open_files[fid - 1].owner = client_task;
			stored = 1;
		}
		pthread_mutex_unlock(&mnt->of_lock);
		if (!stored)
			(void)mach_port_deallocate(mach_task_self(), client_task);
	}

	priv = vfs_op_begin(mnt, (vfs_u64_t)fid);

	/* O_TRUNC: drop a regular file's contents to zero length. */
	if ((flags & VFS_O_TRUNC) && priv &&
	    !ext2fs_file_is_directory(priv))
		(void)ext2fs_truncate_file(priv, 0);

	*handle_out = (vfs_u64_t)fid;
	*type_out   = (priv && ext2fs_file_is_directory(priv))
		      ? VFS_FT_DIR : VFS_FT_REG;
	if (priv)
		vfs_op_end(mnt, (vfs_u64_t)fid);
	return KERN_SUCCESS;
}

kern_return_t
vfs_close(mach_port_t fs_port, vfs_u64_t handle)
{
	return ds_ext2_close(fs_port, (natural_t)handle);
}

kern_return_t
vfs_read(
	mach_port_t		fs_port,
	vfs_u64_t		handle,
	vfs_u64_t		offset,
	vfs_u32_t		count,
	pointer_t		*data_out,
	mach_msg_type_number_t	*data_count_out)
{
	/* Existing ds_ext2_read takes natural_t offset (32-bit).  Until
	 * the backend grows 64-bit offsets, refuse calls past 4 GiB and
	 * truncate large reads to 32-bit count. */
	if (offset > 0xFFFFFFFFu)
		return KERN_INVALID_ARGUMENT;
	return ds_ext2_read(fs_port, (natural_t)handle,
			    (natural_t)offset, (natural_t)count,
			    data_out, data_count_out);
}

kern_return_t
vfs_write(
	mach_port_t		fs_port,
	vfs_u64_t		handle,
	vfs_u64_t		offset,
	pointer_t		data,
	mach_msg_type_number_t	data_count,
	vfs_u32_t		*written_out)
{
	kern_return_t kr;

	if (offset > 0xFFFFFFFFu)
		return KERN_INVALID_ARGUMENT;
	kr = ds_ext2_write(fs_port, (natural_t)handle,
			   (natural_t)offset, data, data_count);
	*written_out = (kr == KERN_SUCCESS) ? data_count : 0;
	return kr;
}

kern_return_t
vfs_truncate(mach_port_t fs_port, vfs_u64_t handle, vfs_u64_t length)
{
	struct mount_context *mnt = (struct mount_context *)fs_port;
	fs_private_t priv = vfs_op_begin(mnt, handle);
	int rc;

	if (!priv)
		return KERN_INVALID_ARGUMENT;
	if (length > 0xFFFFFFFFu) {
		vfs_op_end(mnt, handle);
		return KERN_INVALID_ARGUMENT;
	}
	rc = ext2fs_truncate_file(priv, (vm_size_t)length);
	vfs_op_end(mnt, handle);
	return rc == 0 ? KERN_SUCCESS : KERN_FAILURE;
}

kern_return_t
vfs_stat(mach_port_t fs_port, vfs_path_t path, vfs_stat_t *st)
{
	natural_t fid;
	kern_return_t kr;
	struct mount_context *mnt = (struct mount_context *)fs_port;
	fs_private_t priv;

	/* Open + fstat + close.  v0.1 acceptable; future work: a
	 * dedicated lookup that doesn't allocate an fid. */
	kr = ds_ext2_open(fs_port, path, &fid);
	if (kr != KERN_SUCCESS)
		return kr;
	priv = vfs_op_begin(mnt, (vfs_u64_t)fid);
	if (!priv) {
		ds_ext2_close(fs_port, fid);
		return KERN_FAILURE;
	}
	vfs_fill_stat(priv, st);
	vfs_op_end(mnt, (vfs_u64_t)fid);	/* unpin BEFORE close: it waits */
	ds_ext2_close(fs_port, fid);
	return KERN_SUCCESS;
}

kern_return_t
vfs_fstat(mach_port_t fs_port, vfs_u64_t handle, vfs_stat_t *st)
{
	struct mount_context *mnt = (struct mount_context *)fs_port;
	fs_private_t priv = vfs_op_begin(mnt, handle);

	if (!priv)
		return KERN_INVALID_ARGUMENT;
	vfs_fill_stat(priv, st);
	vfs_op_end(mnt, handle);
	return KERN_SUCCESS;
}

kern_return_t
vfs_readdir(
	mach_port_t		fs_port,
	vfs_u64_t		dir_handle,
	vfs_u64_t		cookie,
	pointer_t		*entries_out,
	mach_msg_type_number_t	*entries_count_out,
	vfs_u64_t		*next_cookie_out)
{
	struct mount_context *mnt = (struct mount_context *)fs_port;
	fs_private_t priv = vfs_op_begin(mnt, dir_handle);
	struct fs_dirent *tmp;
	vfs_dirent_t *outv;
	unsigned int want, got = 0, emit = 0, i;
	vm_offset_t buf;
	kern_return_t kr;
	int rc;

	*entries_out       = (pointer_t)0;
	*entries_count_out = 0;
	*next_cookie_out   = 0;

	if (!priv)
		return KERN_INVALID_ARGUMENT;

	/* ext2fs_readdir() always enumerates from the start, so read
	 * cookie + VFS_DIRENT_MAX entries and slice off the ones already
	 * delivered.  Adequate for the directory sizes this fs sees. */
	want = (unsigned int)cookie + VFS_DIRENT_MAX;
	tmp = (struct fs_dirent *)malloc(want * sizeof(*tmp));
	if (!tmp) {
		vfs_op_end(mnt, dir_handle);
		return KERN_RESOURCE_SHORTAGE;
	}

	rc = ext2fs_readdir(priv, tmp, want, &got);
	vfs_op_end(mnt, dir_handle);
	if (rc != 0) {
		free(tmp);
		return KERN_FAILURE;
	}

	kr = vm_allocate(mach_task_self(), &buf,
			 VFS_DIRENT_MAX * sizeof(vfs_dirent_t), TRUE);
	if (kr != KERN_SUCCESS) {
		free(tmp);
		return kr;
	}
	outv = (vfs_dirent_t *)buf;

	for (i = (unsigned int)cookie; i < got && emit < VFS_DIRENT_MAX; i++) {
		unsigned int nlen = (unsigned int)strlen(tmp[i].name);
		uint8_t vt;
		switch (tmp[i].type) {		/* ext2 FT -> VFS_FT */
		case 1:  vt = VFS_FT_REG;     break;
		case 2:  vt = VFS_FT_DIR;     break;
		case 3:  vt = VFS_FT_CHAR;    break;
		case 4:  vt = VFS_FT_BLOCK;   break;
		case 5:  vt = VFS_FT_FIFO;    break;
		case 6:  vt = VFS_FT_SOCK;    break;
		case 7:  vt = VFS_FT_SYMLINK; break;
		default: vt = VFS_FT_UNKNOWN; break;
		}
		if (nlen > VFS_NAME_MAX)
			nlen = VFS_NAME_MAX;
		outv[emit].d_ino     = (uint64_t)tmp[i].ino;
		outv[emit].d_cookie  = (uint64_t)(i + 1);
		outv[emit].d_type    = vt;
		outv[emit].d_namelen = (uint8_t)nlen;
		memcpy(outv[emit].d_name, tmp[i].name, nlen);
		outv[emit].d_name[nlen] = '\0';
		emit++;
	}
	free(tmp);

	*entries_out       = (pointer_t)buf;
	*entries_count_out = (mach_msg_type_number_t)(emit * sizeof(vfs_dirent_t));
	*next_cookie_out   = (i < got) ? (vfs_u64_t)i : 0;
	return KERN_SUCCESS;
}

kern_return_t
vfs_unlink(mach_port_t fs_port, vfs_path_t path)
{
	struct mount_context *mnt = (struct mount_context *)fs_port;

	return ext2fs_unlink(&mnt->dev, path) == 0
		? KERN_SUCCESS : KERN_FAILURE;
}

kern_return_t
vfs_mkdir(mach_port_t fs_port, vfs_path_t path, int mode)
{
	struct mount_context *mnt = (struct mount_context *)fs_port;

	return ext2fs_mkdir(&mnt->dev, path, mode ? mode : 0755) == 0
		? KERN_SUCCESS : KERN_FAILURE;
}

kern_return_t
vfs_rmdir(mach_port_t fs_port, vfs_path_t path)
{
	struct mount_context *mnt = (struct mount_context *)fs_port;

	return ext2fs_rmdir(&mnt->dev, path) == 0
		? KERN_SUCCESS : KERN_FAILURE;
}

kern_return_t
vfs_rename(mach_port_t fs_port, vfs_path_t old_path, vfs_path_t new_path)
{
	struct mount_context *mnt = (struct mount_context *)fs_port;

	return ext2fs_rename(&mnt->dev, old_path, new_path) == 0
		? KERN_SUCCESS : KERN_FAILURE;
}

kern_return_t
vfs_sync(mach_port_t fs_port)
{
	return ds_ext2_sync(fs_port);
}

/*
 * #284: clean unmount.  Flush everything to disk, then mark the on-disk
 * superblock cleanly unmounted (EXT2_VALID_FS) so the host's fsck.ext2
 * sees a clean volume after a graceful shutdown.  ext2_dev_write bypasses
 * the page cache, so the sync must run first; the mark-clean is the last,
 * authoritative write before host_reboot.
 */
kern_return_t
vfs_unmount(mach_port_t fs_port)
{
	struct mount_context *mnt = (struct mount_context *)fs_port;
	int rc;

	(void) ds_ext2_sync(fs_port);

	rc = ext2fs_mark_clean_dev(&mnt->dev);
	printf("ext2: unmount — '%s' %s\n", mnt->service_name,
	       (rc == 0) ? "marked clean" : "mark-clean FAILED");
	return KERN_SUCCESS;
}

/* ================================================================
 * File-backed mmap (#276 Phase B.3): hand out a libfspager-backed
 * memory_object for the file.  Phase B's read_page returns zeros so
 * we can validate the end-to-end vm_map chain (libposix-uros h_mmap2
 * -> libvfs vfs_mmap -> us -> libfspager -> kernel vm_object).  Phase
 * C plugs ext2fs_pread into read_page and the mapping starts reading
 * real file content.
 * ================================================================ */

static int
ext_pager_read_page(void *state, uint64_t file_id,
                    uint64_t off, void *buf, unsigned int len)
{
	fs_private_t priv = (fs_private_t)(uintptr_t)file_id;
	size_t fsize;
	unsigned int to_read;
	int rc;

	(void)state;

	if (priv == NULL || buf == NULL || len == 0)
		return -EINVAL;

	fsize = ext2fs_file_size(priv);

	/* Wholly past EOF — return a zero page; the kernel will see a
	 * legitimate, all-zero page above the file size (Linux & POSIX
	 * mmap semantics: faulting in the page past the rounded-up file
	 * length yields zeros, not SIGBUS, when the page is partially
	 * within the file). */
	if ((uint64_t)off >= fsize) {
		memset(buf, 0, len);
		return 0;
	}

	to_read = len;
	if ((uint64_t)off + to_read > fsize)
		to_read = (unsigned int)(fsize - off);

	rc = ext2fs_read_file(priv,
	                      (vm_offset_t)off,
	                      (vm_offset_t)buf,
	                      (vm_size_t)to_read);
	if (rc != 0)
		return -EIO;

	/* Final page that straddles EOF: zero the tail so the user task
	 * never sees stale memory beyond the file's logical end. */
	if (to_read < len)
		memset((char *)buf + to_read, 0, len - to_read);

	return 0;
}

static uint64_t
ext_pager_file_size(void *state, uint64_t file_id)
{
	fs_private_t priv = (fs_private_t)(uintptr_t)file_id;
	(void)state;
	if (priv == NULL)
		return 0;
	return (uint64_t)ext2fs_file_size(priv);
}

static void
ext_pager_close_file(void *state, uint64_t file_id)
{
	struct ext2fs_file *fp = (struct ext2fs_file *)(uintptr_t)file_id;

	(void)state;

	/* The pager owns this clone (vfs_mmap copies the open-file slot's
	 * ext2fs_file into a heap allocation when it creates the pager;
	 * see the comment there).  No one else holds a reference to it
	 * once memory_object_terminate has fired, so freeing here is
	 * safe.  Belt-and-braces NULL check in case a future caller
	 * passes 0.
	 *
	 * #283: the clone took a vnode reference in ext2fs_clone_file
	 * (v_refcount++).  We must release it via ext2fs_close_file
	 * before free(), otherwise every file-backed mmap teardown leaks
	 * a vnode-table slot; after VNODE_TABLE_SIZE distinct mappings
	 * the table fills and the next open returns FS_NO_RESOURCES. */
	if (fp != NULL) {
		ext2fs_close_file((fs_private_t)fp);
		free(fp);
	}
}

static int
ext_pager_write_page(void *state, uint64_t file_id,
                     uint64_t off, const void *buf, unsigned int len)
{
	fs_private_t priv = (fs_private_t)(uintptr_t)file_id;
	size_t fsize;
	unsigned int to_write;
	int rc;

	(void)state;

	if (priv == NULL || buf == NULL || len == 0)
		return -EINVAL;

	/* Mach write-back: the kernel hands us a page worth of dirty
	 * bytes (or a clustered run) at `off`.  We let ext2fs_write_file
	 * handle block allocation / extent walk.  Clip at the current
	 * file size — pager-driven writes never extend the file (the
	 * file's grown by explicit POSIX writes or truncate, never by
	 * the page fault path: pages past EOF are zero-filled on read,
	 * dirty pages over the rounded-up final page are clipped here).
	 */
	fsize = ext2fs_file_size(priv);
	if ((uint64_t)off >= fsize)
		return 0;                    /* nothing to persist */

	to_write = len;
	if ((uint64_t)off + to_write > fsize)
		to_write = (unsigned int)(fsize - off);

	rc = ext2fs_write_file(priv,
	                       (vm_offset_t)off,
	                       (vm_offset_t)buf,
	                       (vm_size_t)to_write);
	if (rc != 0)
		return -EIO;

	return 0;
}

static const struct fs_pager_ops ext_pager_ops = {
	.read_page  = ext_pager_read_page,
	.write_page = ext_pager_write_page,
	.close_file = ext_pager_close_file,
	.file_size  = ext_pager_file_size,
};

kern_return_t
vfs_mmap(mach_port_t fs_port, vfs_u64_t handle,
         vfs_u32_t prot, vfs_u32_t flags, mach_port_t *out_mem_obj)
{
	struct mount_context *mnt = (struct mount_context *)fs_port;
	fs_private_t priv;
	mach_port_t mem_obj;

	(void)prot; (void)flags;        /* Phase B: hints only */

	priv = vfs_op_begin(mnt, handle);
	if (priv == NULL) {
		*out_mem_obj = MACH_PORT_NULL;
		return KERN_FAILURE;
	}

	/* The pager's lifetime is decoupled from the open fd's: POSIX
	 * allows close(fd) right after mmap, the mapping (and therefore
	 * the pager) lives on until the last task unmaps it.  We can't
	 * point the pager at the open_files slot — that gets
	 * reused as soon as the client closes the fd, and the next
	 * data_request would walk freed metadata (cr2=0x18 inside
	 * ext2_blkoff, seen during Phase C bringup).  Clone the
	 * ext2fs_file into a heap allocation the pager owns end-to-end
	 * and free in ext_pager_close_file when the kernel sends the
	 * final memory_object_terminate. */
	{
		struct ext2fs_file *clone = malloc(sizeof(*clone));
		if (clone == NULL) {
			vfs_op_end(mnt, handle);
			*out_mem_obj = MACH_PORT_NULL;
			return KERN_RESOURCE_SHORTAGE;
		}
		ext2fs_clone_file(clone, (const struct ext2fs_file *)priv);
		vfs_op_end(mnt, handle);	/* clone done: slot no longer needed */
		priv = (fs_private_t)clone;
	}

	/* Encode the per-file state pointer as file_id so the callbacks
	 * (which receive only file_id, no per-call mount context) can
	 * recover it via cast.  Stable for the lifetime of the pager. */
	mem_obj = fs_pager_create((uint64_t)(uintptr_t)priv,
	                          &ext_pager_ops, NULL);
	if (mem_obj == MACH_PORT_NULL) {
		free((void *)priv);   /* drop the clone we just allocated */
		*out_mem_obj = MACH_PORT_NULL;
		return KERN_FAILURE;
	}
	*out_mem_obj = mem_obj;
	return KERN_SUCCESS;
}

/* ================================================================
 * Multi-subsystem MIG demux: native ext2fs_server (2920) + vfs (3000)
 * ================================================================ */

static boolean_t
ext_server_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
	if (vfs_server(in, out))
		return TRUE;
	if (ext2fs_server_server(in, out))
		return TRUE;
	/* #276: dispatch memory_object_* messages to libfspager so
	 * file-backed mmap traffic (data_request, terminate, etc.)
	 * gets handled alongside the regular fs RPCs in the same loop. */
	if (fs_pager_demux(in, out))
		return TRUE;
	return FALSE;
}

/* ================================================================
 * Mount a single partition
 * ================================================================ */

/*
 * mount_partition — mount a single partition.
 *
 * If blocking is non-zero, blk_open() waits for the driver to appear
 * (used for the first/mandatory partition).  If blocking is zero,
 * blk_open_try() returns immediately with NULL if the driver hasn't
 * registered — used for optional/extra partitions.
 */
static int
mount_partition(struct mount_context *mnt, const char *driver_name,
		const char *service_name, const char *mount_path,
		int blocking)
{
	kern_return_t kr;
	struct blk_dev *bd;

	strncpy(mnt->driver_name, driver_name, sizeof(mnt->driver_name) - 1);
	strncpy(mnt->service_name, service_name,
		sizeof(mnt->service_name) - 1);
	mnt->dirty_head = -1;

	/* Open block device via libblk */
	if (blocking)
		bd = blk_open(name_server_port, driver_name);
	else
		bd = blk_open_try(name_server_port, driver_name);
	if (!bd) {
		if (blocking)
			printf("ext2: blk_open(%s) failed\n", driver_name);
		return -1;
	}
	mnt->dev.blk = bd;
	mnt->dev.dev_port = blk_port(bd);
	mnt->dev.rec_size = blk_rec_size(bd);
	mnt->dev.mount_data = NULL;

	/*
	 * Authenticate the partition port via the Uros capability system.
	 * Without this, BDS rejects every device_read / device_write on
	 * this port with KERN_NO_ACCESS.  The cap is one-shot per port:
	 * once device_open_cap succeeds, all later I/O on the same port
	 * passes the gate until the partition is reset.
	 */
	{
		struct uros_cap tok;
		kern_return_t ckr = cap_request(RESOURCE_BLK_DEVICE,
						cap_name_hash(driver_name),
						CAP_OP_BLK_READ | CAP_OP_BLK_WRITE,
						0 /* no expiry */,
						&tok);
		if (ckr != KERN_SUCCESS) {
			printf("ext2: cap_request(%s) failed (%d)\n",
			       driver_name, ckr);
			return -1;
		}

		char tok_blob[CAP_TOKEN_MAX];
		memcpy(tok_blob, &tok, sizeof(tok));

		security_token_t null_sec = { { 0, 0 } };
		mach_port_t authed = MACH_PORT_NULL;
		ckr = device_open_cap(mnt->dev.dev_port,
				      MACH_PORT_NULL,
				      D_READ | D_WRITE,
				      null_sec,
				      (char *)driver_name,
				      tok_blob,
				      (mach_msg_type_number_t)sizeof(tok),
				      &authed);
		if (ckr != KERN_SUCCESS) {
			printf("ext2: device_open_cap(%s) failed (%d)\n",
			       driver_name, ckr);
			return -1;
		}
		/*
		 * Per-(client, partition) handle (Issue #181): the I/O port
		 * we must use from now on is the one BDS just minted, not
		 * the master partition port we discovered via netname.  Raw
		 * I/O on the master port is rejected by BDS regardless of
		 * how many other clients have authenticated it.
		 */
		mnt->dev.dev_port = authed;
		/*
		 * Re-route libblk through the authenticated handle too:
		 * blk_read/blk_write use bd_port directly, so without this
		 * swap every I/O still hits the master partition port and
		 * BDS rejects it with KERN_NO_ACCESS (Issue #188).
		 */
		blk_set_port(bd, authed);
		printf("ext2: authenticated '%s' via cap %llu (handle=0x%x)\n",
		       driver_name, (unsigned long long)tok.cap_id,
		       (unsigned)authed);
	}

	/* Create initial page cache (non-DMA, no writeback yet) */
	mnt->dev.cache = page_cache_create(8192, NULL, NULL);
	if (!mnt->dev.cache)
		printf("ext2: warning: page cache alloc failed, "
		       "running uncached\n");
	else
		printf("ext2: page cache enabled (8192 blocks)\n");

	/* Verify ext2 superblock by opening a known test file.
	 * Also use the first open to discover the block size and
	 * configure the page cache writeback callback. */
	{
		fs_private_t priv;
		int rc = ext2fs_open_file(&mnt->dev, "hello.txt", &priv);
		if (rc == 0) {
			struct ext2fs_file *fp =
				(struct ext2fs_file *)priv;
			int blksz = EXT2_BLOCK_SIZE(fp->f_fs);

			printf("ext2: mounted, /hello.txt size=%u bytes"
			       " (blk=%d)\n",
			       (unsigned int)ext2fs_file_size(priv),
			       blksz);

			/* Set up writeback context */
			mnt->wb.dev = &mnt->dev;
			mnt->wb.blk_to_sec = blksz / DEV_BSIZE;

			/* Upgrade to DMA-backed page cache */
			{
				/*
				 * #520: addresses are vm_address_t, and the
				 * address list is OUT OF LINE -- so it is no
				 * longer sixteen kilobytes of this frame on
				 * top of the stub's own.  It is memory the
				 * kernel handed over, and this task releases
				 * it below.
				 */
				vm_address_t kva, uva;
				vm_address_t *pa_list = NULL;
				mach_msg_type_number_t pa_cnt = 0;
				unsigned int n_entries = 4096;
				unsigned int n_pages;
				struct page_cache *dma_pc;

				n_pages = (n_entries * blksz + 4095) / 4096;
				if (n_pages > 4096)
					n_pages = 4096;

				kr = device_dma_alloc_sg(
					device_port, n_pages,
					mach_task_self(),
					&kva, &uva,
					&pa_list, &pa_cnt);
				printf("ext2: DMA alloc: kr=%d kva=0x%lx uva=0x%lx n_pages=%u pa_cnt=%u\n",
				       kr, (unsigned long)kva,
				       (unsigned long)uva, n_pages, pa_cnt);
				if (kr == KERN_SUCCESS) {
					dma_pc = page_cache_create_dma(
						n_entries,
						(vm_size_t)blksz,
						(vm_offset_t)uva,
						pa_list, pa_cnt,
						ext2_writeback,
						&mnt->wb);
					if (dma_pc) {
						page_cache_destroy(
							mnt->dev.cache);
						mnt->dev.cache = dma_pc;
						printf("ext2: DMA page "
						       "cache (%u "
						       "entries, %u "
						       "pages)\n",
						       dma_pc->pc_max_entries,
						       pa_cnt);
					} else {
						printf("ext2: DMA cache "
						       "create failed, "
						       "using non-DMA\n");
						vm_deallocate(
							mach_task_self(),
							(vm_offset_t)uva,
							(vm_size_t)n_pages
								* 4096);
					}
				} else {
					printf("ext2: DMA alloc failed "
					       "(kr=%d), using "
					       "non-DMA cache\n", kr);
				}

				/*
				 * ⚠️ #520: the out-of-line list is released on
				 * every path, including the ones that just
				 * failed.  page_cache_create_dma copies what
				 * it needs, so nothing outlives this block --
				 * and a receiver that forgets out-of-line
				 * memory leaks one allocation per mount, which
				 * is exactly the size of leak nobody notices.
				 */
				if (pa_list != NULL)
					(void) vm_deallocate(
						mach_task_self(),
						(vm_address_t)pa_list,
						(vm_size_t)pa_cnt
						* sizeof(vm_address_t));
			}

			/* Set writeback on non-DMA cache if
			 * DMA upgrade failed */
			if (mnt->dev.cache &&
			    !mnt->dev.cache->pc_dma_pool) {
				mnt->dev.cache->pc_writeback =
					ext2_writeback;
				mnt->dev.cache->pc_writeback_ctx =
					&mnt->wb;
			}
			printf("ext2: writeback enabled\n");

			ext2fs_close_file(priv);
			free(priv);
		} else {
			printf("ext2: mount OK (test file not found, "
			       "rc=%d)\n", rc);
		}
	}

	/* Allocate per-mount receive port */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &mnt->port);
	if (kr != KERN_SUCCESS) {
		printf("ext2: port alloc failed for %s\n", service_name);
		return -1;
	}
	kr = mach_port_insert_right(mach_task_self(), mnt->port, mnt->port,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("ext2: port right failed for %s\n", service_name);
		return -1;
	}

	/* Set protected payload — MIG handlers receive mnt as fs_port_arg */
	kr = mach_port_set_protected_payload(mach_task_self(),
					     mnt->port,
					     (unsigned long)mnt);
	if (kr != KERN_SUCCESS)
		printf("ext2: set_protected_payload failed (kr=%d)\n", kr);
	else
		printf("ext2: protected payload set (mnt=%p)\n",
		       (void *)mnt);

	/* Add to port set */
	kr = mach_port_move_member(mach_task_self(), mnt->port, port_set);
	if (kr != KERN_SUCCESS) {
		printf("ext2: port_move_member failed (kr=%d)\n", kr);
		return -1;
	}

	/* Register with name server */
	kr = netname_check_in(name_server_port, service_name,
			      MACH_PORT_NULL, mnt->port);
	if (kr != KERN_SUCCESS)
		printf("ext2: netname_check_in(%s) failed (kr=%d)\n",
		       service_name, kr);
	else
		printf("ext2: registered as \"%s\"\n", service_name);

	/* #220: also register the mount path so libvfs can route
	 * fs_open(path) to this partition without per-client knowledge
	 * of which ext_server instance owns which subtree. */
	if (mount_path) {
		kr = netname_check_in_mount(name_server_port,
					    (char *)mount_path,
					    MACH_PORT_NULL, mnt->port);
		if (kr != KERN_SUCCESS)
			printf("ext2: netname_check_in_mount(%s) "
			       "failed (kr=%d)\n", mount_path, kr);
		else
			printf("ext2: mount registered at \"%s\"\n",
			       mount_path);
	}

	pthread_mutex_init(&mnt->of_lock, NULL);	/* #385 */
	mnt->active = 1;
	return 0;
}

/* ================================================================
 * FLIPC v2 fast-path (#232) — bulk read over a shared-memory channel
 * ================================================================ */

/*
 * Control RPC (vfs.defs): hand libvfs the name of the FLIPC endpoint
 * that serves this mount.  Empty name + non-zero result means no
 * fast-path (libvfs stays on the Mach data path).
 */
kern_return_t
vfs_flipc_endpoint(mach_port_t fs_port, vfs_path_t endpoint, int *result)
{
	struct mount_context *mnt = (struct mount_context *)fs_port;

	if (mnt && mnt->flipc_ep[0]) {
		strncpy(endpoint, mnt->flipc_ep, VFS_PATH_MAX - 1);
		endpoint[VFS_PATH_MAX - 1] = '\0';
		*result = 0;
	} else {
		endpoint[0] = '\0';
		*result = -1;
	}
	return KERN_SUCCESS;
}

/*
 * Serve one pending request on 'rev', replying on 'fwd'.  Non-blocking:
 * returns 1 if a request was processed, 0 if none was pending.  The
 * reactor calls this in a drain loop.  One request is in flight per
 * client (libvfs serialises), so data offset 0 is reused safely.
 */
static int
flipc_serve_one(struct mount_context *mnt, flipc2_channel_t fwd,
		flipc2_channel_t rev)
{
	struct flipc2_desc *req = flipc2_consume_peek(rev);
	if (!req)
		return 0;

	uint32_t op       = req->opcode;
	uint64_t cookie   = req->cookie;
	uint64_t handle   = req->param[1];
	uint64_t offset   = req->param[2];
	uint64_t count    = req->data_length;
	uint64_t data_off = req->data_offset;
	void *wsrc = (op == VFS_FLIPC_OP_WRITE)
		     ? flipc2_data_ptr(rev, data_off) : (void *)0;
	flipc2_consume_release(rev);

	struct flipc2_desc *rep = flipc2_produce_wait(fwd, FLIPC2_SPIN_DEFAULT);
	if (!rep)
		return 1;		/* ring wedged — drop the reply */
	rep->opcode      = op;
	rep->cookie      = cookie;
	rep->flags       = 0;
	rep->data_offset = 0;
	rep->data_length = 0;
	rep->status      = VFS_FLIPC_ERR_INVAL;

	if (op == VFS_FLIPC_OP_READ) {
		int idx = (int)handle - 1;
		fs_private_t priv = of_op_begin(mnt, idx);	/* #388 pin */
		if (priv == (fs_private_t)0) {
			rep->status = VFS_FLIPC_ERR_BADHANDLE;
		} else {
			size_t fsize = ext2fs_file_size(priv);
			uint64_t cap = fwd->hdr->data_size;
			if (cap > VFS_FLIPC_MAX_READ)
				cap = VFS_FLIPC_MAX_READ;
			if (offset >= fsize || count == 0) {
				rep->status = VFS_FLIPC_OK;
			} else {
				if (offset + count > fsize)
					count = fsize - offset;
				if (count > cap)
					count = cap;
				void *dst = flipc2_data_ptr(fwd, 0);
				int rc = ext2fs_read_file(priv,
					(vm_offset_t)offset,
					(vm_offset_t)dst,
					(vm_size_t)count);
				if (rc != 0) {
					rep->status = VFS_FLIPC_ERR_IO;
				} else {
					rep->status      = VFS_FLIPC_OK;
					rep->data_length = count;
				}
			}
			of_op_end(mnt, idx);
		}
	} else if (op == VFS_FLIPC_OP_WRITE) {
		int idx = (int)handle - 1;
		fs_private_t priv = wsrc ? of_op_begin(mnt, idx)	/* #388 pin */
					 : (fs_private_t)0;
		if (priv == (fs_private_t)0) {
			rep->status = VFS_FLIPC_ERR_BADHANDLE;
		} else {
			if (count == 0) {
				rep->status = VFS_FLIPC_OK;
			} else {
				int rc = ext2fs_write_file(priv,
					(vm_offset_t)offset,
					(vm_offset_t)wsrc,
					(vm_size_t)count);
				if (rc != 0) {
					rep->status = VFS_FLIPC_ERR_IO;
				} else {
					/* #388: the dirty list is shared with
					 * close/sync/writeback — of_lock, like
					 * every other dirty_list_add caller */
					pthread_mutex_lock(&mnt->of_lock);
					dirty_list_add(mnt, idx);
					pthread_mutex_unlock(&mnt->of_lock);
					rep->status      = VFS_FLIPC_OK;
					rep->data_length = count;
				}
			}
			of_op_end(mnt, idx);
		}
	}
	flipc2_produce_commit(fwd);
	return 1;
}

struct flipc_reactor_arg {
	flipc2_endpoint_t	ep;
	struct mount_context	*mnt;
};
static struct flipc_reactor_arg flipc_reactor_args[MAX_MOUNTS];

/*
 * Single-thread reactor per endpoint (#232).  One thread owns the whole
 * lifecycle: it polls every connected client's reverse channel for data
 * AND pumps the endpoint control port for connect / dead-name events.
 * Because the same thread that learns a client died is the one consuming
 * its channels, it can drop the channel from the pollset and destroy it
 * with no other thread touching it — eliminating the use-after-free that
 * a separate per-client consumer thread suffered.
 */
#define FLIPC_REACTOR_MAX_CONNS	16
#define FLIPC_POLL_TIMEOUT_MS	10

static void *
flipc_reactor(void *arg)
{
	struct flipc_reactor_arg *ra = (struct flipc_reactor_arg *)arg;
	flipc2_endpoint_t ep = ra->ep;
	struct mount_context *mnt = ra->mnt;
	flipc2_pollset_t ps;
	struct { flipc2_channel_t fwd, rev; } conns[FLIPC_REACTOR_MAX_CONNS];
	int nconns = 0;

	if (flipc2_pollset_create(&ps) != FLIPC2_SUCCESS)
		return NULL;

	for (;;) {
		flipc2_event_t events[FLIPC_REACTOR_MAX_CONNS];
		flipc2_ep_event_t ev;
		int n = 0;
		int i, j;

		/* Data plane.  flipc2_poll arms cons_sleeping and blocks with
		 * urmach_futex_waitv on every channel's prod_tail (#325), so a
		 * client's request wakes it within microseconds; the timeout
		 * only bounds how long an idle reactor waits before it
		 * re-checks the control port.  When no clients are connected,
		 * block on the control port instead so we don't busy-spin an
		 * empty pollset. */
		if (nconns > 0) {
			n = flipc2_poll(ps, events, FLIPC_REACTOR_MAX_CONNS,
					FLIPC_POLL_TIMEOUT_MS);
			for (i = 0; i < n; i++) {
				flipc2_channel_t rev = events[i].channel;
				flipc2_channel_t fwd = 0;
				for (j = 0; j < nconns; j++)
					if (conns[j].rev == rev) {
						fwd = conns[j].fwd;
						break;
					}
				if (fwd)
					while (flipc_serve_one(mnt, fwd, rev))
						;
			}
		}

		/* Control plane: connect / dead-name.  Non-blocking while
		 * clients exist (poll above did the waiting); blocking only
		 * when idle with no clients. */
		if (flipc2_endpoint_pump(ep,
					 nconns > 0 ? 0 : FLIPC_POLL_TIMEOUT_MS,
					 &ev) != FLIPC2_SUCCESS)
			continue;

		if (ev.type == FLIPC2_EP_EVENT_NEW) {
			if (nconns < FLIPC_REACTOR_MAX_CONNS) {
				conns[nconns].fwd = ev.fwd_ch;
				conns[nconns].rev = ev.rev_ch;
				nconns++;
				flipc2_pollset_add(ps, ev.rev_ch);
			}
			/* The client may have posted its first request before
			 * we joined it to the pollset — drain it now. */
			while (flipc_serve_one(mnt, ev.fwd_ch, ev.rev_ch))
				;
		} else if (ev.type == FLIPC2_EP_EVENT_DEAD) {
			flipc2_pollset_remove(ps, ev.rev_ch);
			for (j = 0; j < nconns; j++)
				if (conns[j].rev == ev.rev_ch) {
					conns[j] = conns[--nconns];
					break;
				}
			flipc2_channel_destroy(ev.fwd_ch);
			flipc2_channel_destroy(ev.rev_ch);
		}
	}
	return NULL;
}

/*
 * Create the FLIPC endpoint for one mount and start its reactor thread.
 * On any failure the mount simply has no fast-path (flipc_ep stays "").
 */
static void
flipc_start_mount(struct mount_context *mnt)
{
	flipc2_endpoint_t ep;
	flipc2_return_t ret;
	static int slot;

	(void)snprintf(mnt->flipc_ep, sizeof(mnt->flipc_ep),
		       "%s%s", VFS_FLIPC_ENDPOINT_PREFIX, mnt->service_name);

	ret = flipc2_endpoint_create(mnt->flipc_ep, 4,
				     VFS_FLIPC_CHANNEL_SIZE,
				     VFS_FLIPC_RING_ENTRIES, 0, &ep);
	if (ret != FLIPC2_SUCCESS) {
		printf("ext2: FLIPC endpoint_create(%s) failed %d\n",
		       mnt->flipc_ep, ret);
		mnt->flipc_ep[0] = '\0';
		return;
	}

	if (slot >= MAX_MOUNTS) {
		mnt->flipc_ep[0] = '\0';
		return;
	}
	flipc_reactor_args[slot].ep  = ep;
	flipc_reactor_args[slot].mnt = mnt;

	pthread_t t;
	if (pthread_create(&t, NULL, flipc_reactor,
			   &flipc_reactor_args[slot]) != 0) {
		printf("ext2: FLIPC reactor thread failed for %s\n",
		       mnt->flipc_ep);
		mnt->flipc_ep[0] = '\0';
		return;
	}
	slot++;
	pthread_detach(t);
	printf("ext2: FLIPC fast-path endpoint \"%s\" ready\n", mnt->flipc_ep);
}

/* ================================================================
 * Main entry point
 * ================================================================ */

int
main(int argc, char **argv)
{
	kern_return_t kr;
	int	      i;

	/*
	 * Unknown arguments are ignored rather than fatal: this server is
	 * started by bootstrap from a line in bootstrap.conf, and a server that
	 * refuses to boot over a flag it does not know is a worse failure than
	 * one that ignores it.
	 */
	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "-v") == 0)
			ext2_verbose = 1;

	kr = bootstrap_ports(bootstrap_port,
			     &host_port, &device_port,
			     &root_ledger_wired, &root_ledger_paged,
			     &security_port);
	if (kr != KERN_SUCCESS)
		_exit(1);

	printf_init(device_port);
	panic_init(host_port);

	/* #199 prep: mirror printf to gpu_server's text plane.  No-op
	 * if gpu_server isn't reachable; serial console keeps working. */
	(void)gpu_console_init("ext");

	printf("\n=== ext2 filesystem server " EXT2_SERVER_VERSION_STRING " ===\n");

	/* Create port set for all mount ports */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_PORT_SET, &port_set);
	if (kr != KERN_SUCCESS) {
		printf("ext2: port set alloc failed\n");
		return 1;
	}

	/* #276: hand the same port set to libfspager so the memory_object
	 * ports it allocates for fs_mmap clients land in the same receive
	 * loop as fs_open/read/write below. */
	if (fs_pager_init("ext2", port_set) != 0) {
		printf("ext2: fs_pager_init failed\n");
		return 1;
	}

	/* Mount partitions.
	 * First partition (ahci0a) is mandatory — blocks until available.
	 * Additional partitions are optional — non-blocking lookup. */
	{
		static const struct {
			const char *driver;
			const char *service;
			const char *mount;	/* path prefix for libvfs (#220) */
		} part_table[] = {
			{ "ahci0a", "ext_server",   "/"           },
			{ "ahci0b", "ext_server:1", "/mnt/disk1"  },
			{ "ahci1a", "ext_server:2", "/mnt/disk2"  },
			{ "ahci1b", "ext_server:3", "/mnt/disk3"  },
		};
		int i;

		for (i = 0; i < MAX_MOUNTS; i++) {
			int rc = mount_partition(&mounts[i],
				part_table[i].driver,
				part_table[i].service,
				part_table[i].mount,
				/*blocking=*/ (i == 0));
			if (rc < 0) {
				if (i == 0)
					return 1;	/* mandatory */
				continue;		/* optional */
			}
			n_mounts++;
		}

		printf("ext2: %d partition(s) mounted\n", n_mounts);
	}

	/* Start background writeback thread */
	{
		pthread_t wb_thread;
		pthread_create(&wb_thread, NULL,
			       (void *(*)(void *))writeback_thread, NULL);
		pthread_detach(wb_thread);
		printf("ext2: writeback thread started (%d ms interval)\n",
		       WRITEBACK_INTERVAL_MS);
	}

	/* #232: bring up a FLIPC v2 fast-path endpoint per active mount,
	 * before the Mach loop so fs_flipc_endpoint reports a ready name. */
	{
		int mi;
		for (mi = 0; mi < n_mounts; mi++)
			if (mounts[mi].active)
				flipc_start_mount(&mounts[mi]);
	}

	printf("ext2: ready, entering message loop\n");

	/* MIG server loop — receives from port set covering all mounts.
	 * Demux dispatches to ext2fs_server (subsystem 2920, legacy) and
	 * vfs (subsystem 3000, common interface for libvfs).
	 *
	 * ⚠️ MACH_RCV_TRAILER_SEQNO, because the third demux arm is
	 * libfspager's and its stubs are generated with -DSEQNOS: every
	 * seqnos_memory_object_* routine takes the sequence number as an
	 * argument, so the stub reads it out of the trailer and refuses the
	 * message when the trailer is too short to hold one.  The receive is
	 * the only place that can ask for it, and asking was left to
	 * MACH_MSG_OPTION_NONE here while default_pager -- same stubs, same
	 * flag -- asks for it in both of its loops.  The two halves were
	 * decided in different files and nothing compared them: file-backed
	 * mmap died at the first memory_object_init with "mig:
	 * memory_object_init refused a message: seqno" on the console.
	 */
	mach_msg_server(ext_server_demux, 8192, port_set,
			MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_SEQNO));

	return 0;
}
