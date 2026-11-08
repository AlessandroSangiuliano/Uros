/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The server of mount_test's own mount (#716).
 *
 * It answers libvfs as a server that is wrong would, on the files that ask
 * for it -- a read longer than was asked for, a write said to be longer than
 * was sent, a FLIPC read from outside the channel -- and as an honest one on
 * the others, which are mount_test's controls.  Each file is LIAR_SIZE bytes
 * of one letter, so the reader can tell which path answered: 'M' is the Mach
 * path, 'F' is FLIPC, 'X' is what only a read longer than asked could hold.
 *
 * ⚠️ liar_names.h first: the sixteen functions vfs.defs has a server call are
 * named liar_ here, because their real names are libvfs's API, which the other
 * half of this task calls.
 */

#include "liar_names.h"

#include <mach.h>
#include <mach/message.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <vfs_types.h>
#include <vfs_flipc.h>
#include <flipc2.h>

#include "vfs_server.h"
#include "mount_test.h"

volatile uint32_t liar_mach_reads[LIAR_FILES];
volatile uint32_t liar_flipc_reads[LIAR_FILES];

static mach_port_t		liar_port;
static flipc2_endpoint_t	liar_ep;
static char			liar_ep_name[64];

static const struct {
	const char	*name;
	int		handle;
} liar_files[] = {
	{ "honest",		LIAR_HONEST },
	{ "more",		LIAR_MORE },
	{ "write_more",		LIAR_WRITE_MORE },
	{ "flipc_honest",	LIAR_FLIPC_HONEST },
	{ "flipc_more",		LIAR_FLIPC_MORE },
	{ "flipc_outside",	LIAR_FLIPC_OUTSIDE },
};

static int
liar_handle_of(const char *path)
{
	const char	*base = strrchr(path, '/');
	unsigned int	i;

	base = base ? base + 1 : path;
	for (i = 0; i < sizeof liar_files / sizeof liar_files[0]; i++)
		if (strcmp(base, liar_files[i].name) == 0)
			return liar_files[i].handle;
	return 0;
}

static int
liar_valid(vfs_u64_t handle)
{
	return handle >= LIAR_HONEST && handle < LIAR_FILES;
}

/* How much of a LIAR_SIZE-byte file is left at `offset', up to `count'. */
static uint64_t
liar_left(vfs_u64_t offset, uint64_t count)
{
	uint64_t n = offset < LIAR_SIZE ? LIAR_SIZE - offset : 0;

	return n < count ? n : count;
}

kern_return_t
liar_open(mach_port_t fs_port, mach_port_t client_task, vfs_path_t path,
	  int flags, int mode, vfs_u64_t *handle, vfs_u32_t *type)
{
	int h = liar_handle_of(path);

	(void)fs_port; (void)flags; (void)mode;
	if (h == 0)
		return KERN_INVALID_ARGUMENT;	/* the request, task port and
						 * all, goes back with it */
	/* libvfs sends its task port with every open (#911); this server has
	 * no use for it. */
	(void)mach_port_deallocate(mach_task_self(), client_task);
	*handle = (vfs_u64_t)h;
	*type = VFS_FT_REG;
	return KERN_SUCCESS;
}

kern_return_t
liar_close(mach_port_t fs_port, vfs_u64_t handle)
{
	(void)fs_port; (void)handle;
	return KERN_SUCCESS;
}

kern_return_t
liar_read(mach_port_t fs_port, vfs_u64_t handle, vfs_u64_t offset,
	  vfs_u32_t count, vfs_buf_t *data, mach_msg_type_number_t *dataCnt)
{
	vm_address_t	buf = 0;
	uint64_t	n;

	(void)fs_port;
	*data = 0;
	*dataCnt = 0;
	if (!liar_valid(handle))
		return KERN_INVALID_ARGUMENT;
	__atomic_fetch_add(&liar_mach_reads[handle], 1, __ATOMIC_RELEASE);

	n = (handle == LIAR_MORE) ? (uint64_t)count + LIAR_MORE_BY
				  : liar_left(offset, count);
	if (n == 0)
		return KERN_SUCCESS;
	if (vm_allocate(mach_task_self(), &buf, (vm_size_t)n, TRUE)
	    != KERN_SUCCESS)
		return KERN_RESOURCE_SHORTAGE;
	memset((void *)buf, handle == LIAR_MORE ? 'X' : 'M', (size_t)n);
	*data = (vfs_buf_t)buf;		/* Dealloc: given back once sent */
	*dataCnt = (mach_msg_type_number_t)n;
	return KERN_SUCCESS;
}

