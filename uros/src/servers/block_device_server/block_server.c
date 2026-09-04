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
 * block_server.c — Modular block device server: main + HAL subscription
 *
 * The server no longer walks the PCI bus itself.  At boot it looks up the
 * HAL service in the name server and calls hal_register_driver() with the
 * mass-storage class mask.  The HAL replays the current device registry
 * over the hal_notify subsystem (one hal_device_added() simpleroutine per
 * matching device); each notification lands in hal_device_added() below,
 * which runs the loaded driver modules and probes the matching one.
 *
 * After the initial replay has been drained, partitions are registered
 * with the name server and the server enters its regular message loop
 * (device RPC + ahci_batch + IRQ notifications + hotplug notifications).
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/thread_switch.h>		/* #460: wait for HAL to check in */
#include <mach/bootstrap.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <mach/mig_errors.h>
#include <sa_mach.h>
#include <device/device.h>
#include <device/device_types.h>
#include <servers/netname.h>
#include <libcap.h>			/* #432: a capability for a device's class */
#include "device_master.h"	/* #432: device_claim */
#include <stdio.h>
#include <string.h>
#include "block_server.h"
#include "device_master.h"
#include "device_server.h"
#include "ahci_batch_server.h"
#include "hal.h"
#include <hal_state.h>	/* #513: HAL_DEV_* -- the same three numbers the HAL sets */
#include "hal_notify_server.h"
#include "cap_revoke_server.h"
#include "gpu_console.h"
#include "modload.h"

extern kern_return_t cap_subscribe_revoke(mach_port_t notify_port);

/* ================================================================
 * Global state
 * ================================================================ */

mach_port_t	host_port;
mach_port_t	device_port;
mach_port_t	master_device;
mach_port_t	security_port;
mach_port_t	root_ledger_wired;
mach_port_t	root_ledger_paged;
mach_port_t	port_set;

struct blk_controller	controllers[MAX_CONTROLLERS];
int			n_controllers;

struct blk_partition	partitions[MAX_PARTITIONS];
int			n_partitions;

/* HAL subscription */
static mach_port_t	hal_service_port;	/* HAL server send right */
static mach_port_t	hal_driver_port;	/* our receive right */

/* Mass-storage PCI class (byte 3 of class_rev = 0x01).  class_mask picks
 * the high byte, class_match asks for 0x01xxxxxx. */
#define BLK_CLASS_MASK		0xFF000000u
#define BLK_CLASS_MATCH		0x01000000u

/* ================================================================
 * Module table — dynamically loaded driver modules
 * ================================================================ */

#define MAX_MODULES	16

static const struct block_driver_ops *modules[MAX_MODULES + 1];
static int n_modules;

/* Forward declaration */
static int claim_device_for_driver(unsigned int bus, unsigned int slot,
				   unsigned int func, unsigned int class_rev,
				   uint64_t *cap_id_out);
static void pci_probe_module(const struct block_driver_ops *ops,
			     unsigned int bus, unsigned int slot,
			     unsigned int func, uint64_t claimed_cap_id);
static void report_and_read_back(unsigned int bus, unsigned int slot,
				 unsigned int func, unsigned int outcome);
static void rescan_leaves_bound_devices_alone(void);
static void claim_this_server_does_not_have(void);

/* ================================================================
 * HAL notification callback — invoked by hal_notify_server()
 * whenever the HAL delivers a hal_device_added() RPC.
 *
 * For every loaded module, run its match() against the device class;
 * on the first hit, probe the hardware and parse its disks' MBRs.
 * ================================================================ */

