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
 * block_device.c — MIG device server stubs and readahead cache
 *
 * Implements the ds_device_* functions that the MIG-generated
 * device_server() demux calls.  Protected payload on each partition
 * port provides the struct blk_partition pointer.  All I/O is
 * dispatched through the controller's ops vtable.
 */

#include <mach.h>
#include <mach/mach_port.h>
#include <mach/cap_types.h>
#include <device/device.h>
#include <device/device_types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "block_server.h"
#include "device_master.h"	/* #432: map a client's page for my device */

/*
 * UrMach capability verify trap (slot 37).  Declared here to keep BDS
 * from having to pull in the full libcap public header — we only need
 * the fast-path syscall, not cap_server RPC.
 */
#include <mach/mach_traps.h>	/* the traps, declared once (#426) */

/* #537 step 3: the kept translations -- see blk_xlate_take() below. */
#ifndef BLK_ABLATE_537_NO_TABLE
#define BLK_ABLATE_537_NO_TABLE 0
#endif
#ifndef BLK_ABLATE_537_TABLE_KEPT
#define BLK_ABLATE_537_TABLE_KEPT 0
#endif
#define BLK_DMA_PAGE	4096u		/* the DMA regions' page, both targets */

struct blk_handle;
static void blk_xlate_free(struct blk_xlate *x);
static void blk_xlate_take(struct blk_handle *h, const struct uros_cap *t,
			   struct blk_xlate *x);

/*
 * Linked list of every live blk_handle, used by the no-senders
 * notification path to find the handle by Mach port name (the kernel
 * does NOT substitute the protected payload on notification messages,
 * so msgh_local_port is the receive-port name, not the payload).
 * Single-threaded BDS — no locking yet.
 */
static struct blk_handle *blk_handles_head = NULL;

/*
 * Resolve a device port to its underlying partition struct, but only
 * if the port is an authenticated handle.  Raw partition ports
 * (BLK_MAGIC_PART) — the ones published via netname for discovery —
 * are NOT considered authenticated for I/O: callers must first invoke
 * device_open_cap to obtain a handle.  Returns NULL on any other case.
 */
static struct blk_handle *
blk_authed_handle(mach_port_t device)
{
	if (device == 0)
		return NULL;
	uint32_t *magicp = blk_object_for(device);
	uint32_t magic = magicp ? *magicp : 0;
	if (magic != BLK_MAGIC_HANDLE)
		return NULL;
	struct blk_handle *h = blk_object_for(device);
	/*
	 * Issue #183: a handle whose cap was revoked stays linked in the
	 * list (so explicit close / no-senders can still find it) but
	 * fails the auth check here, so subsequent ds_device_read /
	 * ds_device_write return KERN_NO_ACCESS.
	 */
	if (h->revoked)
		return NULL;
	return h;
}

static struct blk_partition *
blk_part_from_authed_handle(mach_port_t device)
{
	struct blk_handle *h = blk_authed_handle(device);

	return h ? h->part : NULL;
}

/*
 * #599: a token that verifies AND names the kind of resource expected --
 * the trap takes the type since #599, so a check without one cannot be
 * written.
 */
static kern_return_t
blk_token_check(const struct uros_cap *t, uint32_t type, uint32_t op,
		uint64_t id)
{
	return urmach_cap_verify(t, type, op, id);
}

/*
 * Mark every handle whose cap_id matches as revoked.  Called from
 * blk_cap_revoke_notify in block_server.c when cap_server fans out a
 * revocation event.  Multiple handles can share a cap_id only across
 * different partitions today (one cap_request → one cap_id → one
 * device_open_cap), but the loop is general anyway since #183
 * envisages cascade revocation hitting child cap_ids.
 */
int
blk_handles_revoke_by_cap_id(uint64_t cap_id)
{
	int n = 0;
	for (struct blk_handle *h = blk_handles_head; h; h = h->next) {
		unsigned int i, kept;

		if (h->cap_id == cap_id && !h->revoked) {
			h->revoked = 1;
			n++;
			printf("blk: handle for %s revoked (cap %llu, port=0x%x)\n",
			       h->part ? h->part->name : "(unknown)",
			       (unsigned long long)cap_id,
			       (unsigned)h->recv_port);
		}

		/*
		 * #599: and a buffer capability of that id, on any handle.
		 * The kernel refuses it from the revocation on anyway (and the
		 * next transfer would forget it); forgetting it here says so
		 * when it happens, not at the next read.
		 */
		for (i = 0, kept = 0; i < h->n_dma_caps; i++) {
			if (h->dma_cap[i].cap_id == cap_id &&
			    !BLK_ABLATE_537_TABLE_KEPT) {
				printf("blk: %s: capability %llu forgotten — "
				       "cap_server says it was revoked\n",
				       h->part ? h->part->name : "(unknown)",
				       (unsigned long long)cap_id);
				blk_xlate_free(&h->dma_xlate[i]);
				h->dropped++;
				n++;
				continue;
			}
			h->dma_xlate[kept] = h->dma_xlate[i];
			h->dma_cap[kept++] = h->dma_cap[i];
		}
		for (i = kept; i < h->n_dma_caps; i++) {
			memset(&h->dma_cap[i], 0, sizeof(h->dma_cap[i]));
			memset(&h->dma_xlate[i], 0, sizeof(h->dma_xlate[i]));
		}
		h->n_dma_caps = kept;
		if (h->dma_last >= kept)
			h->dma_last = 0;
	}
	return n;
}

/* ================================================================
 * Readahead cache
 *
 * When a sequential read pattern is detected (current LBA ==
 * previous end LBA), we read extra sectors and cache them.
 * The next ds_device_read for the continuation is served from
 * the cache without hitting the disk.
 * ================================================================ */

static struct {
	uint32_t		lba_start;
	uint32_t		lba_count;
	vm_offset_t		buf;
	unsigned int		buf_size;
	struct blk_partition	*part;
} ra_cache;

static void
ra_invalidate(struct blk_partition *part)
{
	if (ra_cache.buf != 0 && ra_cache.part == part) {
		vm_deallocate(mach_task_self(),
			      ra_cache.buf, ra_cache.buf_size);
		ra_cache.buf = 0;
		ra_cache.lba_count = 0;
	}
}

/* ================================================================
 * ds_device_open / close
 * ================================================================ */

kern_return_t
ds_device_open(mach_port_t master, mach_port_t reply,
	       mach_msg_type_name_t reply_poly,
	       mach_port_t ledger, dev_mode_t mode,
	       security_token_t sec_token, dev_name_t name,
	       mach_port_t *device)
{
	struct blk_partition *part = blk_object_for(master);
	if (part && part->recv_port != MACH_PORT_NULL)
		*device = part->recv_port;
	else
		*device = MACH_PORT_NULL;
	return KERN_SUCCESS;
}

/*
 * Explicit per-handle close (Issue #182).
 *
 * Mirrors the no-senders cleanup but fires synchronously inside the
 * client's RPC, so a long-running client can release a partition
 * deterministically — e.g. before re-mounting — without having to
 * exit or fabricate a port-rights drop.
 *
 * Idempotent on non-handle ports: callers may safely invoke
 * device_close on the master ("discovery") partition port or on a
 * port whose underlying handle has already been reaped — both return
 * KERN_SUCCESS without touching state.  No-senders remains armed as
 * the safety net for clients that forget to close.
 */
/*
 * #599: the one end of a handle, for close and for no-senders alike.
 * Unlinks it, says so in the caller's word ("closed" or "released"),
 * poisons and frees it, and destroys its receive right.  Everything a handle
 * holds is let go here, so neither path can forget a part of it.
 */
static void blk_phys_said(const struct blk_handle *h, const char *when);

static void
blk_handle_destroy(struct blk_handle *h, const char *how)
{
	mach_port_t name = h->recv_port;
	struct blk_handle **pp = &blk_handles_head;

	while (*pp != NULL) {
		if (*pp == h) {
			*pp = h->next;
			break;
		}
		pp = &(*pp)->next;
	}

	printf("blk: handle for %s %s (cap %llu, port=0x%x) — %u buffer "
	       "capabilit%s dropped, %u physical request%s refused\n",
	       h->part ? h->part->name : "(unknown)", how,
	       (unsigned long long)h->cap_id,
	       (unsigned)name,
	       h->n_dma_caps + h->dropped,
	       h->n_dma_caps + h->dropped == 1 ? "y" : "ies",
	       h->refusals, h->refusals == 1 ? "" : "s");

	if (h->phys_req != 0)
		blk_phys_said(h, "in all,");
	for (unsigned int i = 0; i < h->n_dma_caps; i++)
		blk_xlate_free(&h->dma_xlate[i]);	/* #537 */
	memset(h->dma_cap, 0, sizeof(h->dma_cap));	/* #599 */
	h->magic = 0;        /* poison so a stray msg can't reuse it */
	blk_payload_release(h->payload);
	free(h);

	/*
	 * Destroying the receive right also tears down the armed
	 * MACH_NOTIFY_NO_SENDERS send-once we registered at open time,
	 * so no explicit cancellation is needed.
	 */
	(void)mach_port_destroy(mach_task_self(), name);
}

kern_return_t
ds_device_close(mach_port_t device)
{
	if (device == 0)
		return KERN_SUCCESS;

	uint32_t *magicp = blk_object_for(device);
	uint32_t magic = magicp ? *magicp : 0;
	if (magic != BLK_MAGIC_HANDLE)
		return KERN_SUCCESS;

	blk_handle_destroy(blk_object_for(device), "closed");
	return KERN_SUCCESS;
}

/* ================================================================
 * ds_device_open_cap — capability-gated open (Uros Issue #178)
 *
 * Per-(client, partition) authentication (Issue #181): the master
 * port published via netname is a "discovery" port that ds_device_*
 * I/O routines refuse.  device_open_cap verifies the token, allocates
 * a fresh receive port that BDS owns, attaches a struct blk_handle as
 * the protected payload, and hands the port to the caller.  Possession
 * of that port is the proof of authentication; raw I/O on the master
 * port stays rejected even after this call succeeds.
 *
 * Lifetime: the handle is reaped on whichever happens first —
 *   - explicit device_close from the client (Issue #182), or
 *   - MACH_NOTIFY_NO_SENDERS when the last send right disappears
 *     (Issue #181, kicks in when the client task exits or forgets
 *     to close).
 * ================================================================ */

