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

static struct hal_device_info registry[HAL_MAX_DEVICES];
static int n_registry;

/*
 * Who reported binding each device (#513).
 *
 * 🔑 BESIDE THE RECORD AND NOT IN IT.  hal_list_devices hands the record out as
 * untyped out-of-line bytes, and a port name copied that way is a number in the
 * HAL's namespace that means nothing in the reader's -- a right cannot travel
 * in a byte array.  What the reader is owed is the STATE, which is in the
 * record; who holds the device is the HAL's own business, and this is where it
 * keeps it.
 */
static mach_port_t driver_port[HAL_MAX_DEVICES];
static uint32_t    driver_state[HAL_MAX_DEVICES];

static int
find_index(unsigned int bus, unsigned int slot, unsigned int func)
{
	int i;
	for (i = 0; i < n_registry; i++) {
		if (registry[i].bus == bus &&
		    registry[i].slot == slot &&
		    registry[i].func == func)
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
		 * 🔑 AND IT STAYS WHOLESALE, which is the point of keeping the
		 * driver state out of this record (#513).  A scan reports what
		 * the bus says; who bound the device is not in here to be
		 * overwritten, so the refresh cannot lose it and nobody has to
		 * remember that it must not.
		 *
		 * ⚠️ That is the defect #427 described in the `status' field it
		 * removed -- a rescan resetting a value on a device already
		 * bound -- and it could not be demonstrated there because the
		 * value the refresh overwrote was the value it wrote.  Fixing
		 * it by copying the field across would have left the same line
		 * one edit away from doing it again.
		 */
		registry[existing] = *dev;
		return HAL_REGISTRY_ADD_EXISTING;
	}

	if (n_registry >= HAL_MAX_DEVICES) {
		printf("hal: registry full, dropping device %u:%u.%u\n",
		       dev->bus, dev->slot, dev->func);
		return HAL_REGISTRY_ADD_ERROR;
	}

	registry[n_registry] = *dev;
	driver_state[n_registry] = HAL_DEV_UNCLAIMED;
	driver_port[n_registry] = MACH_PORT_NULL;
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

	driver_state[i] = state;

	/*
	 * ⚠️ The port is remembered only for a device that is HELD.  A driver
	 * that reported a failure has given the device back -- see the
	 * cap_revoke in block_server.c -- so keeping its port here would make
	 * a later dead-name release a device it does not have.
	 */
	driver_port[i] = (state == HAL_DEV_BOUND) ? port : MACH_PORT_NULL;
	return 0;
}

int
hal_registry_driver_state(unsigned int bus, unsigned int slot,
			  unsigned int func, uint32_t *state)
{
	int i = find_index(bus, slot, func);

	if (i < 0)
		return -1;

	*state = driver_state[i];
	return 0;
}

int
hal_registry_is_bound(unsigned int bus, unsigned int slot, unsigned int func)
{
	int i = find_index(bus, slot, func);

	return i >= 0 && driver_state[i] == HAL_DEV_BOUND;
}

int
hal_registry_release_driver(mach_port_t port)
{
	int i, released = 0;

	if (port == MACH_PORT_NULL)
		return 0;

	for (i = 0; i < n_registry; i++) {
		if (driver_port[i] != port)
			continue;

		printf("hal: %u:%u.%u released — the driver that bound it is "
		       "gone\n", registry[i].bus, registry[i].slot,
		       registry[i].func);

		driver_state[i] = HAL_DEV_UNCLAIMED;
		driver_port[i] = MACH_PORT_NULL;
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
	return (i >= 0) ? &registry[i] : NULL;
}

int
hal_registry_copy_all(struct hal_device_info *out, unsigned int max)
{
	unsigned int n;

	if (out == NULL)
		return 0;

	n = (unsigned int)n_registry;
	if (n > max)
		n = max;
	if (n != 0)
		memcpy(out, registry, n * sizeof(*out));
	return (int)n;
}