kern_return_t
hal_device_added(mach_port_t driver_port,
		 unsigned int bus, unsigned int slot, unsigned int func,
		 unsigned int vendor_device, unsigned int class_rev,
		 unsigned int irq)
{
	int m;
	uint64_t claimed_cap_id = 0;

	(void)driver_port;
	(void)irq;

	if (n_controllers >= MAX_CONTROLLERS) {
		printf("blk: controller table full, ignoring %u:%u.%u\n",
		       bus, slot, func);
		return KERN_SUCCESS;
	}

	for (m = 0; modules[m] != NULL; m++) {
		const struct block_driver_ops *ops = modules[m];

		if (!ops->match(vendor_device, class_rev))
			continue;

		printf("blk: HAL event %u:%u.%u vendor=0x%08x "
		       "class=0x%08x matched by %s\n",
		       bus, slot, func, vendor_device, class_rev, ops->name);

		/*
		 * 🔴 CLAIMED BEFORE PROBED, and refused means not probed.
		 * Matching a driver to a device is this server saying which
		 * hardware it thinks it can handle; the claim is the KERNEL
		 * saying whether it may.  A probe that ran first would have
		 * touched the device -- reset it, mapped its registers, armed
		 * its interrupt -- before anyone asked.
		 */
		if (!claim_device_for_driver(bus, slot, func, class_rev,
					     &claimed_cap_id)) {
			printf("blk: %u:%u.%u NOT claimed — %s will not probe "
			       "it\n", bus, slot, func, ops->name);
			break;
		}

		pci_probe_module(ops, bus, slot, func, claimed_cap_id);
		break;
	}

	return KERN_SUCCESS;
}


/*
 * ── May this server drive this device? (#432) ────────────────────────
 *
 * 🔑 THE TOKEN IS FOR THE CLASS AND THE CLAIM IS FOR THE INSTANCE, which is
 * the split the manifest cannot make on its own: a policy file knows what kind
 * of hardware a driver is for, and cannot know what is plugged in.  So this
 * asks cap_server for a capability covering the device's CLASS -- out of this
 * server's own manifest -- and hands it to the kernel, which reads the class
 * back out of the device's configuration space and requires the two to agree.
 *
 * ⚠️ `class_rev' is the register: class code in 31:8, revision in 7:0.  The
 * capability names the class, so the revision goes -- a driver is for a kind
 * of device and not for a stepping of it.
 *
 * ⚠️ A machine with no manifest shipped gets a token for the asking, and this
 * passes.  Enforcement is always on; how strict it is, is what the policy file
 * decides.  The alternative -- turning the check off when there is no policy
 * -- is a mechanism that is absent exactly when somebody starts relying on it.
 */
static int
claim_device_for_driver(unsigned int bus, unsigned int slot, unsigned int func,
			unsigned int class_rev, uint64_t *cap_id_out)
{
	struct uros_cap	tok;
	kern_return_t	kr;
	natural_t	bdf = (natural_t)((bus << 8) | (slot << 3) | func);

	memset(&tok, 0, sizeof(tok));

	kr = cap_request(RESOURCE_PCI_DEVICE, (uint64_t)(class_rev >> 8),
			 CAP_OP_PCI_DMA_MAP | CAP_OP_PCI_MMIO_MAP
			 | CAP_OP_PCI_IRQ, 0, &tok);
	if (kr != KERN_SUCCESS) {
		printf("blk: cap_server would not issue a capability for "
		       "class 0x%06x (kr=%d)\n", class_rev >> 8, (int)kr);
		return 0;
	}

	kr = device_claim(master_device, bdf, (char *)&tok, sizeof(tok));
	if (kr != KERN_SUCCESS) {
		printf("blk: the kernel refused this server the claim on "
		       "%u:%u.%u (kr=%d)\n", bus, slot, func, (int)kr);
		return 0;
	}

	/*
	 * ⚠️ ONE CAPABILITY PER CONTROLLER, and it matters that cap_server
	 * mints a fresh one on every request even for the same class.  Giving
	 * a device back means revoking the capability behind it, and one token
	 * shared between two controllers would take both.
	 */
	if (cap_id_out != NULL)
		*cap_id_out = tok.cap_id;

	return 1;
}

/*
 * ── Telling the HAL what happened, and reading it back (#513) ────────
 *
 * 🔴 THE READ-BACK IS THE POINT, and it is what the removed `status' field
 * could never have produced.  That one was reported by two RPCs and set by
 * nothing, so every reader saw its initial value and believed it.  What shows
 * this one carries information is a client reading a value a client SET -- and
 * a second device, in the same breath, still reading the initial one.
 *
 * ⚠️ 0:0.0 is the control and it is chosen rather than convenient: the host
 * bridge is on every board this runs on, it is in the registry because the scan
 * found it, and no driver will ever claim it.  A control that was absent would
 * make this arm pass by returning an error, which is the shape of proof this
 * whole issue exists to avoid.
 */