kern_return_t
ds_device_open_cap(mach_port_t master, mach_port_t reply,
		   mach_msg_type_name_t reply_poly,
		   mach_port_t ledger, dev_mode_t mode,
		   security_token_t sec_token, dev_name_t name,
		   char *token_blob, mach_msg_type_number_t token_len,
		   mach_port_t *device)
{
	*device = MACH_PORT_NULL;

	struct blk_partition *part = blk_object_for(master);
	if (!part || part->magic != BLK_MAGIC_PART)
		return D_NO_SUCH_DEVICE;

	if (token_len != sizeof(struct uros_cap))
		return KERN_NO_ACCESS;

	struct uros_cap token;
	memcpy(&token, token_blob, sizeof(token));

	uint32_t op = CAP_OP_BLK_READ | CAP_OP_BLK_WRITE;

	/*
	 * Per Issue #184 a partition is reachable under two names: the
	 * driver-specific one ("ahci0a") and a driver-agnostic alias
	 * ("disk0a").  The cap_id is hashed from whichever name the
	 * client looked up, so try both.
	 */
	uint64_t resource_id = cap_name_hash(part->name);
	kern_return_t kr = blk_token_check(&token, RESOURCE_BLK_DEVICE, op,
					   resource_id);	/* #599: typed */
	if (kr != KERN_SUCCESS && part->stable_name[0] != '\0') {
		uint64_t alias_id = cap_name_hash(part->stable_name);
		kr = blk_token_check(&token, RESOURCE_BLK_DEVICE, op, alias_id);
		if (kr == KERN_SUCCESS)
			resource_id = alias_id;
	}
	if (kr != KERN_SUCCESS) {
		/*
		 * ⚠️ "refused", not "FAIL".  Turning away a token that does not
		 * verify is this routine working, and cap_test's negative test
		 * [2] provokes this very line on every i386 boot by opening
		 * ahci0a with a zero-filled token -- the console reads
		 * "cap_test: [2] ... zero-token rejected: OK" just after it.
		 * Spelled FAIL it cost a reader an afternoon of chasing a
		 * defect that was a passing test, the same way `grep -c panic'
		 * counts successes.  A denial that reads like a malfunction
		 * gets investigated; one that reads like a denial gets read.
		 */
		printf("blk: refused %s: token does not grant op=0x%x on "
		       "id=0x%llx (kr=%d)\n",
		       part->name, op, (unsigned long long)resource_id, kr);
		return KERN_NO_ACCESS;
	}

	struct blk_handle *h = (struct blk_handle *)malloc(sizeof(*h));
	if (h == NULL)
		return KERN_RESOURCE_SHORTAGE;
	memset(h, 0, sizeof(*h));		/* #599: no capability yet */
	h->magic     = BLK_MAGIC_HANDLE;
	h->part      = part;
	h->cap_id    = token.cap_id;
	h->revoked   = 0;

	mach_port_t hport = MACH_PORT_NULL;
	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &hport);
	if (kr != KERN_SUCCESS) {
		free(h);
		return kr;
	}
	h->payload = blk_payload_register(h);
	if (h->payload == 0) {
		(void)mach_port_mod_refs(mach_task_self(), hport,
					 MACH_PORT_RIGHT_RECEIVE, -1);
		free(h);
		return KERN_RESOURCE_SHORTAGE;
	}
	(void)mach_port_set_protected_payload(mach_task_self(),
					      hport, h->payload);
	(void)mach_port_move_member(mach_task_self(), hport, port_set);
	h->recv_port = hport;
	h->next = blk_handles_head;
	blk_handles_head = h;

	/*
	 * No-senders notification: when the client drops the last copy
	 * of the send right MIG is about to mint for the reply, the
	 * kernel posts a notification message to this same port; the
	 * blk_demux dispatcher sees it (msgh_id == MACH_NOTIFY_NO_SENDERS)
	 * and tears down the handle.  sync=1 delays arming until that
	 * first send right is created (the one MIG returns to the
	 * client), so we don't fire on our own intermediate state.
	 */
	mach_port_t prev_notify = MACH_PORT_NULL;
	kern_return_t nkr = mach_port_request_notification(mach_task_self(),
					     hport,
					     MACH_NOTIFY_NO_SENDERS,
					     1,
					     hport,
					     MACH_MSG_TYPE_MAKE_SEND_ONCE,
					     &prev_notify);
	if (nkr != KERN_SUCCESS) {
		printf("blk: request_notification(NO_SENDERS) on %s failed: %d\n",
		       part->name, nkr);
		/* The handle still works for I/O — we just won't reap it. */
	}

	printf("blk: validated cap %llu op=0x%x for %s -> handle port=0x%x\n",
	       (unsigned long long)token.cap_id, op, part->name,
	       (unsigned)hport);

	*device = hport;
	return KERN_SUCCESS;
}

/* ================================================================
 * blk_handle_no_senders — release a per-client handle.
 *
 * Called from blk_demux when a MACH_NOTIFY_NO_SENDERS message lands
 * on a handle port (i.e. the last client send right just disappeared,
 * usually because the task exited or explicitly dropped it).  Frees
 * the struct blk_handle and destroys the receive right; the next
 * device_open_cap from the same or any other task allocates a fresh
 * one.
 * ================================================================ */

boolean_t
blk_handle_no_senders(mach_msg_header_t *in, mach_msg_header_t *out)
{
	if ((in->msgh_bits != MACH_MSGH_BITS(0, MACH_MSG_TYPE_PORT_SEND_ONCE))
	    || (in->msgh_id != MACH_NOTIFY_NO_SENDERS))
		return FALSE;

	mach_port_t name = in->msgh_local_port;
	struct blk_handle *h;

	for (h = blk_handles_head; h != NULL; h = h->next)
		if (h->recv_port == name)
			break;

	if (h != NULL)
		blk_handle_destroy(h, "released");
	else {
		printf("blk: no-senders for unknown port 0x%x — ignored\n",
		       (unsigned)name);
	}

	out->msgh_remote_port = MACH_PORT_NULL;
	return TRUE;
}

/* ================================================================
 * ds_device_read — main read path with readahead cache
 * ================================================================ */

