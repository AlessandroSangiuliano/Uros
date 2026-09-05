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
	if (existing >= 0
	    && registry[existing].info.vendor_device == dev->vendor_device) {
		/*
		 * ── Same BDF, same device: the entry is left ALONE (#427) ────
		 *
		 * 🔴 IT USED TO BE REFRESHED WHOLESALE, and that cannot survive
		 * a record with a MEASURED field in it.  A scan cannot produce
		 * a size -- sizes are probed, once, because probing writes to
		 * the device -- so a refresh from a fresh scan would put zeros
		 * back over every size the HAL had measured, on the first
		 * hal_rescan of the boot.  The version of this that copies them
		 * across is a step somebody has to remember; not assigning is a
		 * step that cannot be forgotten.
		 *
		 * 🔑 And nothing is lost, because THIS RECORD IS THE MASTER
		 * COPY AND NOT A CACHE.  Configuration space holds where a
		 * device must be, not where it is: a reset or a D3 transition
		 * zeroes the BARs, and the right value afterwards is the one
		 * that was saved here, not the one the bus now reports.  The
		 * refresh was writing back what it had just read from the same
		 * place, which is why removing it changes nothing observable.
		 *
		 * The caller still hears EXISTING, so #173 keeps its property:
		 * a rescan over a stable bus fires no hal_device_added.
		 */
		return HAL_REGISTRY_ADD_EXISTING;
	}

	if (existing >= 0) {
		/*
		 * ⚠️ Same BDF, DIFFERENT device.  Not a refresh: whatever this
		 * entry described is gone, and everything the HAL knows about
		 * it -- the driver that bound it, the sizes it measured -- is
		 * about a device that is no longer there.
		 *
		 * 🔑 Reported as NEW rather than as a duplicate, and said out
		 * loud.  It is the only path on which re-measuring a known BDF
		 * is the right thing to do, and the subscribers have to hear
		 * about a device they have never been told about.
		 */
		printf("hal: %u:%u.%u changed device: 0x%08x is now 0x%08x\n",
		       dev->bus, dev->slot, dev->func,
		       registry[existing].info.vendor_device,
		       dev->vendor_device);

		registry[existing].info = *dev;
		registry[existing].state = HAL_DEV_UNCLAIMED;
		registry[existing].port = MACH_PORT_NULL;
		return HAL_REGISTRY_ADD_NEW;
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

/*
 * ── The sizes, written back after the one measurement (#427) ─────────
 *
 * 🔑 ONLY THE SIZES.  The caller hands over the region list it measured, and
 * this takes the one field the measurement produced and nothing else: a region
 * list that came back differing in any other way is a device that changed
 * underneath the probe, and copying the whole list would let that overwrite the
 * addresses the registry is the master copy of.
 *
 * ⚠️ Matched by the SLOT a region starts at, not by its index.  The two are
 * different numbers on any device with a 64-bit BAR -- a region at index 1 can
 * start at slot 2 -- and indexing was the shape of the defect in ahci_module.c
 * this issue had to fix once already.
 */
int
hal_registry_set_sizes(unsigned int bus, unsigned int slot, unsigned int func,
		       const struct pci_bar_region *measured, unsigned int n)
{
	int		i = find_index(bus, slot, func);
	unsigned int	m, k;

	if (i < 0 || measured == NULL)
		return -1;

	for (m = 0; m < n; m++)
		for (k = 0; k < registry[i].info.n_bars; k++)
			if (registry[i].info.bars[k].slot == measured[m].slot)
				registry[i].info.bars[k].size = measured[m].size;

	return 0;
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