static void
report_and_read_back(unsigned int bus, unsigned int slot, unsigned int func,
		     unsigned int outcome)
{
	kern_return_t	kr;
	unsigned int	got = 0, control = 0;

	kr = hal_report_probe(hal_service_port, bus, slot, func,
			      hal_driver_port, outcome);
	if (kr != KERN_SUCCESS) {
		printf("blk: the HAL would not record %u:%u.%u as %u "
		       "(kr=%d)\n", bus, slot, func, outcome, (int)kr);
		return;
	}

	if (hal_get_device_state(hal_service_port, bus, slot, func, &got)
	    != KERN_SUCCESS) {
		printf("blk: the HAL has no state for %u:%u.%u\n",
		       bus, slot, func);
		return;
	}

	if (hal_get_device_state(hal_service_port, 0, 0, 0, &control)
	    != KERN_SUCCESS)
		control = (unsigned int)-1;

	if (got == outcome && control == HAL_DEV_UNCLAIMED)
		printf("blk: the HAL says %u:%u.%u is %u and 0:0.0 is still "
		       "%u — the registry reports a value a client set, and "
		       "one it did not\n",
		       bus, slot, func, got, control);
	else
		printf("blk: WRONG — reported %u for %u:%u.%u, read back %u; "
		       "the unclaimed control 0:0.0 reads %u\n",
		       outcome, bus, slot, func, got, control);

	if (outcome == HAL_DEV_BOUND)
		claim_this_server_does_not_have();
}

/*
 * ── Making the HAL's check fire, rather than assuming it would (#513) ─
 *
 * 🔴 A GUARD THAT IS ALWAYS TRUE IS WORSE THAN NO GUARD.  Every report this
 * server makes is honest, so the HAL's "ask the kernel whether it is really
 * held" would answer yes on every boot for ever and never be seen to do
 * anything.  The only way to know it discriminates is to make it say no.
 *
 * So: report BOUND for the host bridge, which this server has not claimed and
 * would not know what to do with.  The report is well formed and comes from a
 * registered driver port -- everything the HAL could check about the REPORTER
 * passes -- so the only thing that can refuse it is the kernel's answer about
 * the DEVICE.
 *
 * ⚠️ And the refusal is confirmed by a reading, not by a return code alone: a
 * call that failed for some other reason returns an error too.  0:0.0 must
 * still read UNCLAIMED afterwards, which says the registry was not written.
 */
static void
claim_this_server_does_not_have(void)
{
	kern_return_t	kr;
	unsigned int	state = (unsigned int)-1;

	kr = hal_report_probe(hal_service_port, 0, 0, 0, hal_driver_port,
			      HAL_DEV_BOUND);

	if (hal_get_device_state(hal_service_port, 0, 0, 0, &state)
	    != KERN_SUCCESS)
		state = (unsigned int)-1;

	if (kr != KERN_SUCCESS && state == HAL_DEV_UNCLAIMED)
		printf("blk: the HAL refused this server's claim on 0:0.0 "
		       "(kr=%d) and left it %u — it checks the kernel and does "
		       "not take a driver's word\n", (int)kr, state);
	else
		printf("blk: WRONG — the HAL accepted a claim on 0:0.0 that "
		       "this server does not hold (kr=%d), and it now reads "
		       "%u\n", (int)kr, state);
}

/*
 * ── A rescan must not forget who owns what (#513) ────────────────────
 *
 * 🔴 THE DEFECT THIS ANSWERS COULD NOT BE OBSERVED BEFORE.  hal_registry_add()
 * refreshes a known device wholesale on every scan, and this server calls
 * hal_rescan() on every boot -- so any field the scan does not produce was
 * silently reset.  #427 found that reasoning about the `status' field it was
 * removing and could not demonstrate it: the value the refresh overwrote was
 * the value it wrote, so the loss was invisible.  With a field a client sets,
 * the same line is a real loss and this is the arm that sees it.
 *
 * ⚠️ Run AFTER the probes, which is the whole point.  The rescan this server
 * already did inside hal_subscribe() happens before anything is bound, so it
 * could never have shown this: it asked the question while the answer was
 * still the default.
 */
