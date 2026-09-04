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
 * hal_registry.c — HAL device registry
 *
 * Flat array of hal_device_info records keyed by PCI BDF.  Single-
 * threaded server (MIG demux) so no locking is needed.
 */

#include <string.h>
#include <stdio.h>
#include "hal_server.h"

/*
 * ── One entry, two halves, and only one of them travels (#513) ───────
 *
 * 🔴 THE DRIVER'S HALF IS NOT IN struct hal_device_info, deliberately.  That
 * record is what a SCAN produces and hal_registry_add() refreshes it wholesale
 * every time a rescan sees the same BDF -- so a field the scan cannot produce,
 * kept in it, survives only because somebody remembered to copy it across.  That
 * is the defect #427 described in the `status' field it removed, rebuilt with a
 * guard in front of it.  Out here the refresh cannot reach it at all.
 *
 * 🔑 AND IN THE SAME STRUCT RATHER THAN IN PARALLEL ARRAYS, which is where this
 * went first.  Three arrays keyed by position agree only while nothing is ever
 * removed or reordered -- an invariant this file happened to satisfy and did not
 * state, which is the same kind of promise the record-refresh above was.  One
 * entry cannot desynchronise from itself.
 *
 * ⚠️ `port' also has to stay off the wire for a second reason: hal_list_devices
 * hands `info' out as untyped out-of-line bytes, and a port name copied that way
 * is a number in the HAL's namespace that means nothing in the reader's.  A
 * right cannot travel in a byte array.
 */
struct hal_entry {
	struct hal_device_info	info;	/* what the scan found -- on the wire */
	uint32_t		state;	/* HAL_DEV_*, what a driver reported  */
	mach_port_t		port;	/* who reported it; the HAL's own     */
};

static struct hal_entry registry[HAL_MAX_DEVICES];
static int n_registry;

static int
find_index(unsigned int bus, unsigned int slot, unsigned int func)
{
	int i;
	for (i = 0; i < n_registry; i++) {
		if (registry[i].info.bus == bus &&
		    registry[i].info.slot == slot &&
		    registry[i].info.func == func)
			return i;
	}
	return -1;
}

int
hal_registry_add(const struct hal_device_info *dev)
{
	int existing;

	if (dev == NULL)
		return HAL_REGISTRY_ADD_ERROR;

	existing = find_index(dev->bus, dev->slot, dev->func);
	if (existing >= 0) {
		/*
		 * Same BDF: refresh the entry but report as duplicate so
		 * the caller does not fire a fresh hal_device_added.  #173
		 * relies on this to keep hal_rescan idempotent when no
		 * topology changed.
		 *
		 * 🔑 AND IT ASSIGNS `.info' RATHER THAN THE ENTRY (#513).  A
		 * scan reports what the bus says; what a driver reported is in
		 * the same struct and outside this assignment, so the refresh
		 * cannot lose it and nobody has to remember that it must not.
		 */
		registry[existing].info = *dev;
		return HAL_REGISTRY_ADD_EXISTING;
	}

	if (n_registry >= HAL_MAX_DEVICES) {
		printf("hal: registry full, dropping device %u:%u.%u\n",
		       dev->bus, dev->slot, dev->func);
		return HAL_REGISTRY_ADD_ERROR;
	}

	registry[n_registry].info = *dev;
	registry[n_registry].state = HAL_DEV_UNCLAIMED;
	registry[n_registry].port = MACH_PORT_NULL;
	n_registry++;
	return HAL_REGISTRY_ADD_NEW;
}

int
hal_registry_set_driver_state(unsigned int bus, unsigned int slot,
			      unsigned int func, uint32_t state,
			      mach_port_t port)
{
	int i = find_index(bus, slot, func);

	if (i < 0)
		return -1;

	registry[i].state = state;

	/*
	 * ⚠️ The port is remembered only for a device that is HELD.  A driver
	 * that reported a failure has given the device back -- see the
	 * cap_revoke in block_server.c -- so keeping its port here would make
	 * a later dead-name release a device it does not have.
	 */
	registry[i].port = (state == HAL_DEV_BOUND) ? port : MACH_PORT_NULL;
	return 0;
}

int
hal_registry_driver_state(unsigned int bus, unsigned int slot,
			  unsigned int func, uint32_t *state)
{
	int i = find_index(bus, slot, func);

	if (i < 0)
		return -1;

	*state = registry[i].state;
	return 0;
}

int
hal_registry_is_bound(unsigned int bus, unsigned int slot, unsigned int func)
{
	int i = find_index(bus, slot, func);

	return i >= 0 && registry[i].state == HAL_DEV_BOUND;
}

int
hal_registry_release_driver(mach_port_t port)
{
	int i, released = 0;

	if (port == MACH_PORT_NULL)
		return 0;

	for (i = 0; i < n_registry; i++) {
		if (registry[i].port != port)
			continue;

		printf("hal: %u:%u.%u released — the driver that bound it is "
		       "gone\n", registry[i].info.bus, registry[i].info.slot,
		       registry[i].info.func);

		registry[i].state = HAL_DEV_UNCLAIMED;
		registry[i].port = MACH_PORT_NULL;
		released++;
	}

	return released;
}

int
hal_registry_count(void)
{
	return n_registry;
}

const struct hal_device_info *
hal_registry_get(unsigned int bus, unsigned int slot, unsigned int func)
{
	int i = find_index(bus, slot, func);
	return (i >= 0) ? &registry[i].info : NULL;
}

int
hal_registry_copy_all(struct hal_device_info *out, unsigned int max)
{
	unsigned int n, i;

	if (out == NULL)
		return 0;

	n = (unsigned int)n_registry;
	if (n > max)
		n = max;

	/*
	 * ⚠️ Copied one at a time, and that is the cost of the entry above:
	 * `info' is no longer the whole element, so a single memcpy of the
	 * array would hand the caller this server's port names as if they were
	 * device fields.  128 entries is not a loop worth avoiding.
	 */
	for (i = 0; i < n; i++)
		out[i] = registry[i].info;

	return (int)n;
}
