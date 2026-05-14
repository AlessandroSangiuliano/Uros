/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * gpu_server/core.c — Resource registry + module orchestration.
 *
 * Owns:
 *   - the device table              (gpu_dev_id_t → struct gpu_device_entry)
 *   - the static-module registry    (back-end ops loaded at startup)
 *   - the discovery driver          (calls each module's probe/attach)
 *   - the cap_check stub            (real verification arrives in #197)
 *   - the text_puts internal sink   (text_render arrives in #196)
 *
 * 0.1.0 skeleton intentionally keeps tables flat and lock-free: the
 * gpu_server is single-threaded inside mach_msg_server today, and the
 * render thread that #196 introduces will own its own state.
 */

#include <mach.h>
#include <stdio.h>
#include <string.h>
#include <mach/cap_types.h>
#include "gpu_server.h"

/* ================================================================
 * Device table
 *
 * Slot 0 is reserved as "invalid handle"; live entries use ids 1..N.
 * ================================================================ */

static struct gpu_device_entry devices[GPU_MAX_DEVICES];
static unsigned int             n_devices;	/* count of live entries */

static struct gpu_device_entry *
alloc_dev_slot(void)
{
	unsigned int i;

	/* slot 0 reserved for "invalid" sentinel */
	for (i = 1; i < GPU_MAX_DEVICES; i++) {
		if (!devices[i].in_use) {
			memset(&devices[i], 0, sizeof(devices[i]));
			devices[i].in_use = 1;
			devices[i].id     = (gpu_dev_id_t)i;
			return &devices[i];
		}
	}
	return NULL;
}

struct gpu_device_entry *
gpu_core_dev_lookup(gpu_dev_id_t id)
{
	if (id == 0 || id >= GPU_MAX_DEVICES)
		return NULL;
	if (!devices[id].in_use)
		return NULL;
	return &devices[id];
}

unsigned int
gpu_core_dev_count(void)
{
	return n_devices;
}

uint64_t
gpu_core_sum_scrolls(void)
{
	uint64_t total = 0;
	unsigned int i;

	for (i = 1; i < GPU_MAX_DEVICES; i++) {
		if (!devices[i].in_use)
			continue;
		if (devices[i].module == NULL)
			continue;
		if (devices[i].module->get_scroll_count == NULL)
			continue;
		total += devices[i].module->get_scroll_count(devices[i].priv);
	}
	return total;
}

int
gpu_core_dev_copy_all(struct gpu_device_info *out, unsigned int max)
{
	unsigned int i, n = 0;

	for (i = 1; i < GPU_MAX_DEVICES && n < max; i++) {
		if (devices[i].in_use) {
			out[n++] = devices[i].info;
		}
	}
	return (int)n;
}

/* ================================================================
 * Discovery
 *
 * In 0.1.0 the vga module (#195) does not need HAL — it claims the
 * legacy VGA framebuffer unconditionally.  Future modules call
 * hal_list_devices on hal_port and probe each DISPLAY-class entry;
 * for now we just call every loaded module's probe() with a NULL
 * hint, matching the legacy-VGA fallback in design doc §8.1.
 * ================================================================ */

void
gpu_core_run_discovery(const gpu_module_ops_t * const *modules,
		       unsigned int n_modules,
		       mach_port_t hal_port)
{
	unsigned int m;

	(void)hal_port;	/* used by future modules; silenced for 0.1.0 */

	if (modules == NULL || n_modules == 0) {
		printf("gpu_server: no back-end modules loaded — discovery "
		       "skipped\n");
		return;
	}

	for (m = 0; m < n_modules; m++) {
		const gpu_module_ops_t *ops = modules[m];
		void *priv;
		struct gpu_device_entry *dev;

		if (ops == NULL)
			continue;
		if (ops->abi_version != GPU_MODULE_ABI_VERSION) {
			printf("gpu_server: rejecting module \"%s\" — "
			       "ABI %u != %u\n",
			       ops->name ? ops->name : "(unnamed)",
			       (unsigned)ops->abi_version,
			       (unsigned)GPU_MODULE_ABI_VERSION);
			continue;
		}
		if (ops->probe == NULL)
			continue;
		priv = ops->probe(NULL);
		if (priv == NULL)
			continue;

		dev = alloc_dev_slot();
		if (dev == NULL) {
			printf("gpu_server: device table full, "
			       "skipping \"%s\"\n", ops->name);
			if (ops->detach)
				ops->detach(priv);
			continue;
		}

		dev->module = ops;
		dev->priv   = priv;
		dev->info.id          = dev->id;
		dev->info.pci_bdf     = 0;
		dev->info.vendor_device = 0;
		dev->info.n_displays  = (ops->display_count != NULL)
					? ops->display_count(priv) : 0;
		strncpy(dev->info.module_name, ops->name,
			GPU_DEV_NAME_LEN - 1);
		dev->info.module_name[GPU_DEV_NAME_LEN - 1] = '\0';

		if (ops->attach != NULL && ops->attach(priv) < 0) {
			printf("gpu_server: module \"%s\" attach failed\n",
			       ops->name);
			dev->in_use = 0;
			continue;
		}

		n_devices++;
		printf("gpu_server: device %u attached "
		       "(module=\"%s\", n_displays=%u)\n",
		       (unsigned)dev->id, ops->name,
		       (unsigned)dev->info.n_displays);
	}
}

/* ================================================================
 * Capability check (#197)
 *
 * Routes every Mach RPC through urmach_cap_verify — same UrMach
 * fast-path block_device_server uses for partition I/O.  The token
 * blob the client passes over MIG is a wire-format `struct uros_cap`;
 * we memcpy into a stack copy and hand it to the kernel trap.
 *
 * Token-less callers (size 0) are accepted only for the special case
 * of the bootstrap log_forwarder before its cap is issued — that
 * path lands in gpu_text_render_enqueue with token_count==0 and is
 * silently allowed.  Every cap-bearing RPC enforces a non-zero
 * token of exactly sizeof(struct uros_cap).
 * ================================================================ */

extern kern_return_t urmach_cap_verify(const struct uros_cap *token,
				       uint32_t op,
				       uint64_t resource_id);

int
gpu_core_cap_check(const char *token, unsigned int token_count,
		   uint64_t op, uint64_t resource_id)
{
	struct uros_cap cap;
	kern_return_t kr;

	if (token == NULL || token_count == 0) {
		/* Pre-#198 bootstrap log path: no cap yet.  See
		 * gpu_text_render_enqueue for the only caller that
		 * relies on this. */
		return 0;
	}
	if (token_count != sizeof(struct uros_cap))
		return -1;

	memcpy(&cap, token, sizeof(cap));
	kr = urmach_cap_verify(&cap, (uint32_t)op, resource_id);
	if (kr != KERN_SUCCESS) {
		printf("gpu_server: cap_verify FAIL "
		       "(op=0x%llx res=0x%llx kr=%d)\n",
		       (unsigned long long)op,
		       (unsigned long long)resource_id, (int)kr);
		return -1;
	}
	return 0;
}

/* ================================================================
 * Per-cap handle table (#202)
 *
 * One row per (cap_id, dev_id) opened by a client.  device_open
 * inserts; device_close removes; cap_revoke_notify walks and frees
 * every row carrying the revoked cap_id.  The table is intentionally
 * small (GPU_MAX_HANDLES) — a single VGA in 0.1.0 has at most one
 * "real" client (the bootstrap log_forwarder), so 32 rows leave room
 * for the test harnesses that #197/#202 wires.
 *
 * Single-threaded server: lock-free reads/writes are correct here —
 * the dispatch loop in main.c serialises every MIG handler and the
 * cap_revoke_notify message lands on the same thread.  When the
 * render thread starts touching this table (post-0.2 BO lifetimes)
 * the access pattern grows a lock; today it does not.
 * ================================================================ */

struct gpu_handle {
	int		in_use;
	uint64_t	cap_id;
	gpu_dev_id_t	dev_id;
};

static struct gpu_handle handles[GPU_MAX_HANDLES];

static struct gpu_handle *
handle_lookup(uint64_t cap_id, gpu_dev_id_t dev_id)
{
	unsigned int i;

	for (i = 0; i < GPU_MAX_HANDLES; i++) {
		if (handles[i].in_use &&
		    handles[i].cap_id == cap_id &&
		    handles[i].dev_id == dev_id)
			return &handles[i];
	}
	return NULL;
}

int
gpu_core_handle_open(uint64_t cap_id, gpu_dev_id_t dev_id)
{
	unsigned int i;

	if (handle_lookup(cap_id, dev_id) != NULL) {
		/* Duplicate open of (cap, dev): caller is confused.
		 * Don't error — treat as no-op, the table still has the
		 * row and revocation will tear it down once. */
		return 0;
	}
	for (i = 0; i < GPU_MAX_HANDLES; i++) {
		if (!handles[i].in_use) {
			handles[i].in_use = 1;
			handles[i].cap_id = cap_id;
			handles[i].dev_id = dev_id;
			return 0;
		}
	}
	printf("gpu_server: handle table full, rejecting open "
	       "cap=%llu dev=%u\n",
	       (unsigned long long)cap_id, (unsigned)dev_id);
	return -1;
}

int
gpu_core_handle_close(uint64_t cap_id, gpu_dev_id_t dev_id)
{
	struct gpu_handle *h = handle_lookup(cap_id, dev_id);
	if (h == NULL)
		return -1;
	h->in_use = 0;
	h->cap_id = 0;
	h->dev_id = 0;
	return 0;
}

unsigned int
gpu_core_handle_revoke(uint64_t cap_id)
{
	unsigned int i, freed = 0;

	for (i = 0; i < GPU_MAX_HANDLES; i++) {
		if (handles[i].in_use && handles[i].cap_id == cap_id) {
			handles[i].in_use = 0;
			handles[i].cap_id = 0;
			handles[i].dev_id = 0;
			freed++;
		}
	}
	return freed;
}

/* ================================================================
 * Core init
 * ================================================================ */

int
gpu_core_init(void)
{
	memset(devices, 0, sizeof(devices));
	memset(handles, 0, sizeof(handles));
	n_devices = 0;
	return 0;
}

/* ================================================================
 * Text plane (skeleton sink)
 *
 * Until #196 ships text_render.c, this just forwards to printf so
 * we can verify clients are landing on us.  Truncated to one line
 * per call to keep the kernel console legible.
 * ================================================================ */

void
gpu_core_text_puts(const char *buf, size_t len)
{
	unsigned int i;

	if (buf == NULL || len == 0)
		return;

	/* Route to the first attached device whose module supports
	 * text_puts.  In 0.1.0 that's the vga module on dev_id 1; in
	 * 0.2+ a module may decline (text_puts == NULL) in which case
	 * core falls back to the kernel console so we never lose
	 * panic / log output. */
	for (i = 1; i < GPU_MAX_DEVICES; i++) {
		struct gpu_device_entry *dev = gpu_core_dev_lookup(i);
		if (dev == NULL)
			continue;
		if (dev->module == NULL || dev->module->text_puts == NULL)
			continue;
		(void)dev->module->text_puts(dev->priv, buf, len);
		return;
	}

	/* No back-end claimed text_puts — write through the bootstrap
	 * console so log output still reaches the user.  This keeps the
	 * skeleton observable when the build runs without modules. */
	{
		char line[128];
		size_t n = (len < sizeof(line) - 1) ? len : sizeof(line) - 1;
		memcpy(line, buf, n);
		line[n] = '\0';
		printf("gpu_server: text_puts(%u): %s\n", (unsigned)len, line);
	}
}