static void
rescan_leaves_bound_devices_alone(void)
{
	unsigned int	before = 0, after = 0;
	kern_return_t	kr;
	unsigned int	bus, slot, func;

	if (n_controllers == 0)
		return;

	bus  = controllers[0].pci_bus;
	slot = controllers[0].pci_slot;
	func = controllers[0].pci_func;

	if (hal_get_device_state(hal_service_port, bus, slot, func, &before)
	    != KERN_SUCCESS)
		return;

	kr = hal_rescan(hal_service_port);

	if (hal_get_device_state(hal_service_port, bus, slot, func, &after)
	    != KERN_SUCCESS) {
		printf("blk: WRONG — %u:%u.%u vanished from the registry "
		       "across a rescan\n", bus, slot, func);
		return;
	}

	if (before == HAL_DEV_BOUND && after == before)
		printf("blk: a rescan (kr=%d) left %u:%u.%u bound — the "
		       "refresh no longer overwrites what the scan does not "
		       "produce\n", (int)kr, bus, slot, func);
	else
		printf("blk: WRONG — %u:%u.%u was %u before the rescan and %u "
		       "after\n", bus, slot, func, before, after);
}

/*
 * Probe a matched PCI device: allocate IRQ port, call module probe,
 * enumerate disks, parse MBR for each disk.
 */
