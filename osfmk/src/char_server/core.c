/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * char_server/core.c — resource registry + module orchestration.
 *
 * Mirror gpu_server/core.c.  Single-threaded (mach_msg_server is the
 * only writer); modules' IRQ paths run on threads they spawn
 * themselves and only call back via mach_msg sends to subscribers.
 */

#include <mach.h>
#include <mach/cap_types.h>
#include <stdio.h>
#include <string.h>
#include "char_server.h"

extern kern_return_t urmach_cap_verify(const struct uros_cap *token,
				       uint32_t op,
				       uint64_t resource_id);

/* ============================================================
 * Device table
 * ============================================================ */

static struct char_device_entry devices[CHAR_MAX_DEVICES];
static unsigned int             n_devices;

static struct char_device_entry *
alloc_dev_slot(void)
{
	unsigned int i;
	for (i = 1; i < CHAR_MAX_DEVICES; i++) {	/* slot 0 = invalid */
		if (!devices[i].in_use) {
			memset(&devices[i], 0, sizeof(devices[i]));
			devices[i].in_use = 1;
			devices[i].id     = (char_dev_id_t)i;
			return &devices[i];
		}
	}
	return NULL;
}

struct char_device_entry *
char_core_dev_lookup(char_dev_id_t id)
{
	if (id == 0 || id >= CHAR_MAX_DEVICES)
		return NULL;
	if (!devices[id].in_use)
		return NULL;
	return &devices[id];
}

unsigned int
char_core_dev_count(void)
{
	return n_devices;
}

int
char_core_dev_copy_all(struct char_device_info *out, unsigned int max)
{
	unsigned int i, n = 0;
	for (i = 1; i < CHAR_MAX_DEVICES && n < max; i++) {
		if (devices[i].in_use)
			out[n++] = devices[i].info;
	}
	return (int)n;
}

/* ============================================================
 * Discovery
 * ============================================================ */

void
char_core_run_discovery(const char_module_ops_t * const *modules,
			unsigned int n_modules,
			mach_port_t hal_port)
{
	unsigned int m;

	(void)hal_port;	/* future modules (usb_hid_kbd) will probe via HAL */

	if (modules == NULL || n_modules == 0) {
		printf("char_server: no back-end modules loaded — discovery "
		       "skipped\n");
		return;
	}

	for (m = 0; m < n_modules; m++) {
		const char_module_ops_t *ops = modules[m];
		void *priv;
		struct char_device_entry *dev;

		if (ops == NULL)
			continue;
		if (ops->abi_version != CHAR_MODULE_ABI_VERSION) {
			printf("char_server: rejecting module \"%s\" — ABI "
			       "%u != %u\n",
			       ops->name ? ops->name : "(unnamed)",
			       (unsigned)ops->abi_version,
			       (unsigned)CHAR_MODULE_ABI_VERSION);
			continue;
		}
		if (ops->probe == NULL)
			continue;
		priv = ops->probe(NULL);
		if (priv == NULL)
			continue;

		dev = alloc_dev_slot();
		if (dev == NULL) {
			printf("char_server: device table full, dropping "
			       "\"%s\"\n", ops->name);
			if (ops->detach)
				ops->detach(priv);
			continue;
		}

		dev->module = ops;
		dev->priv   = priv;
		dev->info.id    = dev->id;
		dev->info.class = ops->device_class;
		dev->info.flags = 0;
		strncpy(dev->info.module_name, ops->name,
			CHAR_DEV_NAME_LEN - 1);
		dev->info.module_name[CHAR_DEV_NAME_LEN - 1] = '\0';

		if (ops->attach != NULL && ops->attach(priv) < 0) {
			printf("char_server: module \"%s\" attach failed\n",
			       ops->name);
			dev->in_use = 0;
			continue;
		}

		n_devices++;
		printf("char_server: device %u attached "
		       "(module=\"%s\", class=%u)\n",
		       (unsigned)dev->id, ops->name,
		       (unsigned)dev->info.class);
	}
}

/* ============================================================
 * Capability check.  Empty tokens rejected — every char_server entry
 * point is cap-gated, no early-boot exception path (cap_server is
 * always up before any client could plausibly want to talk to us:
 * char_server boots after cap_server in bootstrap.conf).
 * ============================================================ */

int
char_core_cap_check(const char *token, unsigned int token_count,
		    uint64_t op, uint64_t resource_id)
{
	struct uros_cap cap;
	kern_return_t kr;

	if (token == NULL || token_count != sizeof(struct uros_cap))
		return -1;

	memcpy(&cap, token, sizeof(cap));
	kr = urmach_cap_verify(&cap, (uint32_t)op, resource_id);
	if (kr != KERN_SUCCESS) {
		printf("char_server: cap_verify FAIL "
		       "(op=0x%llx res=0x%llx kr=%d)\n",
		       (unsigned long long)op,
		       (unsigned long long)resource_id, (int)kr);
		return -1;
	}
	return 0;
}

/* ============================================================
 * Init
 * ============================================================ */

int
char_core_init(void)
{
	memset(devices, 0, sizeof(devices));
	n_devices = 0;
	return 0;
}