kern_return_t
liar_write(mach_port_t fs_port, vfs_u64_t handle, vfs_u64_t offset,
	   vfs_buf_t data, mach_msg_type_number_t dataCnt, vfs_u32_t *written)
{
	(void)fs_port; (void)offset;
	/* The data came out of line and is this task's: given back here, on a
	 * success -- on an error mach_msg_server() destroys the request. */
	if (dataCnt > 0)
		(void)vm_deallocate(mach_task_self(), (vm_address_t)data,
				    dataCnt);
	*written = (handle == LIAR_WRITE_MORE) ? dataCnt + 1 : dataCnt;
	return KERN_SUCCESS;
}

kern_return_t
liar_flipc_endpoint(mach_port_t fs_port, vfs_path_t endpoint, int *result)
{
	(void)fs_port;
	if (liar_ep_name[0] == '\0') {
		endpoint[0] = '\0';
		*result = -1;
		return KERN_SUCCESS;
	}
	(void)strncpy(endpoint, liar_ep_name, VFS_PATH_MAX - 1);
	endpoint[VFS_PATH_MAX - 1] = '\0';
	*result = 0;
	return KERN_SUCCESS;
}

/* What mount_test never asks of its mount. */
kern_return_t
liar_truncate(mach_port_t fs_port, vfs_u64_t handle, vfs_u64_t length)
{
	(void)fs_port; (void)handle; (void)length;
	return KERN_FAILURE;
}

kern_return_t
liar_stat(mach_port_t fs_port, vfs_path_t path, vfs_stat_t *st)
{
	(void)fs_port; (void)path; (void)st;
	return KERN_FAILURE;
}

kern_return_t
liar_fstat(mach_port_t fs_port, vfs_u64_t handle, vfs_stat_t *st)
{
	(void)fs_port; (void)handle; (void)st;
	return KERN_FAILURE;
}

kern_return_t
liar_readdir(mach_port_t fs_port, vfs_u64_t dir_handle, vfs_u64_t cookie,
	     vfs_dirent_array_t *entries, mach_msg_type_number_t *entriesCnt,
	     vfs_u64_t *next_cookie)
{
	(void)fs_port; (void)dir_handle; (void)cookie;
	*entries = 0;
	*entriesCnt = 0;
	*next_cookie = 0;
	return KERN_FAILURE;
}

kern_return_t
liar_unlink(mach_port_t fs_port, vfs_path_t path)
{
	(void)fs_port; (void)path;
	return KERN_FAILURE;
}

kern_return_t
liar_mkdir(mach_port_t fs_port, vfs_path_t path, int mode)
{
	(void)fs_port; (void)path; (void)mode;
	return KERN_FAILURE;
}

kern_return_t
liar_rmdir(mach_port_t fs_port, vfs_path_t path)
{
	(void)fs_port; (void)path;
	return KERN_FAILURE;
}

kern_return_t
liar_rename(mach_port_t fs_port, vfs_path_t old_path, vfs_path_t new_path)
{
	(void)fs_port; (void)old_path; (void)new_path;
	return KERN_FAILURE;
}

kern_return_t
liar_sync(mach_port_t fs_port)
{
	(void)fs_port;
	return KERN_SUCCESS;
}

kern_return_t
liar_mmap(mach_port_t fs_port, vfs_u64_t handle, vfs_u32_t prot,
	  vfs_u32_t flags, mach_port_t *mem_obj)
{
	(void)fs_port; (void)handle; (void)prot; (void)flags;
	*mem_obj = MACH_PORT_NULL;
	return KERN_FAILURE;
}

kern_return_t
liar_unmount(mach_port_t fs_port)
{
	(void)fs_port;
	return KERN_FAILURE;
}

static void *
liar_serve(void *arg)
{
	(void)arg;
	(void)mach_msg_server(vfs_server, 8192, liar_port,
			      MACH_MSG_OPTION_NONE);
	printf("mount_test: the liar's message loop ended\n");
	return NULL;
}