static void
pci_probe_module(const struct block_driver_ops *ops,
		 unsigned int bus, unsigned int slot, unsigned int func,
		 uint64_t claimed_cap_id)
{
	struct blk_controller *ctrl = &controllers[n_controllers];
	kern_return_t kr;
	mach_port_t irq;
	void *priv;
	vm_offset_t bars;
	mach_msg_type_number_t bars_count;
	unsigned int n_bars;
	int rc;
	int nd, d;

	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &irq);
	if (kr != KERN_SUCCESS) {
		printf("blk: IRQ port alloc failed for %s\n", ops->name);
		return;
	}
	kr = mach_port_insert_right(mach_task_self(), irq, irq,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("blk: IRQ port right failed for %s\n", ops->name);
		return;
	}
	mach_port_move_member(mach_task_self(), irq, port_set);

	/*
	 * The device's regions, from the HAL that decoded them (#427).
	 *
	 * 🔑 Fetched HERE and handed to the module, rather than left for the
	 * module to fetch.  Every driver used to read its own BAR out of
	 * configuration space -- not from four bad decisions but because
	 * hal_get_device_info returned everything except the BARs, so there
	 * was nothing to ask.  Now there is, and asking once in the server
	 * keeps a second RPC dependency out of every module.
	 *
	 * ⚠️ A failure here is reported and the probe still runs with no
	 * regions.  A module that needs one says so in its own words, naming
	 * the region it wanted; a module that does not -- and some devices
	 * have none -- is not stopped by a question it never asked.
	 */
	bars = 0;
	bars_count = 0;
	n_bars = 0;
	kr = hal_get_device_bars(hal_service_port, bus, slot, func,
				 &bars, &bars_count, &n_bars);
	if (kr != KERN_SUCCESS) {
		printf("blk: hal_get_device_bars(%u:%u.%u) failed (kr=%d) — "
		       "%s probes without regions\n",
		       bus, slot, func, kr, ops->name);
		n_bars = 0;
	}

	rc = ops->probe(bus, slot, func, master_device, irq,
			(const struct pci_bar_region *)bars, n_bars, &priv);

	/*
	 * ⚠️ Released on both paths, and before the early return.  The buffer
	 * is out-of-line memory the RPC handed this task; the probe borrows it
	 * and does not own it, so a module that keeps the pointer keeps a
	 * dangling one.  Modules copy what they need -- see ahci_module.c.
	 */
	if (bars != 0)
		(void)vm_deallocate(mach_task_self(), bars,
				    (vm_size_t)bars_count);

	if (rc < 0) {
		printf("blk: %s probe failed at PCI %u:%u.%u\n",
		       ops->name, bus, slot, func);

		report_and_read_back(bus, slot, func, HAL_DEV_PROBE_FAILED);

		/*
		 * 🔑 AND THE DEVICE GOES BACK, which is both the right thing
		 * and the thing that shows revocation works.  A driver that
		 * could not bring a controller up is holding a claim it will
		 * never use, and on this machine a claim is not bookkeeping:
		 * it is an IOMMU domain the device can still reach memory
		 * through.  Revoking the capability is what takes that away --
		 * the kernel tears the domain down inside urmach_cap_revoke,
		 * and leaves the device reaching NOTHING rather than back on
		 * pass-through.
		 *
		 * ⚠️ The capability is this controller's own.  cap_server mints
		 * a fresh one per request even for the same class, so this
		 * gives back one device and not every device of its kind.
		 */
		if (claimed_cap_id != 0) {
			natural_t	bdf = (natural_t)((bus << 8)
							  | (slot << 3) | func);
			vm_address_t	kva = 0, dma = 0;
			uint64_t	probe_region = 0;
			kern_return_t	rkr, akr;

			rkr = cap_revoke(claimed_cap_id);

			/*
			 * 🔴 PROVED BY BEING REFUSED, and not by asking.
			 * device_dma_owned answers "is another task driving
			 * this" -- and THIS server was the one driving it, so
			 * it answers no whether the claim was dropped or is
			 * still standing.  A check that cannot distinguish the
			 * two outcomes is not a check, and this one read as a
			 * pass for one run before it was noticed.
			 *
			 * Asking for a DMA buffer can only succeed if the
			 * claim is still there.
			 */
			akr = device_dma_alloc(master_device, bdf, 4096,
					       &kva, &dma, &probe_region);
			if (akr == KERN_SUCCESS) {
				printf("blk: gave %u:%u.%u back (cap_revoke "
				       "kr=%d) and STILL got a DMA buffer for "
				       "it — the revocation did not reach the "
				       "kernel\n", bus, slot, func, (int)rkr);
				(void) device_dma_free(master_device, bdf,
						       kva, 4096);
			} else {
				printf("blk: gave %u:%u.%u back — cap_revoke "
				       "kr=%d, and the kernel now refuses this "
				       "server a DMA buffer for it (kr=%d): "
				       "the domain went with the "
				       "capability\n", bus, slot, func,
				       (int)rkr, (int)akr);
			}
		}
		return;
	}

	ctrl->ops = ops;
	ctrl->priv = priv;
	ctrl->pci_bus = bus;
	ctrl->pci_slot = slot;
	ctrl->pci_func = func;
	ctrl->irq_port = irq;

	nd = ops->get_disks(priv, ctrl->disks, MAX_DISKS_PER_CTRL);
	if (nd < 0)
		nd = 0;
	ctrl->n_disks = nd;

	printf("blk: %s at PCI %u:%u.%u — %d disk(s)\n",
	       ops->name, bus, slot, func, nd);

	n_controllers++;

	report_and_read_back(bus, slot, func, HAL_DEV_BOUND);

	/*
	 * Stable disk numbering (Issue #184): give every disk a global
	 * monotonically-increasing index that doesn't depend on which
	 * driver (AHCI, virtio-blk, …) discovered it.  Used to publish
	 * the "disk<N><letter>" netname aliases.
	 */
	static int next_stable_disk_idx = 0;
	for (d = 0; d < nd; d++)
		blk_read_mbr(ctrl, d, ops->name, next_stable_disk_idx++);
}

/* ================================================================
 * HAL subscription — look up "hal", register, drain initial replay
 * ================================================================ */

/*
 * #460: how long to wait for "hal" to check in, and how long to sleep
 * between tries.  100 x 100ms = ten seconds, the same order as the wait
 * kernel242_test uses for ipc_bench.  Far longer than the window this
 * closes (a few milliseconds), and still short enough that a HAL which
 * genuinely never arrives is reported rather than waited on forever.
 */