kern_return_t
ds_device_read(mach_port_t device, mach_port_t reply,
	       mach_msg_type_name_t reply_poly,
	       dev_mode_t mode, recnum_t recnum,
	       io_buf_len_t bytes_wanted,
	       io_buf_ptr_t *data, mach_msg_type_number_t *data_count)
{
	struct blk_partition *part = blk_part_from_authed_handle(device);
	if (!part)
		return KERN_NO_ACCESS;
	struct blk_controller *ctrl = part->ctrl;
	kern_return_t kr;
	vm_offset_t buf, read_buf;
	unsigned int total, nsectors, lba;
	unsigned int read_buf_size;
	unsigned int max_xfer;

	if (bytes_wanted <= 0)
		return D_INVALID_SIZE;

	total = (unsigned int)bytes_wanted;
	if (total % SECTOR_SIZE)
		total = (total + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
	nsectors = total / SECTOR_SIZE;

	if (recnum + nsectors > part->num_sectors)
		return D_INVALID_SIZE;

	lba = part->start_lba + recnum;

	kr = vm_allocate(mach_task_self(), &buf, total, TRUE);
	if (kr != KERN_SUCCESS)
		return D_NO_MEMORY;

	/* Check readahead cache */
	if (ra_cache.buf != 0 &&
	    ra_cache.part == part &&
	    lba >= ra_cache.lba_start &&
	    lba + nsectors <= ra_cache.lba_start + ra_cache.lba_count) {
		unsigned int cache_off =
			(lba - ra_cache.lba_start) * SECTOR_SIZE;
		memcpy((void *)buf,
		       (void *)(ra_cache.buf + cache_off), total);
		*data = (io_buf_ptr_t)buf;
		*data_count = (mach_msg_type_number_t)bytes_wanted;
		return KERN_SUCCESS;
	}

	/*
	 * Cache miss — read from disk.
	 * Determine readahead: if sequential and small, prefetch extra.
	 */
	max_xfer = ctrl->disks[part->disk_index].max_transfer_bytes;
	if (max_xfer == 0)
		max_xfer = 128u * 1024u;

	{
		unsigned int ra_max_sectors = max_xfer / SECTOR_SIZE;
		unsigned int read_sects = nsectors;
		unsigned int ra_extra = 0;
		uint32_t part_end = part->start_lba + part->num_sectors;
		unsigned int offset, chunk;

		/* Sequential detection: prefetch up to ra_max_sectors.
		 * Gate on a recorded prior position (lba_count != 0), NOT on a
		 * prior readahead buffer.  buf is set only *after* a readahead
		 * runs, so requiring buf != 0 here was chicken-and-egg: the
		 * readahead never bootstrapped and every sequential read fell
		 * through to disk.  The position (lba_start/lba_count/part) is
		 * recorded on every read, with or without a buffer, so this now
		 * fires on the 2nd sequential read. */
		if (ra_cache.lba_count != 0 &&
		    ra_cache.part == part &&
		    lba == ra_cache.lba_start + ra_cache.lba_count &&
		    nsectors <= ra_max_sectors / 2 &&
		    lba + ra_max_sectors <= part_end) {
			read_sects = ra_max_sectors;
			ra_extra = read_sects - nsectors;
		}

		unsigned int read_bytes = read_sects * SECTOR_SIZE;
		unsigned int ra_buf_needed = ra_extra * SECTOR_SIZE;
		vm_offset_t ra_buf = 0;

		if (ra_extra > 0) {
			kr = vm_allocate(mach_task_self(), &ra_buf,
					 ra_buf_needed, TRUE);
			if (kr != KERN_SUCCESS) {
				read_sects = nsectors;
				read_bytes = total;
				ra_extra = 0;
			}
		}

		/* Read in chunks of max_xfer */
		for (offset = 0; offset < read_bytes; offset += chunk) {
			unsigned int batch_sects;

			chunk = read_bytes - offset;
			if (chunk > max_xfer)
				chunk = max_xfer;
			batch_sects = chunk / SECTOR_SIZE;

			if (ctrl->ops->read_sectors(ctrl->priv,
						    part->disk_index,
						    lba + offset / SECTOR_SIZE,
						    batch_sects,
						    &read_buf,
						    &read_buf_size) < 0) {
				vm_deallocate(mach_task_self(), buf, total);
				if (ra_buf)
					vm_deallocate(mach_task_self(),
						      ra_buf, ra_buf_needed);
				return D_IO_ERROR;
			}

			/* Copy: first 'total' bytes → client, rest → RA */
			if (offset < total) {
				unsigned int client_chunk = total - offset;
				if (client_chunk > chunk)
					client_chunk = chunk;
				memcpy((void *)(buf + offset),
				       (void *)read_buf, client_chunk);
				if (client_chunk < chunk && ra_buf) {
					memcpy((void *)ra_buf,
					       (void *)(read_buf + client_chunk),
					       chunk - client_chunk);
				}
			} else if (ra_buf) {
				unsigned int ra_off = offset - total;
				memcpy((void *)(ra_buf + ra_off),
				       (void *)read_buf, chunk);
			}

			vm_deallocate(mach_task_self(),
				      read_buf, read_buf_size);
		}

		/* Update readahead cache */
		if (ra_cache.buf != 0)
			vm_deallocate(mach_task_self(),
				      ra_cache.buf, ra_cache.buf_size);

		if (ra_extra > 0 && ra_buf != 0) {
			ra_cache.lba_start = lba + nsectors;
			ra_cache.lba_count = ra_extra;
			ra_cache.buf       = ra_buf;
			ra_cache.buf_size  = ra_buf_needed;
			ra_cache.part      = part;
		} else {
			ra_cache.lba_start = lba;
			ra_cache.lba_count = nsectors;
			ra_cache.buf       = 0;
			ra_cache.buf_size  = 0;
			ra_cache.part      = part;
		}
	}

	*data = (io_buf_ptr_t)buf;
	*data_count = (mach_msg_type_number_t)bytes_wanted;
	return KERN_SUCCESS;
}

/* ================================================================
 * ds_device_write
 * ================================================================ */

kern_return_t
ds_device_write(mach_port_t device, mach_port_t reply,
		mach_msg_type_name_t reply_poly,
		dev_mode_t mode, recnum_t recnum,
		io_buf_ptr_t data, mach_msg_type_number_t data_count,
		io_buf_len_t *bytes_written)
{
	struct blk_partition *part = blk_part_from_authed_handle(device);
	if (!part) {
		vm_deallocate(mach_task_self(), (vm_offset_t)data, data_count);
		return KERN_NO_ACCESS;
	}
	struct blk_controller *ctrl = part->ctrl;
	unsigned int total, nsectors, lba;
	unsigned int offset, chunk, max_xfer;

	if (data_count <= 0)
		return D_INVALID_SIZE;

	total = (unsigned int)data_count;
	if (total % SECTOR_SIZE)
		total = (total + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
	nsectors = total / SECTOR_SIZE;

	if (recnum + nsectors > part->num_sectors) {
		vm_deallocate(mach_task_self(), (vm_offset_t)data, data_count);
		return D_INVALID_SIZE;
	}

	ra_invalidate(part);

	lba = part->start_lba + recnum;
	max_xfer = ctrl->disks[part->disk_index].max_transfer_bytes;
	if (max_xfer == 0)
		max_xfer = 128u * 1024u;

	for (offset = 0; offset < total; offset += chunk) {
		unsigned int batch_sects;

		chunk = total - offset;
		if (chunk > max_xfer)
			chunk = max_xfer;
		batch_sects = chunk / SECTOR_SIZE;

		if (ctrl->ops->write_sectors(ctrl->priv,
					     part->disk_index,
					     lba + offset / SECTOR_SIZE,
					     batch_sects,
					     (vm_offset_t)data + offset,
					     chunk) < 0) {
			vm_deallocate(mach_task_self(),
				      (vm_offset_t)data, data_count);
			return D_IO_ERROR;
		}
	}

	vm_deallocate(mach_task_self(), (vm_offset_t)data, data_count);
	*bytes_written = (io_buf_len_t)data_count;
	return KERN_SUCCESS;
}

/* ================================================================
 * ds_device_write_batch — multiple non-contiguous blocks
 * ================================================================ */

kern_return_t
ds_device_write_batch(mach_port_t device, mach_port_t reply,
		      mach_msg_type_name_t reply_poly,
		      dev_mode_t mode,
		      recnum_t *recnums,
		      mach_msg_type_number_t recnumsCnt,
		      unsigned int *sizes,
		      mach_msg_type_number_t sizesCnt,
		      io_buf_ptr_t data,
		      mach_msg_type_number_t data_count,
		      io_buf_len_t *bytes_written)
{
	struct blk_partition *part = blk_part_from_authed_handle(device);
	if (!part) {
		vm_deallocate(mach_task_self(), (vm_offset_t)data, data_count);
		return KERN_NO_ACCESS;
	}
	struct blk_controller *ctrl = part->ctrl;

	if (recnumsCnt != sizesCnt || recnumsCnt == 0)
		return D_INVALID_SIZE;

	ra_invalidate(part);

	/* If module supports batch write, delegate */
	if (ctrl->ops->write_batch) {
		uint32_t lbas[16];
		unsigned int i;

		for (i = 0; i < recnumsCnt && i < 16; i++)
			lbas[i] = part->start_lba + recnums[i];

		if (ctrl->ops->write_batch(ctrl->priv,
					   part->disk_index,
					   lbas, sizes, recnumsCnt,
					   (vm_offset_t)data,
					   data_count) < 0) {
			vm_deallocate(mach_task_self(),
				      (vm_offset_t)data, data_count);
			return D_IO_ERROR;
		}

		/* Sum sizes for bytes_written */
		{
			unsigned int tw = 0;
			for (i = 0; i < recnumsCnt; i++)
				tw += sizes[i];
			*bytes_written = (io_buf_len_t)tw;
		}
		vm_deallocate(mach_task_self(),
			      (vm_offset_t)data, data_count);
		return KERN_SUCCESS;
	}

	/* Fallback: write each block individually */
	{
		unsigned int i, data_off = 0, total_written = 0;

		for (i = 0; i < recnumsCnt; i++) {
			unsigned int sz = sizes[i];
			unsigned int wr_total, lba, off, chunk;
			unsigned int max_xfer;

			if (sz == 0)
				continue;
			if (data_off + sz > data_count) {
				vm_deallocate(mach_task_self(),
					      (vm_offset_t)data, data_count);
				return D_INVALID_SIZE;
			}

			wr_total = (sz + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
			lba = part->start_lba + recnums[i];
			max_xfer = ctrl->disks[part->disk_index].max_transfer_bytes;
			if (max_xfer == 0)
				max_xfer = 128u * 1024u;

			for (off = 0; off < wr_total; off += chunk) {
				unsigned int sects;
				chunk = wr_total - off;
				if (chunk > max_xfer)
					chunk = max_xfer;
				sects = chunk / SECTOR_SIZE;

				if (ctrl->ops->write_sectors(
					ctrl->priv, part->disk_index,
					lba + off / SECTOR_SIZE,
					sects,
					(vm_offset_t)data + data_off + off,
					chunk) < 0) {
					vm_deallocate(mach_task_self(),
						      (vm_offset_t)data,
						      data_count);
					return D_IO_ERROR;
				}
			}
			data_off += sz;
			total_written += sz;
		}

		vm_deallocate(mach_task_self(),
			      (vm_offset_t)data, data_count);
		*bytes_written = (io_buf_len_t)total_written;
		return KERN_SUCCESS;
	}
}

/* ================================================================
 * Zero-copy physical DMA read/write
 * ================================================================ */

/* ================================================================
 * A client's pages, made reachable by THIS server's device (#432, #599)
 * ================================================================ */

/*
 * 🔴 THE HOLE THIS CLOSES IS THE ONE #432 NAMED `DEVICE_DMA_NO_BDF'.
 *
 * ext_server allocates a scatter-gather page cache and passes the PHYSICAL
 * addresses of those pages on every device_read_phys -- because it owns no
 * device and has no bus/device/function to name.  Once this server's disk is
 * confined to what it was granted, those pages are in nobody's domain.  The
 * SERVER WHICH OWNS THE DEVICE does the mapping, and that server is this one.
 *
 * 🔴 #599: ON THE CLIENT'S WORD, PER HANDLE, AND ASKED EVERY TIME.
 *
 *  - A client hands the capability for its buffer to the HANDLE it reads
 *    through (device_register_dma).  It is tried for that handle's
 *    transfers and nobody else's.  They were kept per controller and tried
 *    for any client, so once ext_server registered, any task with a handle
 *    on that controller could have had the disk read or write its page
 *    cache.
 *  - Every page of every physical transfer is asked of the kernel
 *    (device_dma_map_foreign_op), for the direction the transfer needs.
 *    This server keeps no translation, so it has none that can outlive the
 *    buffer it came from or the capability behind it: the cache it kept
 *    was never invalidated, and served a freed region's address to any
 *    client without asking.
 *  - A page no capability covers is refused BEFORE any DMA, and the
 *    client is told so, on every handle.  The old path sent the
 *    untranslated address, the engine refused the transfer, the controller
 *    reported success, and the client was answered with whatever the page
 *    held.  Until every client handed its buffers over, a handle that had
 *    never registered one passed its addresses through; none does any
 *    more, on either target, and the exception is gone.
 */

kern_return_t
ds_device_register_dma(mach_port_t device, mach_port_t reply,
		       mach_msg_type_name_t reply_poly,
		       char *token, mach_msg_type_number_t tokenCnt)
{
	struct blk_handle	*h = blk_authed_handle(device);
	struct uros_cap		t;
	kern_return_t		kr;
	unsigned int		i;

	(void)reply;
	(void)reply_poly;

	if (h == NULL || h->part == NULL || h->part->ctrl == NULL)
		return KERN_NO_ACCESS;
	if (h->part->ctrl->ops->read_sectors_phys == NULL &&
	    h->part->ctrl->ops->write_sectors_phys == NULL)
		return D_INVALID_OPERATION;
	if (tokenCnt != sizeof(struct uros_cap))
		return KERN_INVALID_ARGUMENT;

	memcpy(&t, token, sizeof(t));
	kr = blk_token_check(&t, RESOURCE_DMA_BUFFER, CAP_OP_DMA_DEVICE_READ,
			     t.resource_id);
	if (kr != KERN_SUCCESS)
		kr = blk_token_check(&t, RESOURCE_DMA_BUFFER,
				     CAP_OP_DMA_DEVICE_WRITE, t.resource_id);
	if (kr != KERN_SUCCESS) {
		printf("blk: %s: a buffer capability was refused (kr=%d) — "
		       "it is not a DMA buffer's, or does not verify\n",
		       h->part->name, (int)kr);
		return kr;
	}

	for (i = 0; i < h->n_dma_caps; i++)
		if (h->dma_cap[i].cap_id == t.cap_id)
			break;
	if (i == h->n_dma_caps) {
		if (h->n_dma_caps == BLK_HANDLE_DMA_CAPS)
			return KERN_NO_SPACE;
		h->n_dma_caps++;
	}
	h->dma_cap[i] = t;
	blk_xlate_take(h, &t, &h->dma_xlate[i]);	/* #537 step 3 */

	printf("blk: a client handed %s a capability for buffer %llu "
	       "(%u on this handle)\n", h->part->name,
	       (unsigned long long)t.resource_id, h->n_dma_caps);
	return KERN_SUCCESS;
}

/*
 * ── The two arguments that have to agree (#532) ──────────────────────────
 *
 * A physical-DMA request carries a list of pages and a count of bytes, and
 * they arrive as separate arguments from a caller this server does not
 * trust.  Every bound the two entry points checked was about the disk or
 * about the local array; none was about the arithmetic that decides whether
 * the transfer can happen at all.
 *
 * 🔑 This is the place to check it because it is where the two arguments
 * first meet.  Both drivers refuse a short list of their own now, but a
 * driver that forgets to is then only as wrong as its own transfer, and the
 * next driver written starts from zero again.
 */
#define BLK_PHYS_PAGES_MAX	32

static int
blk_pages_cover(mach_msg_type_number_t npages, unsigned int total,
		const char *what)
{
	if ((uint64_t)npages * 4096u >= (uint64_t)total)
		return 1;

	printf("blk: %s of %u bytes refused — %u page(s) hold %llu\n",
	       what, total, (unsigned)npages,
	       (unsigned long long)((uint64_t)npages * 4096u));
	return 0;
}

static unsigned long long blk_tsc(void);

/*
 * #599: the cost of asking.  Every page of a physical transfer is asked of
 * the kernel (device_dma_map_foreign_op): one RPC and one MAC a page.  This
 * keeps the cycles spent asking against the cycles of the transfer, per
 * handle, and says them at powers of two from 1024 pages and when the
 * handle ends -- the number that decides whether the kernel needs a faster
 * path for the same answer.  Pages and not requests (#599): the asking is
 * per page, and ext_server's reads are many pages a request, so a mount
 * that read 16 MB had made 256 requests and never said anything -- and a
 * mount's handle does not end.
 */
static void
blk_phys_said(const struct blk_handle *h, const char *when)
{
	uint64_t per_page = h->phys_pages ? h->xlate_cyc / h->phys_pages : 0;
	uint64_t per_req = h->phys_req ? h->xfer_cyc / h->phys_req : 0;
	uint64_t all = h->xlate_cyc + h->xfer_cyc;
	unsigned permille = all ? (unsigned)((h->xlate_cyc * 1000) / all) : 0;

	cap_u64_t asks, claim, mac, find, rest, grants, grant_cyc;

	printf("blk: %s: %s %llu physical requests, %llu pages: asking the "
	       "kernel %llu cycles a page, the transfer %llu a request — "
	       "asking is %u.%u%% of it\n", h->part ? h->part->name : "?",
	       when, (unsigned long long)h->phys_req,
	       (unsigned long long)h->phys_pages, (unsigned long long)per_page,
	       (unsigned long long)per_req, permille / 10, permille % 10);

	/*
	 * #537: the kernel's part of each ask, split, read here and not printed
	 * by the kernel: this runs after the transfer, outside the window the
	 * asks are timed in, and a line the kernel printed inside one was
	 * counted as the ask's.  Its sums are over every ask since boot, this
	 * handle's and any other's, so the RPC is the difference of two
	 * averages and can come out a little either way.
	 *
	 * ⚠️ A line of its own: a print longer than 256 bytes leaves in two
	 * writes, and another task's output can land between them -- the one
	 * line this first was, about 290 bytes, was cut at byte 256 in 8 of 18.
	 */
	if (device_dma_ask_cost(master_device, &asks, &claim, &mac, &find,
				&rest, &grants, &grant_cyc) == KERN_SUCCESS &&
	    asks != 0) {
		uint64_t in_kernel = (claim + mac + find + rest) / asks;
		char rpc[48];

		/*
		 * The asks that granted are apart in the kernel's sums and inside
		 * this handle's count -- its own first ask granted -- and the
		 * kernel's grants are not told apart by handle.  So the RPC is the
		 * difference only on a boot with no grant; with an IOMMU it is the
		 * one a boot without measured, the path being the same.
		 */
		if (grants == 0)
			snprintf(rpc, sizeof(rpc), "the RPC about %lld",
				 (long long)per_page - (long long)in_kernel);
		else
			snprintf(rpc, sizeof(rpc), "the RPC not separable");

		printf("blk: %s: in the kernel, over all %llu asks since boot: "
		       "%llu cycles a page -- the claim %llu, the MAC %llu, the "
		       "region and the page %llu, the rest %llu; %s; %llu grants, "
		       "%llu cycles (#537)\n", h->part ? h->part->name : "?",
		       (unsigned long long)asks, (unsigned long long)in_kernel,
		       (unsigned long long)(claim / asks),
		       (unsigned long long)(mac / asks),
		       (unsigned long long)(find / asks),
		       (unsigned long long)(rest / asks), rpc,
		       (unsigned long long)grants,
		       (unsigned long long)grant_cyc);
	}
}

static void
blk_phys_account(struct blk_handle *h, unsigned int pages,
		 unsigned long long t0, unsigned long long t1,
		 unsigned long long t2)
{
	h->phys_req++;
	h->phys_pages += pages;
	h->xlate_cyc += t1 - t0;
	h->xfer_cyc += t2 - t1;
	{
		uint64_t next = h->phys_said_pages ? 2 * h->phys_said_pages
						   : 1024;

		if (h->phys_pages >= next) {
			while (2 * next <= h->phys_pages)
				next *= 2;
			h->phys_said_pages = next;
			blk_phys_said(h, "after");
		}
	}
}

/* A refusal the kernel will give again whatever is asked (#599). */
static int
blk_refusal_is_final(kern_return_t kr)
{
	return kr == CAP_ERR_REVOKED || kr == CAP_ERR_INVALID_TOKEN ||
	       kr == CAP_ERR_RESOURCE_MISMATCH || kr == KERN_INVALID_ADDRESS;
}

/*
 * The address this handle's controller reaches page `pa' at, for a transfer
 * in direction `op' (the device reads the page: CAP_OP_DMA_DEVICE_READ; it
 * writes it: _WRITE).  Asks the kernel with this handle's capabilities, the
 * one that answered last first.  Success from any wins.  A capability the
 * kernel refuses for good (revoked, not genuine, not a buffer's, the buffer
 * gone) is forgotten; one that does not cover this page or this direction is
 * kept.  Any other answer ends the search and is returned.
 */
static kern_return_t
blk_dma_for(struct blk_handle *h, vm_address_t pa, natural_t op,
	    vm_address_t *dma)
{
	struct blk_controller *ctrl = h->part->ctrl;
	natural_t	bdf = (natural_t)((ctrl->pci_bus << 8) |
					  (ctrl->pci_slot << 3) |
					  ctrl->pci_func);
	kern_return_t	kr, result = KERN_NO_ACCESS;
	int		drop[BLK_HANDLE_DMA_CAPS] = { 0 };
	uint64_t	answered = 0;
	unsigned int	j, i, n = h->n_dma_caps, kept;

	h->dma_asked = 0;
	for (j = 0; j < n; j++) {
		i = (h->dma_last + j) % n;
		h->dma_asked++;
		kr = device_dma_map_foreign_op(master_device, bdf, pa, op,
					       (char *)&h->dma_cap[i],
					       sizeof(h->dma_cap[i]), dma);
		if (kr == KERN_SUCCESS) {
			answered = h->dma_cap[i].cap_id;
			result = KERN_SUCCESS;
			break;
		}
		if (kr == KERN_PROTECTION_FAILURE)
			result = kr;
		else if (blk_refusal_is_final(kr)) {
			drop[i] = 1;
			h->dma_final_kr = kr;
		}
		else if (kr != KERN_NO_ACCESS) {
			result = kr;
			break;
		}
	}

	/* Forget the dead ones, keeping the order; find the one that answered. */
	for (i = 0, kept = 0; i < n; i++) {
		if (drop[i]) {
			printf("blk: %s: capability %llu forgotten — the kernel "
			       "says it is revoked, not genuine, not a buffer's, "
			       "or its buffer is gone\n", h->part->name,
			       (unsigned long long)h->dma_cap[i].cap_id);
			blk_xlate_free(&h->dma_xlate[i]);	/* #537 */
			h->dropped++;
			continue;
		}
		h->dma_xlate[kept] = h->dma_xlate[i];
		h->dma_cap[kept++] = h->dma_cap[i];
	}
	for (i = kept; i < n; i++) {
		memset(&h->dma_cap[i], 0, sizeof(h->dma_cap[i]));
		memset(&h->dma_xlate[i], 0, sizeof(h->dma_xlate[i]));
	}
	h->n_dma_caps = kept;
	h->dma_last = 0;
	for (i = 0; i < kept; i++)
		if (result == KERN_SUCCESS && h->dma_cap[i].cap_id == answered)
			h->dma_last = i;

	return result;
}

/*
 * #537 step 2: every page of a request in one ask, when it has more than one.
 * The handle's capabilities are tried as blk_dma_for() tries them, the one
 * that answered last first, one call each for the whole request.  Anything
 * but an answer for all the pages falls back to blk_dma_for() page by page,
 * which finds the page refused, forgets a capability the kernel refuses for
 * good, and is what blk_refused() has always been told about -- a request
 * whose pages lie in two buffers is answered that way too.  On a refusal
 * *refused is the page.  UROS_ABLATE_537_NO_BATCH asks page by page always.
 */
#ifndef BLK_ABLATE_537_NO_BATCH
#define BLK_ABLATE_537_NO_BATCH 0
#endif

/*
 * #537 step 3: a buffer's translation, kept per handle where the device is
 * isolated (see struct blk_xlate).  UROS_ABLATE_537_NO_TABLE keeps none, so
 * every transfer asks; UROS_ABLATE_537_TABLE_KEPT leaves the capabilities and
 * their tables on a revocation notice, so a revoked buffer's translation is
 * USED -- the IOMMU's refusal of it is what cap_test [26] asks about.
 */

static void
blk_xlate_free(struct blk_xlate *x)
{
	if (x->ix != NULL)
		free(x->ix);
	memset(x, 0, sizeof(*x));
}

static void
blk_xlate_sift(struct blk_xlate_page *ix, unsigned int root, unsigned int n)
{
	for (;;) {
		unsigned int child = 2 * root + 1;
		struct blk_xlate_page t;

		if (child >= n)
			return;
		if (child + 1 < n && ix[child + 1].pa > ix[child].pa)
			child++;
		if (ix[root].pa >= ix[child].pa)
			return;
		t = ix[root];
		ix[root] = ix[child];
		ix[child] = t;
		root = child;
	}
}

/*
 * Ask once for the translation of the buffer `t' names, and keep it in `x'.
 * Nothing kept when the device is not isolated, or anything fails: the
 * transfers then ask, as they always did.
 */
static void
blk_xlate_take(struct blk_handle *h, const struct uros_cap *t,
	       struct blk_xlate *x)
{
	struct blk_controller *ctrl = h->part->ctrl;
	natural_t	bdf = (natural_t)((ctrl->pci_bus << 8) |
					  (ctrl->pci_slot << 3) |
					  ctrl->pci_func);
	natural_t	isolated = 0, identity = 0, ops = 0;
	vm_address_t	base = 0, *pages = NULL;
	mach_msg_type_number_t cnt = 0;
	struct blk_xlate_page *ix;
	struct blk_xlate_page tmp;
	unsigned int	i;
	kern_return_t	kr;

	blk_xlate_free(x);
	if (BLK_ABLATE_537_NO_TABLE)
		return;
	kr = device_dma_map_foreign_region(master_device, bdf, (char *)t,
					   sizeof(*t), &isolated, &identity,
					   &base, &ops, &pages, &cnt);
	if (kr != KERN_SUCCESS || !isolated || cnt == 0) {
		if (pages != NULL && cnt > 0)
			(void)vm_deallocate(mach_task_self(),
					    (vm_address_t)pages,
					    cnt * sizeof(vm_address_t));
		return;
	}
	ix = (struct blk_xlate_page *)malloc(cnt * sizeof(*ix));
	if (ix != NULL) {
		for (i = 0; i < cnt; i++) {
			ix[i].pa = pages[i];
			ix[i].page = i;
		}
		for (i = cnt / 2; i-- > 0; )
			blk_xlate_sift(ix, i, cnt);
		for (i = cnt; i-- > 1; ) {
			tmp = ix[0];
			ix[0] = ix[i];
			ix[i] = tmp;
			blk_xlate_sift(ix, 0, i);
		}
		x->npages = cnt;
		x->identity = (int)identity;
		x->ops = ops;
		x->base = base;
		x->ix = ix;
		printf("blk: %s: buffer %llu translated once, %u pages kept on "
		       "this handle -- its transfers ask the kernel nothing "
		       "(#537)\n", h->part->name,
		       (unsigned long long)t->resource_id, (unsigned)cnt);
	}
	(void)vm_deallocate(mach_task_self(), (vm_address_t)pages,
			    cnt * sizeof(vm_address_t));
}

/* The device address of `pa' for `op' from this handle's kept translations. */
static int
blk_xlate_find(const struct blk_handle *h, vm_address_t pa, natural_t op,
	       vm_address_t *dma)
{
	vm_address_t page = pa & ~(vm_address_t)(BLK_DMA_PAGE - 1);
	unsigned int i;

	for (i = 0; i < h->n_dma_caps; i++) {
		const struct blk_xlate *x = &h->dma_xlate[i];
		unsigned int lo = 0, hi = x->npages;

		if (x->npages == 0 || (x->ops & op) == 0)
			continue;
		while (lo < hi) {
			unsigned int mid = lo + (hi - lo) / 2;

			if (x->ix[mid].pa < page)
				lo = mid + 1;
			else
				hi = mid;
		}
		if (lo < x->npages && x->ix[lo].pa == page) {
			*dma = x->identity ? pa
				: x->base + (vm_address_t)x->ix[lo].page *
					    BLK_DMA_PAGE +
				  (pa & (vm_address_t)(BLK_DMA_PAGE - 1));
			return 1;
		}
	}
	return 0;
}

static kern_return_t
blk_dma_for_pages(struct blk_handle *h, const vm_address_t *pa,
		  unsigned int n, natural_t op, vm_address_t *dma,
		  unsigned int *refused)
{
	struct blk_controller *ctrl = h->part->ctrl;
	natural_t	bdf = (natural_t)((ctrl->pci_bus << 8) |
					  (ctrl->pci_slot << 3) |
					  ctrl->pci_func);
	unsigned int	j, i, k;
	kern_return_t	kr;

	/* #537 step 3: every page from a kept translation, or none of them. */
	for (k = 0; k < n; k++)
		if (!blk_xlate_find(h, pa[k], op, &dma[k]))
			break;
	if (k == n)
		return KERN_SUCCESS;

	if (n > 1 && !BLK_ABLATE_537_NO_BATCH) {
		for (j = 0; j < h->n_dma_caps; j++) {
			mach_msg_type_number_t cnt = n;

			i = (h->dma_last + j) % h->n_dma_caps;
			kr = device_dma_map_foreign_ops(master_device, bdf,
					(vm_address_t *)pa, n, op,
					(char *)&h->dma_cap[i],
					sizeof(h->dma_cap[i]), dma, &cnt);
			if (kr == KERN_SUCCESS && cnt == n) {
				h->dma_last = i;
				return KERN_SUCCESS;
			}
			if (kr != KERN_NO_ACCESS)
				break;		/* the per-page path judges it */
		}
	}
	for (k = 0; k < n; k++) {
		kr = blk_dma_for(h, pa[k], op, &dma[k]);
		if (kr != KERN_SUCCESS) {
			*refused = k;
			return kr;
		}
	}
	return KERN_SUCCESS;
}

/*
 * Say a refused physical transfer, per handle: at the first, at every power
 * of two, and whenever the reason changes (#599).
 */
static void
blk_refused(struct blk_handle *h, vm_address_t pa, natural_t op,
	    kern_return_t kr)
{
	int changed = kr != h->refusal_kr;

	h->refusals++;
	h->refusal_kr = kr;
	if (!changed && (h->refusals & (h->refusals - 1)) != 0)
		return;
	/*
	 * #599: "not asked" only when blk_dma_for had nothing to ask with.
	 * It asks with every capability and forgets the ones the kernel
	 * refuses for good, so a handle can reach here holding none after the
	 * kernel refused them in this very request (found in review).
	 */
	if (h->n_dma_caps == 0 && h->dma_asked != 0)
		printf("blk: %s: a physical %s of 0x%lx refused before any DMA "
		       "— the kernel refused, for good, the %u capabilit%s "
		       "this handle held (kr=%d), and they are forgotten "
		       "(refusal %u on this handle)\n",
		       h->part->name,
		       op == CAP_OP_DMA_DEVICE_WRITE ? "read" : "write",
		       (unsigned long)pa, h->dma_asked,
		       h->dma_asked == 1 ? "y" : "ies", (int)h->dma_final_kr,
		       h->refusals);
	else if (h->n_dma_caps == 0)
		printf("blk: %s: a physical %s of 0x%lx refused before any DMA "
		       "— this handle holds no buffer capability (%s); the "
		       "kernel was not asked (refusal %u on this handle)\n",
		       h->part->name,
		       op == CAP_OP_DMA_DEVICE_WRITE ? "read" : "write",
		       (unsigned long)pa,
		       h->dropped ? "those handed were revoked or their "
				    "buffers freed" : "none was handed",
		       h->refusals);
	else
		printf("blk: %s: a physical %s of 0x%lx refused before any DMA "
		       "— the kernel was asked with %u capabilit%s and "
		       "answered kr=%d; the handle holds %u now (refusal %u on "
		       "this handle)\n",
		       h->part->name,
		       op == CAP_OP_DMA_DEVICE_WRITE ? "read" : "write",
		       (unsigned long)pa, h->dma_asked,
		       h->dma_asked == 1 ? "y" : "ies", (int)kr,
		       h->n_dma_caps, h->refusals);
}

kern_return_t
ds_device_read_phys(mach_port_t device, mach_port_t reply,
		    mach_msg_type_name_t reply_poly,
		    dev_mode_t mode, recnum_t recnum,
		    io_buf_len_t bytes_wanted,
		    vm_address_t *phys_addrs,
		    mach_msg_type_number_t phys_addrsCnt,
		    io_buf_len_t *bytes_read)
{
	struct blk_handle *h = blk_authed_handle(device);
	if (!h)
		return KERN_NO_ACCESS;
	struct blk_partition *part = h->part;
	struct blk_controller *ctrl = part->ctrl;
	unsigned int total, nsectors;
	kern_return_t kr;

	if (!ctrl->ops->read_sectors_phys)
		return D_INVALID_OPERATION;

	if (bytes_wanted == 0 || phys_addrsCnt == 0)
		return D_INVALID_SIZE;

	total = (bytes_wanted + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
	nsectors = total / SECTOR_SIZE;

	if (recnum + nsectors > part->num_sectors)
		return D_INVALID_SIZE;

	if (!blk_pages_cover(phys_addrsCnt, total, "read"))
		return D_INVALID_SIZE;

	{
		vm_address_t	dma[BLK_PHYS_PAGES_MAX];
		unsigned int	i;

		if (phys_addrsCnt > BLK_PHYS_PAGES_MAX)
			return D_INVALID_SIZE;

		/*
		 * #599: every page first, for the direction a read needs (the
		 * device writes the page); the first refusal ends the request
		 * before the driver is called, so nothing is sent and nothing
		 * reports success.
		 */
		unsigned long long t0 = blk_tsc(), t1;
		int rc;

		kr = blk_dma_for_pages(h, phys_addrs, phys_addrsCnt,
				       CAP_OP_DMA_DEVICE_WRITE, dma, &i);
		if (kr != KERN_SUCCESS) {
			blk_refused(h, phys_addrs[i], CAP_OP_DMA_DEVICE_WRITE,
				    kr);
			return kr;
		}

		t1 = blk_tsc();
		rc = ctrl->ops->read_sectors_phys(ctrl->priv,
					  part->disk_index,
					  part->start_lba + recnum,
					  nsectors,
					  dma, phys_addrsCnt,
					  total);
		blk_phys_account(h, phys_addrsCnt, t0, t1, blk_tsc());
		if (rc < 0)
			return D_IO_ERROR;
	}

	*bytes_read = bytes_wanted;
	return KERN_SUCCESS;
}

kern_return_t
ds_device_write_phys(mach_port_t device, mach_port_t reply,
		     mach_msg_type_name_t reply_poly,
		     dev_mode_t mode, recnum_t recnum,
		     io_buf_len_t bytes_to_write,
		     vm_address_t *phys_addrs,
		     mach_msg_type_number_t phys_addrsCnt,
		     io_buf_len_t *bytes_written)
{
	struct blk_handle *h = blk_authed_handle(device);
	if (!h)
		return KERN_NO_ACCESS;
	struct blk_partition *part = h->part;
	kern_return_t kr;
	struct blk_controller *ctrl = part->ctrl;
	unsigned int total, nsectors;

	if (!ctrl->ops->write_sectors_phys)
		return D_INVALID_OPERATION;

	if (bytes_to_write == 0 || phys_addrsCnt == 0)
		return D_INVALID_SIZE;

	total = (bytes_to_write + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
	nsectors = total / SECTOR_SIZE;

	if (recnum + nsectors > part->num_sectors)
		return D_INVALID_SIZE;

	if (!blk_pages_cover(phys_addrsCnt, total, "write"))
		return D_INVALID_SIZE;

	ra_invalidate(part);

	{
		vm_address_t	dma[BLK_PHYS_PAGES_MAX];
		unsigned int	i;

		if (phys_addrsCnt > BLK_PHYS_PAGES_MAX)
			return D_INVALID_SIZE;

		/*
		 * 🔴 THE WRITE HALF HANDED OVER UNTRANSLATED ADDRESSES once;
		 * both halves translate every page now, each for its own
		 * direction (#599: a write needs the device to READ the page),
		 * and refuse the same way: before the driver is called.
		 */
		unsigned long long t0 = blk_tsc(), t1;
		int rc;

		kr = blk_dma_for_pages(h, phys_addrs, phys_addrsCnt,
				       CAP_OP_DMA_DEVICE_READ, dma, &i);
		if (kr != KERN_SUCCESS) {
			blk_refused(h, phys_addrs[i], CAP_OP_DMA_DEVICE_READ,
				    kr);
			return kr;
		}

		t1 = blk_tsc();
		rc = ctrl->ops->write_sectors_phys(ctrl->priv,
						   part->disk_index,
						   part->start_lba + recnum,
						   nsectors,
						   dma, phys_addrsCnt,
						   total);
		blk_phys_account(h, phys_addrsCnt, t0, t1, blk_tsc());
		if (rc < 0)
			return D_IO_ERROR;
	}

	*bytes_written = bytes_to_write;
	return KERN_SUCCESS;
}

/* ================================================================
 * ds_device_get_status — DEV_GET_SIZE returns partition info
 * ================================================================ */

kern_return_t
ds_device_get_status(mach_port_t device, dev_flavor_t flavor,
		     dev_status_t status,
		     mach_msg_type_number_t *status_count)
{
	/*
	 * get_status returns public metadata (size, sector size) and is
	 * called by libblk during discovery, before any device_open_cap.
	 * Accept both raw partition ports and authenticated handles.
	 */
	struct blk_partition *part;
	if (device == 0)
		return D_NO_SUCH_DEVICE;
	uint32_t *magicp = blk_object_for(device);
	uint32_t magic = magicp ? *magicp : 0;
	if (magic == BLK_MAGIC_HANDLE)
		part = ((struct blk_handle *)blk_object_for(device))->part;
	else if (magic == BLK_MAGIC_PART)
		part = blk_object_for(device);
	else
		return D_NO_SUCH_DEVICE;

	if (flavor == DEV_GET_SIZE) {
		/*
		 * DEV_GET_SIZE_DEVICE_SIZE is in bytes (matches the kernel
		 * IDE driver and what default_pager / libblk expect — both
		 * compute capacity_sectors = DEVICE_SIZE / RECORD_SIZE).
		 * Returning sectors here was a latent bug that bounded the
		 * default_pager swap to a few KB once paging moved through
		 * BDS in Issue #184.
		 *
		 * ── The slot is an `int', and that is a 2 GiB ceiling (#420)
		 *
		 * 🔴 `dev_status_t' is `int *'.  A partition of 2 GiB or more
		 * has a byte count that does not fit, and the cast that used
		 * to be here turned it into a NEGATIVE number that every
		 * caller then divided by the record size.  Nothing in the
		 * tree has a partition that large yet, which is the only
		 * reason it has never been seen.
		 *
		 * 🔑 AND IT IS #420's OWN DONE-WHEN, unnoticed: that issue
		 * asks that a disk larger than 2 TiB "reports its true
		 * capacity", and no disk above 2 GiB can report anything
		 * true through this slot no matter how wide the block layer's
		 * own sector types become.  Widening them is necessary and
		 * not sufficient; the protocol has to move too.
		 *
		 * Until it does, say the one true thing the protocol can
		 * express.  DEV_GET_SIZE_DEVICE_SIZE is documented as "0 if
		 * unknown" in <device/device_types.h>, so an unrepresentable
		 * size is reported as unknown -- a refusal in the protocol's
		 * own vocabulary -- and the record size, which is still
		 * answerable, is still answered.
		 */
		/*
		 * ⚠️ Derived from the destination type and not taken from
		 * <limits.h>: #506 took the host's headers out of userland
		 * and `-ffreestanding' does not put limits.h back, so an
		 * include here would reintroduce exactly what that issue
		 * removed.  `~0u >> 1' is the largest value an int slot can
		 * carry, and it is one expression instead of a dependency.
		 */
		const uint64_t	slot_max = (uint64_t)(~0u >> 1);
		uint64_t	bytes;

		bytes = (uint64_t)part->num_sectors * SECTOR_SIZE;
		if (bytes > slot_max) {
			printf("blk: %s is %u sectors, and its size in bytes "
			       "does not fit the int DEV_GET_SIZE returns — "
			       "reporting it as unknown (#420)\n",
			       part->name, (unsigned)part->num_sectors);
			status[DEV_GET_SIZE_DEVICE_SIZE] = 0;
		} else {
			status[DEV_GET_SIZE_DEVICE_SIZE] = (int)bytes;
		}
		status[DEV_GET_SIZE_RECORD_SIZE] = (int)SECTOR_SIZE;
		*status_count = DEV_GET_SIZE_COUNT;
		return KERN_SUCCESS;
	}
	return D_INVALID_OPERATION;
}

/* ================================================================
 * Stubs for unimplemented device operations
 * ================================================================ */

kern_return_t
ds_device_read_inband(mach_port_t device, mach_port_t reply,
		      mach_msg_type_name_t reply_poly,
		      dev_mode_t mode, recnum_t recnum,
		      io_buf_len_t bytes_wanted,
		      io_buf_ptr_inband_t data,
		      mach_msg_type_number_t *data_count)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_device_write_inband(mach_port_t device, mach_port_t reply,
		       mach_msg_type_name_t reply_poly,
		       dev_mode_t mode, recnum_t recnum,
		       io_buf_ptr_inband_t data,
		       mach_msg_type_number_t data_count,
		       io_buf_len_t *bytes_written)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_device_set_status(mach_port_t device, dev_flavor_t flavor,
		     dev_status_t status,
		     mach_msg_type_number_t status_count)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_device_map(mach_port_t device, vm_prot_t prot,
	      vm_offset_t offset, vm_size_t size,
	      mach_port_t *pager, boolean_t unmap)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_device_set_filter(mach_port_t device, mach_port_t receive_port,
		     int priority, filter_array_t filter,
		     mach_msg_type_number_t filter_count)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_io_done_queue_create(mach_port_t host, mach_port_t *queue)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_io_done_queue_terminate(mach_port_t queue)
{
	return D_INVALID_OPERATION;
}

/* ================================================================
 * Read-back self-test (#520)
 * ================================================================ */

/*
 * 🔑 THE POINT IS THE ORACLE, NOT THE READ.
 *
 * #520's done-when asks for "a read whose BYTES are checked, not merely a
 * call that returns success", and the reason it is worded that way is that
 * this server already had the other kind: cap_test issued a device_read,
 * looked at the return code, threw the buffer away, and printed "ok".  Every
 * defect this port was written to find -- an address that lost its top half,
 * a DMA into a plausible wrong page, an LBA computed without the partition
 * base -- returns KERN_SUCCESS and a buffer full of the wrong thing.
 *
 * So what matters is where the expected values come from.  All of them here
 * were written by software that is not ours and does not know we exist:
 *
 *   sfdisk    wrote the MBR, its signature and its partition entries
 *   mke2fs    wrote the ext2 superblock, its magic and its geometry
 *
 * and one of them was written by this server itself, at probe time, from the
 * same bytes:
 *
 *   the partition table this server parsed and published
 *
 * ⚠️ Which is the check that pays.  A driver could return a buffer of the
 * right shape from the wrong place and still show an ext2 magic -- there is
 * one every 4 KiB on this disk.  It cannot make the MBR's `relsect' field
 * equal the number this server independently arrived at unless it really
 * read sector zero, because that is a 32-bit value held in two places that
 * only agree if the read landed where it was asked to.
 *
 * ── Two reads, and why the second is not a duplicate ────────────────
 *
 * The MBR is at absolute LBA 0 and the superblock is 1024 bytes into the
 * partition, which is somewhere else entirely.  A driver that ignored the
 * LBA and returned one cached buffer for every request would pass a check
 * that only ever looked at one place; here the two reads must come back
 * DIFFERENT, and that is asserted rather than assumed.
 *
 * ── What is deliberately NOT required ───────────────────────────────
 *
 * The volume label.  It is printed because it is worth seeing, but nothing
 * fails if it changes: tying a self-test to a string in a build script makes
 * the script's next edit look like a driver defect.  The structural facts --
 * a magic at a fixed offset, a geometry that fits inside its partition, a
 * base sector held twice -- do not move when someone renames a disk.
 *
 * ⚠️ And ext2 itself is not required.  A partition that is not ext2 skips the
 * filesystem half and SAYS SO, because a check that quietly does not run is
 * indistinguishable from one that passed.
 */

#define EXT2_SB_OFFSET		1024u	/* superblock, bytes into the fs */
#define EXT2_SB_MAGIC		0xEF53u
#define EXT2_SB_OFF_BLOCKS	4u	/* s_blocks_count		*/
#define EXT2_SB_OFF_LOGBSIZE	24u	/* s_log_block_size		*/
#define EXT2_SB_OFF_MAGIC	56u	/* s_magic			*/
#define EXT2_SB_OFF_LABEL	120u	/* s_volume_name, 16 bytes	*/

static uint16_t
rd16(const unsigned char *p, unsigned int off)
{
	return (uint16_t)(p[off] | ((uint16_t)p[off + 1] << 8));
}

static uint32_t
rd32(const unsigned char *p, unsigned int off)
{
	return (uint32_t)p[off]
	     | ((uint32_t)p[off + 1] << 8)
	     | ((uint32_t)p[off + 2] << 16)
	     | ((uint32_t)p[off + 3] << 24);
}

static void blk_readback_one(struct blk_partition *part);
static void blk_read_bench_one(struct blk_partition *part);

/* ================================================================
 * What a read costs, so #432's cost can be measured and not assumed
 * ================================================================ */

/*
 * 🔴 IN CYCLES, AND DELIBERATELY NOT IN BYTES PER SECOND.  This server does
 * not know the timestamp counter's frequency, and a throughput printed from a
 * frequency nobody measured is a number with a unit it has not earned.  What
 * #432 asks for is the COST OF TRANSLATION, which is a ratio between two runs
 * of the same boot -- and a ratio of cycle counts is dimensionless and needs
 * no frequency at all.
 *
 * 🔑 AND VIRTIO IS THE CONTROL GROUP, IN THE SAME BOOT.  QEMU's transitional
 * virtio-blk does its DMA outside the emulated IOMMU, and AHCI is an ordinary
 * bus master that goes through it.  So one boot produces both a treated and an
 * untreated measurement of the same kernel on the same machine at the same
 * clock: if the AHCI figures move between `-I' and no `-I' and the virtio
 * figure does not, the difference is translation.  If both move, it is the
 * boot.  A benchmark with no control cannot tell those apart, and the second
 * is what a governor, a thermal limit or a busy host produces.
 *
 * ⚠️ It reads through the DRIVER and not through ds_device_read, so the
 * readahead cache is not in the path: every iteration is a real transfer.  The
 * buffer allocation and release are in the timing too -- they are the same on
 * both sides of the comparison, and taking them out would mean timing
 * something no caller can actually ask for.
 */
/*
 * 🔴🔴 ONE AGGREGATE OVER 32 READS DID NOT DISCRIMINATE, AND THIS IS WHAT
 * REPLACED IT.  Five pairs of boots, medians: virtio (the control) moved
 * -0.7%, and the two AHCI disks moved -5.5% and +6.1% -- in OPPOSITE
 * DIRECTIONS -- while one arm's five samples ran 3807 to 7093, a spread of
 * 1.9x.  An experiment discriminates only when the dispersion is smaller than
 * the effect, and that one measured the host's file cache warming up.
 *
 * 🔑 So the median is taken INSIDE one boot, over rounds, and the first rounds
 * are thrown away.  A cold host cache is one round now instead of a whole
 * sample, and one boot answers with a figure instead of a guess -- which also
 * means the boot-to-boot spread finally says something about boots.
 *
 * ── AND WHAT IT THEN MEASURED, WHICH IS NOT A NUMBER ──────────────────
 *
 * Five more pairs, medians of five boots each, KVM, governor at performance:
 *
 *	virtio (untreated control)	-3.2%	spread 26% / 12%
 *	ahci0a				+0.9%	spread  9% / 15%
 *	ahci1a				+0.6%	spread  9% /  9%
 *
 * The two TREATED disks now agree with each other and the untreated control
 * moved further than either of them, in the other direction.  That is the
 * signature of an effect below the noise, and the honest reading is that this
 * instrument cannot resolve the cost of translation on this emulator.
 *
 * ⚠️ Normalising each boot by its own control made it WORSE -- spread 18% to
 * 32%, and the two disks disagreeing by 5.7 points.  So the noise is not
 * common to the devices within a boot, and dividing by the control adds
 * variance instead of cancelling it.  Written down because it is the obvious
 * next thing to try and it does not work here.
 *
 * 🔑 WHAT THE MEASUREMENT DOES ESTABLISH IS AN UPPER BOUND, and that is worth
 * having: the cost is not large.  A kernel that had ended up mapping and
 * unmapping per TRANSFER rather than per buffer would show a factor, not a
 * percent, and this says it does not.  The number itself needs hardware --
 * on silicon the engine's cost is an IOTLB lookup, and on an emulator it is
 * whatever that emulator's software does, which is a fact about QEMU.
 */
#define	BLK_BENCH_CHUNK		64u	/* sectors per read: 32 KiB   */
#define	BLK_BENCH_ROUNDS	64u	/* timed rounds, 2 MiB total  */
#define	BLK_BENCH_WARMUP	8u	/* discarded: the host's cache */

static unsigned long long
blk_tsc(void)
{
	unsigned int lo, hi;

	/*
	 * 🔴 THE WHOLE COUNTER.  rdtsc answers in EDX:EAX and reading EAX
	 * alone is a difference modulo 2^32 -- at 3.9 GHz that wraps every 1.1
	 * seconds, and a wrapped value is indistinguishable from a small one.
	 * #523 was one benchmark reading half of this and reporting i386 as
	 * 110x slower than x86-64 at creating a thread.
	 */
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long)hi << 32) | lo;
}

void
blk_read_bench(void)
{
	int	i;

	if (n_partitions <= 0)
		return;

	for (i = 0; i < n_partitions; i++)
		blk_read_bench_one(&partitions[i]);
}

static void
blk_read_bench_one(struct blk_partition *part)
{
	struct blk_controller	*ctrl = part->ctrl;
	unsigned long long	each[BLK_BENCH_ROUNDS];
	unsigned long long	start, total;
	unsigned int		round, done = 0, window;

	if (ctrl == NULL || ctrl->ops == NULL
	    || ctrl->ops->read_sectors == NULL)
		return;

	/*
	 * ⚠️ Refused rather than truncated when the partition is too small.  A
	 * benchmark that quietly read fewer sectors would still print a
	 * per-sector figure, and two runs of different sizes would be compared
	 * as if they were the same measurement.
	 */
	if (part->num_sectors < (uint32_t)BLK_BENCH_CHUNK) {
		printf("blk: read bench did not run on %s — %u sectors is "
		       "smaller than the %u one read takes\n", part->name,
		       (unsigned)part->num_sectors, BLK_BENCH_CHUNK);
		return;
	}

	/*
	 * How many distinct chunks the partition holds, capped at the number
	 * of rounds: a smaller partition is read round and round rather than
	 * refused, and a larger one is not read further than the rounds go.
	 * ⚠️ Both arms of a comparison therefore touch the SAME sectors, which
	 * is what stops "the other disk is bigger" from being the finding.
	 */
	window = (unsigned)(part->num_sectors / BLK_BENCH_CHUNK);
	if (window > BLK_BENCH_ROUNDS)
		window = BLK_BENCH_ROUNDS;

	/*
	 * ⚠️ The window is read WARM and then read again, on purpose.  The
	 * subject is what one transfer costs inside the machine -- the
	 * driver's path, the engine's walk -- and not how fast the host can
	 * fetch a block it has never seen.  Re-reading the same sectors keeps
	 * the second out of the measurement; the guest caches nothing, because
	 * this calls the DRIVER and the readahead cache is above it.
	 */
	for (round = 0; round < BLK_BENCH_WARMUP; round++) {
		vm_offset_t	buf = 0;
		unsigned int	got = 0;

		if (ctrl->ops->read_sectors(ctrl->priv, part->disk_index,
					    part->start_lba
					    + (round % window) * BLK_BENCH_CHUNK,
					    BLK_BENCH_CHUNK, &buf, &got) < 0) {
			if (buf != 0)
				vm_deallocate(mach_task_self(), buf, got);
			printf("blk: read bench on %s failed warming up\n",
			       part->name);
			return;
		}
		vm_deallocate(mach_task_self(), buf, got);
	}

	for (round = 0; round < BLK_BENCH_ROUNDS; round++) {
		vm_offset_t	buf = 0;
		unsigned int	got = 0;

		start = blk_tsc();
		if (ctrl->ops->read_sectors(ctrl->priv, part->disk_index,
					    part->start_lba
					    + (round % window) * BLK_BENCH_CHUNK,
					    BLK_BENCH_CHUNK, &buf, &got) < 0) {
			if (buf != 0)
				vm_deallocate(mach_task_self(), buf, got);
			break;
		}
		each[round] = blk_tsc() - start;

		done += BLK_BENCH_CHUNK;
		vm_deallocate(mach_task_self(), buf, got);
	}

	/*
	 * Insertion sort, because sixty-four elements do not need anything
	 * else and a sort with a bug in it would produce a median that is
	 * merely one of the samples -- which looks entirely reasonable.
	 */
	{
		unsigned a, b;

		for (a = 1; a < round; a++) {
			unsigned long long v = each[a];

			for (b = a; b > 0 && each[b - 1] > v; b--)
				each[b] = each[b - 1];
			each[b] = v;
		}
	}

	total = round == 0 ? 0 : each[round / 2];

	/*
	 * ⚠️ The count of sectors ACTUALLY read is printed beside the figure,
	 * not assumed from the loop bound.  A run that stopped early otherwise
	 * reports a per-sector cost computed over sectors it never read --
	 * which is the shape of a fast-looking result that means the opposite.
	 */
	if (done == 0) {
		printf("blk: read bench on %s read nothing\n", part->name);
		return;
	}

	/*
	 * ⚠️ The MEDIAN round and the FASTEST one, both.  The median is the
	 * figure to compare; the fastest is what says whether the median is a
	 * cost or an interruption -- when they are far apart, something else
	 * was running and the number is about the host.
	 */
	printf("blk: read bench %s — %u rounds of %u sectors, median %llu "
	       "cycles (%llu/sector), fastest %llu\n",
	       part->name, round, BLK_BENCH_CHUNK, total,
	       total / BLK_BENCH_CHUNK, each[0]);
}

/*
 * 🔑 EVERY PARTITION, not the first one.
 *
 * It checked partitions[0] until this target grew a second controller, and
 * then the check ran on the virtio disk and said nothing at all about the
 * AHCI one -- which is the disk that had a defect.  A test that covers the
 * working half is how a defect stays in a tree that reports itself green.
 *
 * Each partition is a different driver's answer about a different disk, so
 * each gets its own five checks and its own verdict line.
 */
void
blk_readback_selftest(void)
{
	int	i;

	if (n_partitions <= 0) {
		printf("blk: read-back self-test did not run — no partition "
		       "was published, so there is nothing to read\n");
		return;
	}

	for (i = 0; i < n_partitions; i++)
		blk_readback_one(&partitions[i]);
}

static void
blk_readback_one(struct blk_partition *part)
{
	struct blk_controller	*ctrl;
	vm_offset_t		mbr = 0, fs = 0;
	unsigned int		mbr_size = 0, fs_size = 0;
	const unsigned char	*m, *f;
	unsigned int		checks = 0, wrong = 0;
	uint32_t		relsect, numsect, fs_blocks, bsize;
	unsigned int		i, sb;
	char			label[17];

	ctrl = part->ctrl;

	if (ctrl == NULL || ctrl->ops == NULL
	    || ctrl->ops->read_sectors == NULL) {
		printf("blk: read-back self-test did not run — %s's driver "
		       "offers no read_sectors\n", part->name);
		return;
	}

	/*
	 * Sector zero of the DISK, not of the partition: the MBR is what
	 * this server parsed its table out of, and reading it back is how
	 * the table gets checked against its own source.
	 */
	if (ctrl->ops->read_sectors(ctrl->priv, part->disk_index, 0, 1,
				    &mbr, &mbr_size) < 0
	    || mbr_size < SECTOR_SIZE) {
		printf("blk: read-back self-test WRONG — the read of LBA 0 "
		       "on %s failed\n", part->name);
		if (mbr != 0)
			vm_deallocate(mach_task_self(), mbr, mbr_size);
		return;
	}

	/*
	 * And the first 4 KiB of the PARTITION, which holds the ext2
	 * superblock 1024 bytes in.  Eight sectors, so the read also
	 * crosses more than one sector and the driver's count arithmetic
	 * is exercised rather than only its LBA.
	 */
	if (ctrl->ops->read_sectors(ctrl->priv, part->disk_index,
				    part->start_lba, 8, &fs, &fs_size) < 0
	    || fs_size < 4096u) {
		printf("blk: read-back self-test WRONG — the read of LBA %u "
		       "on %s failed\n", (unsigned)part->start_lba,
		       part->name);
		vm_deallocate(mach_task_self(), mbr, mbr_size);
		if (fs != 0)
			vm_deallocate(mach_task_self(), fs, fs_size);
		return;
	}

	m = (const unsigned char *)mbr;
	f = (const unsigned char *)fs;

	printf("blk: read-back self-test on %s (%s)\n",
	       part->name, part->stable_name);

	/* [1] sfdisk's signature, at the offset sfdisk put it. */
	checks++;
	if (rd16(m, 510) == MBR_MAGIC) {
		printf("  [1] LBA 0 ends 0x%x — the boot sector signature, "
		       "where a boot sector keeps it\n", (unsigned)MBR_MAGIC);
	} else {
		printf("  [1] WRONG — LBA 0 ends 0x%x, not the 0x%x a boot "
		       "sector must end with\n",
		       (unsigned)rd16(m, 510), (unsigned)MBR_MAGIC);
		wrong++;
	}

	/*
	 * [2] The one that pays.  The MBR entry this partition came from
	 * says where it starts and how long it is; the server holds the
	 * same two numbers, arrived at when it parsed that sector at probe
	 * time.  They agree only if this read landed on sector zero.
	 */
	relsect = 0;
	numsect = 0;
	for (i = 0; i < MBR_NUMPART; i++) {
		unsigned int e = MBR_BOOTSZ + i * 16u;

		if (rd32(m, e + 8) == part->start_lba) {
			relsect = rd32(m, e + 8);
			numsect = rd32(m, e + 12);
			break;
		}
	}
	checks++;
	if (relsect == part->start_lba && numsect == part->num_sectors) {
		printf("  [2] the table says start=%u size=%u and the disk "
		       "says the same — the read reached sector zero\n",
		       (unsigned)relsect, (unsigned)numsect);
	} else {
		printf("  [2] WRONG — the server holds start=%u size=%u, the "
		       "bytes at LBA 0 hold start=%u size=%u\n",
		       (unsigned)part->start_lba,
		       (unsigned)part->num_sectors,
		       (unsigned)relsect, (unsigned)numsect);
		wrong++;
	}

	/* [3] Two different places must give two different answers. */
	checks++;
	if (memcmp(m, f, SECTOR_SIZE) != 0) {
		printf("  [3] LBA 0 and LBA %u came back different, so the "
		       "driver is not answering every read from one buffer\n",
		       (unsigned)part->start_lba);
	} else {
		printf("  [3] WRONG — LBA 0 and LBA %u came back byte for "
		       "byte identical\n", (unsigned)part->start_lba);
		wrong++;
	}

	/* [4] mke2fs's magic, at the offset mke2fs put it. */
	sb = EXT2_SB_OFFSET;
	checks++;
	if (rd16(f, sb + EXT2_SB_OFF_MAGIC) == EXT2_SB_MAGIC) {
		printf("  [4] +%u holds 0x%x — the ext2 superblock is where a "
		       "filesystem writer puts one\n",
		       sb + EXT2_SB_OFF_MAGIC, (unsigned)EXT2_SB_MAGIC);

		for (i = 0; i < 16; i++)
			label[i] = (char)f[sb + EXT2_SB_OFF_LABEL + i];
		label[16] = '\0';

		/*
		 * [5] A third statement of the same physical fact.  sfdisk
		 * said how many sectors the partition has; mke2fs, told
		 * nothing but the size of the file it was handed, wrote a
		 * block count.  The filesystem cannot be bigger than the
		 * partition it is in, and if either read landed somewhere
		 * else the two numbers have no reason to relate.
		 */
		bsize = 1024u << rd32(f, sb + EXT2_SB_OFF_LOGBSIZE);
		fs_blocks = rd32(f, sb + EXT2_SB_OFF_BLOCKS);
		checks++;
		if (bsize >= 1024u && bsize <= 65536u
		    && (uint64_t)fs_blocks * bsize / SECTOR_SIZE
		       <= (uint64_t)part->num_sectors) {
			printf("  [5] %u blocks of %u bytes fit inside %u "
			       "sectors, label \"%s\" — the geometry two "
			       "writers agree on\n",
			       (unsigned)fs_blocks, (unsigned)bsize,
			       (unsigned)part->num_sectors, label);
		} else {
			printf("  [5] WRONG — %u blocks of %u bytes do not "
			       "fit inside %u sectors (label \"%s\")\n",
			       (unsigned)fs_blocks, (unsigned)bsize,
			       (unsigned)part->num_sectors, label);
			wrong++;
		}
	} else {
		printf("  [4] %s is not ext2 (0x%x at +%u, not 0x%x) — the "
		       "filesystem half of this test DID NOT RUN\n",
		       part->name,
		       (unsigned)rd16(f, sb + EXT2_SB_OFF_MAGIC),
		       sb + EXT2_SB_OFF_MAGIC, (unsigned)EXT2_SB_MAGIC);
	}

	if (wrong == 0)
		printf("blk: read-back %u of %u — the bytes on the disk agree "
		       "with the table this server built out of them\n",
		       checks, checks);
	else
		printf("blk: read-back WRONG — %u of %u checks failed\n",
		       wrong, checks);

	vm_deallocate(mach_task_self(), mbr, mbr_size);
	vm_deallocate(mach_task_self(), fs, fs_size);
}