const char *
liar_start(mach_port_t port)
{
	pthread_t t;

	liar_port = port;
	if (pthread_create(&t, NULL, liar_serve, NULL) != 0)
		return "pthread_create failed";
	(void)pthread_detach(t);
	return NULL;
}

/*
 * One FLIPC read, answered on `rep' as the file `handle' asks.
 *
 * ⚠️ LIAR_FLIPC_MORE answers from inside the region and longer than asked,
 * and LIAR_FLIPC_OUTSIDE answers a length that fits from an offset that wraps
 * to just before the region: each breaks one of the two bounds libvfs puts on
 * a reply, and only that one.
 */
static void
liar_flipc_read(flipc2_channel_t fwd, uint64_t handle, uint64_t offset,
		uint64_t want, struct flipc2_desc *rep)
{
	uint8_t		*data = flipc2_data_ptr(fwd, 0);
	uint64_t	room = fwd->hdr->data_size;
	uint64_t	n;

	if (!liar_valid(handle)) {
		rep->status = VFS_FLIPC_ERR_BADHANDLE;
		return;
	}
	__atomic_fetch_add(&liar_flipc_reads[handle], 1, __ATOMIC_RELEASE);

	switch (handle) {
	case LIAR_FLIPC_MORE:
		n = want + LIAR_FLIPC_MORE_BY;
		if (n > room)
			n = room;
		rep->data_offset = 0;
		break;
	case LIAR_FLIPC_OUTSIDE:
		n = LIAR_SIZE;
		rep->data_offset = (uint64_t)0 - 8;
		break;
	default:
		n = liar_left(offset, want);
		rep->data_offset = 0;
		break;
	}
	memset(data, 'F', (size_t)(n < room ? n : room));
	rep->data_length = n;
	rep->status = VFS_FLIPC_OK;
}

static void *
liar_flipc(void *arg)
{
	flipc2_channel_t	fwd, rev;
	struct flipc2_desc	*req, *rep;

	(void)arg;
	if (flipc2_endpoint_accept(liar_ep, &fwd, &rev) != FLIPC2_SUCCESS) {
		printf("mount_test: the liar's FLIPC accept failed\n");
		return NULL;
	}
	for (;;) {
		uint32_t	op;
		uint64_t	cookie, handle, offset, want;

		req = flipc2_consume_wait(rev, FLIPC2_SPIN_DEFAULT);
		if (req == NULL)
			break;
		op     = req->opcode;
		cookie = req->cookie;
		handle = req->param[1];
		offset = req->param[2];
		want   = req->data_length;
		flipc2_consume_release(rev);

		rep = flipc2_produce_wait(fwd, FLIPC2_SPIN_DEFAULT);
		if (rep == NULL)
			break;
		rep->opcode      = op;
		rep->cookie      = cookie;
		rep->flags       = 0;
		rep->data_offset = 0;
		rep->data_length = 0;
		rep->status      = VFS_FLIPC_ERR_INVAL;
		if (op == VFS_FLIPC_OP_READ)
			liar_flipc_read(fwd, handle, offset, want, rep);
		else if (op == VFS_FLIPC_OP_WRITE) {
			rep->status      = VFS_FLIPC_OK;
			rep->data_length = want;
		}
		flipc2_produce_commit(fwd);
	}
	printf("mount_test: the liar's FLIPC channel ended\n");
	return NULL;
}

const char *
liar_flipc_start(void)
{
	pthread_t t;

	(void)snprintf(liar_ep_name, sizeof liar_ep_name, "%s%s",
		       VFS_FLIPC_ENDPOINT_PREFIX, "mount_test");
	if (flipc2_endpoint_create(liar_ep_name, 1, VFS_FLIPC_CHANNEL_SIZE,
				   VFS_FLIPC_RING_ENTRIES, 0, &liar_ep)
	    != FLIPC2_SUCCESS) {
		liar_ep_name[0] = '\0';
		return "flipc2_endpoint_create failed";
	}
	if (pthread_create(&t, NULL, liar_flipc, NULL) != 0) {
		liar_ep_name[0] = '\0';
		return "pthread_create failed";
	}
	(void)pthread_detach(t);
	return NULL;
}