#define	BLK_HAL_WAIT_TRIES	100
#define	BLK_HAL_WAIT_MS		100

static int
hal_subscribe(void)
{
	kern_return_t	kr;
	int		tries;

	/*
	 * Wait for HAL to check in rather than giving up on the first look.
	 *
	 * bootstrap starts hal_server before this one, but *starting* is not
	 * *registering*: hal publishes its netname only after bringing up its
	 * module loader, and this server can reach the lookup first.  On a
	 * wedged boot the failed lookup was logged FOUR lines before "hal:
	 * registered service"; on a healthy boot hal registered eighteen
	 * lines before the lookup.  Same two events, opposite order -- a
	 * plain startup race (#460).
	 *
	 * Losing that race used to end main() with a single printf, and at the
	 * time a server whose main() returned did NOT die: crt0 called
	 * pthread_exit(), where a joinable main thread waited to be joined by
	 * a joiner that does not exist, and the task stayed alive answering
	 * nothing.  So 'disk0a' was never registered, bootstrap sat in
	 * "stage-2: waiting for 'disk0a'" forever, no test ever ran, and all
	 * four processors went idle -- a boot that looks hung from outside
	 * with nothing in the log to say why.  That is the whole early-wedge
	 * mode of #460.
	 *
	 * ⚠️ THAT HALF IS FIXED AND THE WAIT IS STILL RIGHT (#513).  A return
	 * from main() is exit() now, so the task would die rather than linger
	 * -- which turns a silent wedge into a server that is simply gone, and
	 * bootstrap still waits for a name nobody will register.  What removes
	 * the failure is winning the race, not how loudly losing it ends.
	 */
	for (tries = 0; ; tries++) {
		kr = netname_look_up(name_server_port, "", "hal",
				     &hal_service_port);
		if (kr == KERN_SUCCESS)
			break;
		if (tries >= BLK_HAL_WAIT_TRIES) {
			printf("blk: netname_look_up(\"hal\") failed (kr=%d) "
			       "after %d tries over %d ms — HAL never checked "
			       "in; no disks will be registered and the boot "
			       "will wait for 'disk0a' forever\n",
			       kr, tries, tries * BLK_HAL_WAIT_MS);
			return -1;
		}
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT,
				     BLK_HAL_WAIT_MS);
	}
	if (tries > 0)
		printf("blk: HAL checked in after %d retr%s (%d ms) — "
		       "startup race, not an error\n",
		       tries, tries == 1 ? "y" : "ies",
		       tries * BLK_HAL_WAIT_MS);

	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &hal_driver_port);
	if (kr != KERN_SUCCESS) {
		printf("blk: driver port alloc failed\n");
		return -1;
	}
	kr = mach_port_insert_right(mach_task_self(),
		hal_driver_port, hal_driver_port, MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("blk: driver port send-right failed\n");
		return -1;
	}

	kr = hal_register_driver(hal_service_port,
				 BLK_CLASS_MASK, BLK_CLASS_MATCH,
				 hal_driver_port);
	if (kr != KERN_SUCCESS) {
		printf("blk: hal_register_driver failed (kr=%d)\n", kr);
		return -1;
	}

	printf("blk: registered with HAL (mask=0x%08x match=0x%08x)\n",
	       BLK_CLASS_MASK, BLK_CLASS_MATCH);

	/* #173 smoke test: trigger one extra discovery pass.  The bus
	 * is stable after boot so HAL should report "0 new" and we
	 * must not receive a second hal_device_added burst. */
	{
		kern_return_t krr = hal_rescan(hal_service_port);
		printf("blk: hal_rescan kr=%d (expect quiet — no fresh devs)\n",
		       krr);
	}
	return 0;
}

/*
 * Drain the initial hal_device_added() burst queued on hal_driver_port
 * by the HAL replay.  Uses MACH_RCV_TIMEOUT; when the queue has been
 * empty for DRAIN_TIMEOUT_MS we consider the initial scan complete.
 *
 * The port is then handed over to port_set so future hotplug events
 * flow into the main message loop together with device I/O and IRQs.
 */
