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
		/* Same BDF: refresh the entry but report as duplicate so
		 * the caller does not fire a fresh hal_device_added.  #173
		 * relies on this to keep hal_rescan idempotent when no
		 * topology changed. */
		registry[existing] = *dev;
		return HAL_REGISTRY_ADD_EXISTING;
	}

	if (n_registry >= HAL_MAX_DEVICES) {
		printf("hal: registry full, dropping device %u:%u.%u\n",
		       dev->bus, dev->slot, dev->func);
		return HAL_REGISTRY_ADD_ERROR;
	}

	registry[n_registry++] = *dev;
	return HAL_REGISTRY_ADD_NEW;
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
hal_registry_set_status(unsigned int bus, unsigned int slot,
			unsigned int func, uint32_t status)
{
	int i = find_index(bus, slot, func);
	if (i < 0)
		return -1;
	registry[i].status = status;
	return 0;
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
