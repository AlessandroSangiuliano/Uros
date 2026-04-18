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
 * hal_server.h — Hardware Abstraction Layer server
 *
 * The HAL is a discovery + resource-policy orchestrator.  It enumerates
 * hardware (PCI bus today, ACPI tomorrow), maintains an authoritative
 * device registry and matches devices to driver servers via a class-mask
 * subscription mechanism.  It does NOT touch hardware registers nor sit
 * in any data path — driver servers talk to kernel device_master RPCs
 * directly once HAL hands them a BDF.
 *
 * This header defines the runtime-loadable discovery module interface
 * (hal_discovery_ops) and the in-memory registry structures shared
 * between hal_server.c, hal_registry.c and hal_mig.c.
 */

#ifndef _HAL_SERVER_H_
#define _HAL_SERVER_H_

#include <mach.h>
#include <mach/mach_types.h>
#include <stdint.h>

/* ================================================================
 * Limits
 * ================================================================ */

#define HAL_MAX_DEVICES		128
#define HAL_MAX_DRIVER_REGS	16
#define HAL_MAX_BARS		6

/* ================================================================
 * Device status
 * ================================================================ */

#define HAL_DEV_UNBOUND		0	/* discovered, no driver claimed yet */
#define HAL_DEV_PROBING		1	/* driver is probing */
#define HAL_DEV_ACTIVE		2	/* driver took ownership */
#define HAL_DEV_ERROR		3	/* probe failed */

/* ================================================================
 * Device record
 *
 * This is the on-the-wire layout returned by hal_list_devices (OOL
 * buffer holds an array of these back-to-back).  Keep it POD and
 * fixed-size so the client can iterate without MIG typing help.
 * ================================================================ */

struct hal_device_info {
	uint32_t	bus;
	uint32_t	slot;
	uint32_t	func;
	uint32_t	vendor_device;		/* PCI 0x00 */
	uint32_t	class_rev;		/* PCI 0x08 */
	uint32_t	irq;			/* PCI 0x3C interrupt line */
	uint32_t	bars[HAL_MAX_BARS];	/* PCI 0x10..0x24 */
	uint32_t	status;			/* HAL_DEV_* */
};

/* ================================================================
 * Driver subscription
 *
 * A driver server that wants to be notified about a class of devices
 * calls hal_register_driver(class_mask, class_match, port).  A match
 * occurs when (dev.class_rev & class_mask) == class_match.
 * ================================================================ */

struct hal_driver_reg {
	uint32_t	class_mask;
	uint32_t	class_match;
	mach_port_t	driver_port;
	int		in_use;
};

/* ================================================================
 * Discovery module interface
 *
 * Modules live under /mach_servers/modules/hal/<name>.so.  Each
 * module exports a symbol <name>_discovery_ops of this type; the
 * loader (libmodload) strips ".so" and appends the suffix.
 *
 * init()  — one-time setup; the module stashes master_device if it
 *           needs kernel PCI config RPCs.
 * scan()  — fills up to max entries in out[] and returns how many
 *           devices were discovered (>=0) or -1 on fatal error.
 * ================================================================ */

struct hal_discovery_ops {
	const char	*name;
	int  (*init)(mach_port_t master_device);
	int  (*scan)(struct hal_device_info *out, unsigned int max);
};

/* ================================================================
 * Registry API (hal_registry.c)
 * ================================================================ */

int  hal_registry_add(const struct hal_device_info *dev);
int  hal_registry_count(void);
const struct hal_device_info *hal_registry_get(unsigned int bus,
					       unsigned int slot,
					       unsigned int func);
int  hal_registry_set_status(unsigned int bus, unsigned int slot,
			     unsigned int func, uint32_t status);
/*
 * Copy the full registry into a caller-supplied buffer.  Returns the
 * number of entries copied.
 */
int  hal_registry_copy_all(struct hal_device_info *out, unsigned int max);

/* ================================================================
 * Driver subscription API (hal_driver_reg.c)
 * ================================================================ */

int  hal_driver_reg_add(uint32_t class_mask, uint32_t class_match,
			mach_port_t driver_port);
/*
 * For each registered driver whose mask matches dev, send a match
 * notification on driver_port.  Phase 1: best-effort log only —
 * actual notification message is a stub (see hal_driver_reg.c).
 */
void hal_driver_reg_notify_match(const struct hal_device_info *dev);

/* ================================================================
 * Global state (defined in hal_server.c)
 * ================================================================ */

extern mach_port_t	host_port;
extern mach_port_t	device_port;
extern mach_port_t	master_device;
extern mach_port_t	security_port;
extern mach_port_t	root_ledger_wired;
extern mach_port_t	root_ledger_paged;
extern mach_port_t	hal_service_port;

#endif /* _HAL_SERVER_H_ */
