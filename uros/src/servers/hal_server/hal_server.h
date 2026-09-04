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
#include <pci_bar.h>		/* #427: struct pci_bar_region */

/* ================================================================
 * Limits
 * ================================================================ */

#define HAL_MAX_DEVICES		128
#define HAL_MAX_DRIVER_REGS	16
#define HAL_MAX_BARS		6

/* ================================================================
 * Which devices have drivers (#513)
 *
 * #427 removed a four-value `status' field because nothing could change it:
 * every scan wrote UNBOUND, hal_registry_set_status() was called by no code,
 * and its one consumer discarded it explicitly -- `(void)status;'.  🔑 A field
 * two RPCs report and nothing can change is worse than a field that is not
 * there, because a reader believes it.
 *
 * 🔴 SO THESE THREE VALUES EXIST ONLY BECAUSE EACH ONE IS WRITTEN BY SOMEBODY.
 * A driver reports the outcome of its probe through hal_report_probe(), which
 * is the RPC that did not exist and is why the old field could not work.  There
 * is deliberately no PROBING state: nothing would set it, and a fourth value
 * nobody writes would put back exactly what was removed.
 *
 * ⚠️ And BOUND is not taken on the driver's word.  Claiming a device is a
 * transaction with the KERNEL -- device_claim() in <device/device_master.defs>
 * -- so the HAL asks the kernel whether the device is really held before it
 * records that it is.  A registry that believed its clients would be a second
 * claim table able to disagree with the real one, which is the same failure
 * moved one server along.
 * ================================================================ */

#include <hal_state.h>		/* HAL_DEV_* -- shared with the drivers */

/* ================================================================
 * Device record
 *
 * This is the on-the-wire layout returned by hal_list_devices (OOL
 * buffer holds an array of these back-to-back).  Keep it POD and
 * fixed-size so the client can iterate without MIG typing help.
 *
 * ⚠️ Untyped means producer and consumer must come from the same build --
 * true before #427 and still true after it.  It is written down because the
 * regions below carry 64-bit members, which change this struct's alignment
 * and padding as well as its size: the half a reader does not see.
 * ================================================================ */

struct hal_device_info {
	uint32_t	bus;
	uint32_t	slot;
	uint32_t	func;
	uint32_t	vendor_device;		/* PCI 0x00 */
	uint32_t	class_rev;		/* PCI 0x08 */
	uint32_t	irq;			/* PCI 0x3C interrupt line */

	/*
	 * The device's regions, decoded (#427).
	 *
	 * 🔑 REGIONS, and a count, where this was `uint32_t bars[6]' -- the six
	 * raw slot values, filled on every scan and read by nothing.  Six slots
	 * are not six regions: a 64-bit memory BAR occupies two of them, so how
	 * many there are is not known until pci_bars_decode() has walked them,
	 * and n_bars is what makes that answer sayable at all.
	 *
	 * ⚠️ The old field could not have been corrected in place.  A decoded
	 * BAR is a 64-bit base plus a size plus the space type, and none of the
	 * three fits in a uint32_t -- fixing the loop while keeping the type
	 * would have written a truncated address more carefully, with every
	 * guard still green.
	 *
	 * `size' is zero until something probes it; see pci_bar.h for why a
	 * size is measured rather than read, and hal_registry_add() for why the
	 * measurement happens once.
	 */
	uint32_t		n_bars;
	struct pci_bar_region	bars[HAL_MAX_BARS];

	/*
	 * 🔴 WHICH DRIVER BOUND THIS DEVICE IS NOT IN HERE, and #513 put it
	 * here first and moved it out.  This record is what a SCAN produces --
	 * what the bus says about a device -- and hal_registry_add() refreshes
	 * it wholesale every time a rescan sees the same BDF.  A field the scan
	 * cannot produce, kept in the record the scan overwrites, survives only
	 * because somebody remembered to copy it across; and that is the defect
	 * #427 described in the field it removed, rebuilt with a guard in front
	 * of it.
	 *
	 * 🔑 Kept beside the registry instead, where the refresh cannot reach
	 * it: impossible rather than remembered.  hal_get_device_state() is how
	 * it is read, and it is a separate routine for a second reason -- this
	 * one changes while a machine runs and everything above it does not.
	 */
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

/*
 * hal_registry_add return codes.  Distinguishing NEW from EXISTING
 * lets the discovery loop fire hal_device_added only for newly
 * discovered devices, so #173 hal_rescan stays idempotent on a
 * stable bus.
 */
#define HAL_REGISTRY_ADD_ERROR		(-1)
#define HAL_REGISTRY_ADD_EXISTING	0
#define HAL_REGISTRY_ADD_NEW		1

int  hal_registry_add(const struct hal_device_info *dev);
/*
 * #173: trigger a fresh discovery pass on demand.  Reuses the same
 * machinery as the initial boot scan; only devices whose BDF was not
 * already in the registry generate hal_device_added notifications.
 * Safe to call repeatedly — the registry de-duplicates by BDF.
 */
void hal_run_discovery(void);
int  hal_registry_count(void);
const struct hal_device_info *hal_registry_get(unsigned int bus,
					       unsigned int slot,
					       unsigned int func);
/*
 * Copy the full registry into a caller-supplied buffer.  Returns the
 * number of entries copied.
 */
int  hal_registry_copy_all(struct hal_device_info *out, unsigned int max);

/* ================================================================
 * Which device has which driver (#513)
 * ================================================================ */

/*
 * Record the outcome a driver reported.  `driver_port' is remembered next to
 * the device -- not in the record above -- so a driver that dies can have its
 * devices released without asking anybody.
 */
int  hal_registry_set_driver_state(unsigned int bus, unsigned int slot,
				   unsigned int func, uint32_t state,
				   mach_port_t driver_port);

/* HAL_DEV_* for one device, or -1 if there is no such device. */
int  hal_registry_driver_state(unsigned int bus, unsigned int slot,
			       unsigned int func, uint32_t *state);

/*
 * Release every device bound by this driver port and answer how many.  Called
 * when the kernel says the port is dead.
 */
int  hal_registry_release_driver(mach_port_t driver_port);

/* Whether any driver is bound to this device -- for the rescan guard. */
int  hal_registry_is_bound(unsigned int bus, unsigned int slot,
			   unsigned int func);

/* ================================================================
 * Driver subscription API (hal_driver_reg.c)
 * ================================================================ */

int  hal_driver_reg_add(uint32_t class_mask, uint32_t class_match,
			mach_port_t driver_port);
/*
 * For each registered driver whose mask matches dev, send a
 * hal_device_added() notification on driver_port.
 */
void hal_driver_reg_notify_match(const struct hal_device_info *dev);
/*
 * Replay the full registry to one subscription (called once at register
 * time so new subscribers see devices that were already discovered).
 */
void hal_driver_reg_replay(int reg_slot);
/*
 * Release every subscription slot pointing at dead_port (invoked by
 * the demux when the kernel delivers a MACH_NOTIFY_DEAD_NAME).
 */
void hal_driver_reg_handle_dead_name(mach_port_t dead_port);
/*
 * Whether this port is one a driver registered with (#513).  A probe report
 * from a port the HAL never subscribed is refused.
 */
int  hal_driver_reg_known(mach_port_t port);

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