#define DRAIN_TIMEOUT_MS	300
#define DRAIN_BUF_SIZE		512

static void
hal_drain_replay(void)
{
	union {
		mach_msg_header_t	hdr;
		unsigned char		raw[DRAIN_BUF_SIZE];
	} in_msg, out_msg;
	kern_return_t kr;
	int n_events = 0;

	for (;;) {
		in_msg.hdr.msgh_local_port = hal_driver_port;
		in_msg.hdr.msgh_size = sizeof(in_msg);

		kr = mach_msg(&in_msg.hdr,
			      MACH_RCV_MSG | MACH_RCV_TIMEOUT,
			      0, sizeof(in_msg),
			      hal_driver_port,
			      DRAIN_TIMEOUT_MS,
			      MACH_PORT_NULL);
		if (kr == MACH_RCV_TIMED_OUT)
			break;
		if (kr != KERN_SUCCESS) {
			printf("blk: drain mach_msg failed (kr=%d)\n", kr);
			break;
		}

		if (!hal_notify_server(&in_msg.hdr, &out_msg.hdr))
			printf("blk: unexpected msgh_id=%d on drain\n",
			       in_msg.hdr.msgh_id);
		else
			n_events++;
	}

	printf("blk: HAL replay drained — %d event(s), "
	       "%d controller(s), %d partition(s)\n",
	       n_events, n_controllers, n_partitions);

	/* Park the subscription port on the main port set for hotplug. */
	mach_port_move_member(mach_task_self(), hal_driver_port, port_set);
}

/* ================================================================
 * Find controller by IRQ port (for demux)
 * ================================================================ */

static struct blk_controller *
find_controller_by_irq(mach_port_t local_port)
{
	int i;
	for (i = 0; i < n_controllers; i++) {
		if (controllers[i].irq_port == local_port)
			return &controllers[i];
	}
	return NULL;
}

/* ================================================================
 * Combined demux: device RPC + batch RPC + HAL notify + IRQ
 * ================================================================ */

/*
 * Forward decl: handler in block_device.c that reaps a per-client
 * struct blk_handle when its port runs out of senders.
 */
extern boolean_t blk_handle_no_senders(mach_msg_header_t *in,
				       mach_msg_header_t *out);

/*
 * Forward decl: handler in block_device.c that flips the revoked bit
 * on every blk_handle whose cap_id matches.  Returns the number of
 * handles affected (only used for diagnostics).
 */
extern int blk_handles_revoke_by_cap_id(uint64_t cap_id);

/*
 * MIG server-side handler for cap_revoke_notify (Issue #183).  This
 * is invoked by the cap_revoke_server() demux when cap_server fires a
 * notification on our subscription port.
 */
kern_return_t
cap_revoke_notify(mach_port_t notify_port, uint64_t cap_id)
{
	(void)notify_port;
	int n = blk_handles_revoke_by_cap_id(cap_id);
	if (n == 0)
		printf("blk: cap_revoke_notify cap=%llu — no live handle\n",
		       (unsigned long long)cap_id);
	return KERN_SUCCESS;
}

static boolean_t
blk_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
	/*
	 * No-senders notifications are SEND_ONCE messages with a
	 * recognisable msgh_id; check before MIG dispatchers since
	 * none of them know about the notification subsystem id space.
	 */
	if (blk_handle_no_senders(in, out))
		return TRUE;

	if (device_server(in, out))
		return TRUE;

	if (ahci_batch_server(in, out))
		return TRUE;

	if (hal_notify_server(in, out))
		return TRUE;

	if (cap_revoke_server(in, out))
		return TRUE;

	if (in->msgh_id >= IRQ_NOTIFY_MSGH_BASE) {
		struct blk_controller *ctrl =
			find_controller_by_irq(in->msgh_local_port);
		if (ctrl && ctrl->ops->irq_handler)
			ctrl->ops->irq_handler(ctrl->priv);
		((mig_reply_error_t *)out)->RetCode = MIG_NO_REPLY;
		((mig_reply_error_t *)out)->Head.msgh_size =
			sizeof(mig_reply_error_t);
		return TRUE;
	}

	return FALSE;
}

