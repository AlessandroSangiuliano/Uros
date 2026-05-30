/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * gpu_server/gpu_mig.c — Server-side handlers for gpu_server.defs.
 *
 * Naming follows the `gpu_` server prefix declared in gpu_server.defs,
 * so MIG wires the demux in main.c straight to these functions.
 *
 * 0.1.0 (#194) is a skeleton:
 *   - text_puts and query_devices have real (minimal) implementations.
 *   - everything else returns KERN_FAILURE so clients can probe
 *     the surface but cannot do anything destructive.
 *
 * Subsequent issues fill them in:
 *   #195 vga module       — display_set_mode/display_scanout become real
 *                            for the lone VGA device
 *   #196 text_render      — text_puts becomes real (cell grid + scroll)
 *   #197 cap_server glue  — gpu_core_cap_check stops returning 0
 *   future                — bo_alloc/bo_free/context_create
 */

#include <mach.h>
#include <mach/kern_return.h>
#include <mach/gpu_stats.h>
#include <mach/cap_types.h>
#include <stdio.h>
#include <string.h>
#include "gpu_server.h"

/*
 * Pull the cap_id out of the wire-format `struct uros_cap` token the
 * client passed.  device_open / device_close need it to key the
 * handle table; gpu_core_cap_check has already validated the token
 * size by the time we read it.  Returns 0 on token-less callers
 * (handle table won't track them).
 */
static uint64_t
cap_id_from_token(const char *token, mach_msg_type_number_t cnt)
{
	struct uros_cap cap;

	if (token == NULL || cnt != sizeof(cap))
		return 0;
	memcpy(&cap, token, sizeof(cap));
	return cap.cap_id;
}

/* MIG generates a leading mach_port_t argument named gpu_port even
 * for simpleroutines; we just acknowledge and ignore it. */

/* ============================================================
 * Text plane
 * ============================================================ */

kern_return_t
gpu_text_puts(mach_port_t gpu_port,
	      char *cap, mach_msg_type_number_t cap_count,
	      char *buf, mach_msg_type_number_t buf_count)
{
	(void)gpu_port;

	/* Cap check up front so a forged token is rejected before we
	 * touch the queue.  resource_id == 0 = "the system text
	 * surface" (single VGA in 0.1.0); GPU_CAP_DISPLAY_SCANOUT is
	 * the §5 op that authorises writes to a display. */
	if (gpu_core_cap_check(cap, cap_count,
			       GPU_CAP_DISPLAY_SCANOUT, 0) != 0)
		return KERN_PROTECTION_FAILURE;

	/* Hand off to the text_render worker.  This is a MIG
	 * simpleroutine, so any blocking here would push the cost of
	 * VGA paint onto the producer — design doc §11.3 rule 2
	 * forbids that. */
	gpu_text_render_enqueue(buf, (size_t)buf_count);
	return KERN_SUCCESS;
}

/* ============================================================
 * Device queries
 * ============================================================ */

kern_return_t
gpu_query_devices(mach_port_t gpu_port,
		  vm_offset_t *devices, mach_msg_type_number_t *devices_count,
		  uint32_t *n_devices)
{
	unsigned int n = gpu_core_dev_count();
	vm_size_t bytes;
	vm_offset_t buf;
	kern_return_t kr;

	(void)gpu_port;

	bytes = (vm_size_t)n * sizeof(struct gpu_device_info);
	if (bytes == 0) {
		*devices = 0;
		*devices_count = 0;
		*n_devices = 0;
		return KERN_SUCCESS;
	}

	buf = 0;
	kr = vm_allocate(mach_task_self(), &buf, bytes, TRUE);
	if (kr != KERN_SUCCESS)
		return kr;

	(void)gpu_core_dev_copy_all((struct gpu_device_info *)buf, n);

	*devices = buf;
	*devices_count = (mach_msg_type_number_t)bytes;
	*n_devices = (uint32_t)n;
	return KERN_SUCCESS;
}

kern_return_t
gpu_device_open(mach_port_t gpu_port,
		char *cap, mach_msg_type_number_t cap_count,
		uint32_t dev_id)
{
	struct gpu_device_entry *dev;

	(void)gpu_port;

	if (gpu_core_cap_check(cap, cap_count, GPU_CAP_DEV_OPEN,
			       (uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;

	dev = gpu_core_dev_lookup((gpu_dev_id_t)dev_id);
	if (dev == NULL)
		return KERN_INVALID_ARGUMENT;

	/* #202: register the (cap_id, dev_id) pair so cap_revoke_notify
	 * can tear it down when cap_server revokes the issuing cap.  A
	 * full table is the only failure mode (caps without an cap_id —
	 * token-less open — collapse onto cap_id=0 and share one row). */
	if (gpu_core_handle_open(cap_id_from_token(cap, cap_count),
				 (gpu_dev_id_t)dev_id) < 0)
		return KERN_RESOURCE_SHORTAGE;
	return KERN_SUCCESS;
}

kern_return_t
gpu_device_close(mach_port_t gpu_port,
		 char *cap, mach_msg_type_number_t cap_count,
		 uint32_t dev_id)
{
	(void)gpu_port;

	if (gpu_core_cap_check(cap, cap_count, GPU_CAP_DEV_OPEN,
			       (uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;

	if (gpu_core_handle_close(cap_id_from_token(cap, cap_count),
				  (gpu_dev_id_t)dev_id) < 0)
		return KERN_INVALID_ARGUMENT;
	return KERN_SUCCESS;
}

kern_return_t
gpu_query_displays(mach_port_t gpu_port,
		   uint32_t dev_id,
		   vm_offset_t *displays, mach_msg_type_number_t *displays_count,
		   uint32_t *n_displays)
{
	struct gpu_device_entry *dev;
	uint32_t n;
	vm_size_t bytes;
	vm_offset_t buf;
	kern_return_t kr;
	uint32_t i;
	gpu_mode_t *modes;

	(void)gpu_port;

	dev = gpu_core_dev_lookup((gpu_dev_id_t)dev_id);
	if (dev == NULL)
		return KERN_INVALID_ARGUMENT;

	n = dev->info.n_displays;
	bytes = (vm_size_t)n * sizeof(gpu_mode_t);
	if (bytes == 0) {
		*displays = 0;
		*displays_count = 0;
		*n_displays = 0;
		return KERN_SUCCESS;
	}

	buf = 0;
	kr = vm_allocate(mach_task_self(), &buf, bytes, TRUE);
	if (kr != KERN_SUCCESS)
		return kr;

	modes = (gpu_mode_t *)buf;
	memset(modes, 0, bytes);
	for (i = 0; i < n; i++) {
		/* Skeleton: report a fake 80x25 text mode for slot 0; the
		 * real values come from module->display_get when #195
		 * lands. */
		modes[i].kind   = GPU_MODE_TEXT;
		modes[i].width  = 80;
		modes[i].height = 25;
	}

	*displays = buf;
	*displays_count = (mach_msg_type_number_t)bytes;
	*n_displays = n;
	return KERN_SUCCESS;
}

/* ============================================================
 * Display ops — stubs in 0.1.0
 * ============================================================ */

kern_return_t
gpu_display_set_mode(mach_port_t gpu_port,
		     char *cap, mach_msg_type_number_t cap_count,
		     uint32_t dev_id, uint32_t disp_id,
		     uint32_t width, uint32_t height, uint32_t bpp)
{
	(void)gpu_port; (void)dev_id; (void)disp_id;
	(void)width; (void)height; (void)bpp;

	if (gpu_core_cap_check(cap, cap_count, GPU_CAP_DISPLAY_MODESET,
			       (uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;
	return KERN_FAILURE;
}

kern_return_t
gpu_display_scanout(mach_port_t gpu_port,
		    char *cap, mach_msg_type_number_t cap_count,
		    uint32_t dev_id, uint32_t disp_id, uint32_t bo_id)
{
	(void)gpu_port; (void)dev_id; (void)disp_id; (void)bo_id;

	if (gpu_core_cap_check(cap, cap_count, GPU_CAP_DISPLAY_SCANOUT,
			       (uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;
	return KERN_FAILURE;
}

/* ============================================================
 * Buffer-object ops — stubs in 0.1.0
 * ============================================================ */

kern_return_t
gpu_bo_alloc(mach_port_t gpu_port,
	     char *cap, mach_msg_type_number_t cap_count,
	     uint32_t dev_id, uint32_t size, uint32_t flags,
	     uint32_t *bo_id)
{
	(void)gpu_port; (void)dev_id; (void)size; (void)flags;

	if (gpu_core_cap_check(cap, cap_count, GPU_CAP_BO_RW,
			       (uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;
	*bo_id = 0;
	return KERN_FAILURE;
}

kern_return_t
gpu_bo_free(mach_port_t gpu_port,
	    char *cap, mach_msg_type_number_t cap_count,
	    uint32_t dev_id, uint32_t bo_id)
{
	(void)gpu_port; (void)dev_id; (void)bo_id;

	if (gpu_core_cap_check(cap, cap_count, GPU_CAP_BO_RW,
			       (uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;
	return KERN_FAILURE;
}

/* ============================================================
 * Context ops — stubs in 0.1.0
 * ============================================================ */

kern_return_t
gpu_context_create(mach_port_t gpu_port,
		   char *cap, mach_msg_type_number_t cap_count,
		   uint32_t dev_id, uint32_t *ctx_id)
{
	(void)gpu_port; (void)dev_id;

	if (gpu_core_cap_check(cap, cap_count, GPU_CAP_CTX_SUBMIT,
			       (uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;
	*ctx_id = 0;
	return KERN_FAILURE;
}

kern_return_t
gpu_context_destroy(mach_port_t gpu_port,
		    char *cap, mach_msg_type_number_t cap_count,
		    uint32_t dev_id, uint32_t ctx_id)
{
	(void)gpu_port; (void)dev_id; (void)ctx_id;

	if (gpu_core_cap_check(cap, cap_count, GPU_CAP_CTX_SUBMIT,
			       (uint64_t)dev_id) != 0)
		return KERN_PROTECTION_FAILURE;
	return KERN_FAILURE;
}

/* ============================================================
 * Diagnostics (#203)
 * ============================================================ */

kern_return_t
gpu_query_stats(mach_port_t gpu_port,
		char *cap, mach_msg_type_number_t cap_count,
		char *stats, mach_msg_type_number_t *stats_count)
{
	struct gpu_stats s;

	(void)gpu_port;

	/* Cap-gated: this surface exposes internal counters, so we
	 * restrict it to admin holders.  dev_id is irrelevant (the
	 * stats are server-wide); use 0 for the policy match. */
	if (gpu_core_cap_check(cap, cap_count, GPU_CAP_DEV_ADMIN, 0) != 0)
		return KERN_PROTECTION_FAILURE;

	memset(&s, 0, sizeof(s));
	s.version			   = GPU_STATS_VERSION;
	s.devices_attached		   = gpu_core_dev_count();
	s.text_render_drops		   = gpu_text_render_drops();
	s.text_render_chunks_processed = gpu_text_render_chunks_processed();
	s.scroll_count			   = gpu_core_sum_scrolls();

	memcpy(stats, &s, sizeof(s));
	*stats_count = (mach_msg_type_number_t)sizeof(s);
	return KERN_SUCCESS;
}