/* ================================================================
 * Main entry point
 * ================================================================ */

int
main(int argc, char **argv)
{
	kern_return_t kr;

	kr = bootstrap_ports(bootstrap_port,
			     &host_port, &device_port,
			     &root_ledger_wired, &root_ledger_paged,
			     &security_port);
	if (kr != KERN_SUCCESS)
		_exit(1);

	printf_init(device_port);
	panic_init(host_port);
	master_device = device_port;

	printf("\n=== Block device server (HAL-driven) ===\n");

	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_PORT_SET, &port_set);
	if (kr != KERN_SUCCESS) {
		printf("blk: port set alloc failed\n");
		return 1;
	}

	/* #199 prep: mirror printf to gpu_server's text plane.  No-op
	 * if gpu_server isn't up yet; serial console keeps working. */
	(void)gpu_console_init("blk");

	modload_init("blk");
	n_modules = modload_load_class("block", "_module_ops",
				       (void **)modules, MAX_MODULES);
	modules[n_modules] = NULL;

	if (n_modules == 0) {
		printf("blk: no driver modules loaded, exiting\n");
		return 1;
	}

	if (hal_subscribe() < 0)
		return 1;

	hal_drain_replay();

	rescan_leaves_bound_devices_alone();

	if (n_partitions == 0) {
		/*
		 * Issue #400: say what follows from this.  Exiting here is
		 * correct — there is nothing to serve — but downstream it
		 * leaves ext_server without a root, and the boot then waits
		 * forever, which on the console is indistinguishable from a
		 * crash unless we say so here.
		 */
		printf("blk: no usable partitions found, exiting\n");
		printf("blk: ext_server has no root to mount — the boot "
		       "stops here by design, this is not a hang\n");
		return 1;
	}

	blk_register_partitions();

	/*
	 * Issue #183: subscribe to cap_server's revocation back-channel
	 * so we can drop authenticated handles synchronously when a cap
	 * is revoked.  We share the main port_set: cap_revoke_notify
	 * messages flow into blk_demux right alongside device I/O and
	 * IRQs, no separate thread needed.
	 */
	{
		mach_port_t notify_port = MACH_PORT_NULL;
		kern_return_t skr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &notify_port);
		if (skr == KERN_SUCCESS) {
			(void)mach_port_move_member(mach_task_self(),
						    notify_port, port_set);
			mach_port_t send = MACH_PORT_NULL;
			(void)mach_port_insert_right(mach_task_self(),
				notify_port, notify_port,
				MACH_MSG_TYPE_MAKE_SEND);
			send = notify_port;
			skr = cap_subscribe_revoke(send);
			if (skr == KERN_SUCCESS)
				printf("blk: subscribed to cap_revoke_notify "
				       "(port=0x%x)\n", (unsigned)notify_port);
			else
				printf("blk: cap_subscribe_revoke failed "
				       "(kr=%d)\n", (int)skr);
		} else {
			printf("blk: notify port alloc failed (kr=%d)\n",
			       (int)skr);
		}
	}

	/*
	 * 🔑 Before the message loop, not after: the first client of this
	 * server is bootstrap looking for a server to run, and "the disk
	 * could not be read" is a thing to learn from the server that owns
	 * the disk rather than from the task that failed to load.
	 */
	blk_readback_selftest();

	/*
	 * And what a read COSTS, which is the half #432 still needs: its
	 * done-when asks for the performance cost of translation to be
	 * measured rather than assumed, and until this there was nothing in
	 * the boot that timed a transfer at all.  The benchmarks this system
	 * prints are traps and RPCs -- all CPU-bound, none of them touching
	 * the DMA path an IOMMU is in.
	 */
	blk_read_bench();

	printf("blk: init complete, %d partition(s), "
	       "entering message loop\n", n_partitions);

	mach_msg_server(blk_demux, 8192, port_set,
			MACH_MSG_OPTION_NONE);

	printf("blk: mach_msg_server exited unexpectedly\n");
	return 1;
}
