/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What the HAL says about a device's regions, and whether it can be reached
 * when the device sits above four gigabytes (#427).
 *
 * ── Two halves, and only one of them needs a special machine ──────────
 *
 * The SIZES run on every boot.  A region's size is not a field anywhere in
 * configuration space -- it is measured, by writing all ones into the BAR and
 * reading back which bits the device refused -- so a size that arrives here at
 * all is evidence the measurement happened, and a size that survives a
 * hal_rescan is evidence it did not happen twice.  ⚠️ That second one matters
 * more than it looks: the probe writes to the device, and hal_rescan is a
 * public RPC that block_device_server calls on every boot.  A probe on that
 * path would disable the decoding of a disk that is in use.
 *
 * The HIGH half needs a device placed above four gigabytes, and no board these
 * suites boot on puts one there by itself.  It is provided deliberately:
 *
 *	scripts/run-x86_64.sh --entry 14 120 -machine q35 \
 *	  -object memory-backend-ram,id=hm,size=2G \
 *	  -device ivshmem-plain,memdev=hm
 *
 * 🔑 THAT CONFIGURATION FORCES THE PLACEMENT RATHER THAN HOPING FOR IT.  The
 * 32-bit PCI hole on these boards is about two gigabytes wide with several
 * devices already in it, so a two-gigabyte prefetchable BAR cannot fit under
 * four gigabytes and the firmware has nowhere to put it but above.  A test
 * that merely waited to be unlucky would pass with the defect intact -- which
 * is the shape the whole issue is written around.
 *
 * ⚠️ When that device is absent this program says so, by name, and reports the
 * arms it did run.  It does not report "skipped": a check that can quietly
 * decline to check is how a red line stops being read.
 *
 * ── What the arms are written to catch ────────────────────────────────
 *
 * Every one of them fails with a wrong ANSWER rather than with a crash.  A
 * 64-bit BAR read as two 32-bit ones yields a plausible address and a
 * plausible extra region; a size computed by complementing a read-back yields
 * four gigabytes for a thirty-two byte port range; a physical address
 * truncated to 32 bits yields a mapping that WORKS, of the wrong memory, until
 * the low four gigabytes are busy.  None of the three announces itself.
 */

#include <stdio.h>
#include <string.h>
#include <mach.h>
#include <mach/bootstrap.h>
#include <sa_mach.h>
#include <mach_init.h>			/* name_server_port */
#include <servers/netname.h>
#include "hal_server.h"			/* struct hal_device_info */
#include "hal.h"			/* the user half of hal.defs */
#include "device_master.h"		/* device_mmio_map / _unmap */

#define MAX_DEVS	HAL_MAX_DEVICES

static struct hal_device_info	before[MAX_DEVS];
static struct hal_device_info	after[MAX_DEVS];

/*
 * ⚠️ Not `host_port' and `device_port'.  hal_server.h is included here for the
 * device record and it also declares the HAL's own globals under those names,
 * so the obvious spelling makes this program's ports a static redeclaration of
 * somebody else's externs -- which the compiler catches, and which would have
 * been a shared name if it had not.
 */
static mach_port_t	my_host_port;
static mach_port_t	my_device_port;
static mach_port_t	hal_port;

static int	passed;
static int	failed;

static void
arm(const char *name, int ok, const char *detail)
{
	if (ok) {
		printf("hal_bar: %s: OK\n", name);
		passed++;
		return;
	}
	printf("hal_bar: %s: WRONG — %s\n", name, detail);
	failed++;
}

static void
print_u64(const char *label, uint64_t v)
{
	printf("%s0x%08X%08X", label, (unsigned int)(v >> 32),
	       (unsigned int)(v & 0xFFFFFFFFu));
}

/*
 * Fetch the whole registry into `out'.  Returns how many records arrived, or
 * -1.  The buffer is out-of-line and is given back before returning: the
 * program asks twice and a leak here would be a leak per rescan.
 */
static int
fetch_registry(struct hal_device_info *out, unsigned int max)
{
	vm_offset_t		buf = 0;
	mach_msg_type_number_t	bytes = 0;
	unsigned int		n = 0;
	kern_return_t		kr;

	kr = hal_list_devices(hal_port, &buf, &bytes, &n);
	if (kr != KERN_SUCCESS) {
		printf("hal_bar: hal_list_devices failed (kr=%d)\n", kr);
		return -1;
	}

	if (n > max)
		n = max;
	if (n > 0)
		memcpy(out, (const void *)buf,
		       (size_t)n * sizeof(struct hal_device_info));

	if (bytes != 0)
		(void) vm_deallocate(mach_task_self(), buf, bytes);

	return (int)n;
}

/*
 * ── The sizes, on whatever board this is ─────────────────────────────
 */
static void
check_sizes(int n)
{
	int		i;
	unsigned int	b;
	int		regions = 0;
	int		unmeasured = 0;
	int		not_power_of_two = 0;
	int		misaligned = 0;

	for (i = 0; i < n; i++)
		for (b = 0; b < before[i].n_bars; b++) {
			const struct pci_bar_region *r = &before[i].bars[b];

			regions++;

			if (r->size == 0) {
				printf("hal_bar:   %02u:%02u.%u bar%u has no "
				       "size\n", before[i].bus, before[i].slot,
				       before[i].func, r->slot);
				unmeasured++;
				continue;
			}

			/*
			 * 🔑 A BAR's size is a power of two BECAUSE OF HOW IT
			 * IS ENCODED, not by convention: the device hardwires
			 * the bits below its size to zero, so the answer is
			 * always a single bit.  A size that is not one did not
			 * come from a BAR -- it came from arithmetic that went
			 * wrong, which is the failure mode with no other
			 * symptom.
			 */
			if ((r->size & (r->size - 1)) != 0) {
				printf("hal_bar:   %02u:%02u.%u bar%u size is "
				       "not a power of two\n", before[i].bus,
				       before[i].slot, before[i].func, r->slot);
				not_power_of_two++;
			}

			/*
			 * And it is naturally aligned, for the same reason:
			 * the bits the device refuses to let anyone write are
			 * the bits below the size, so the base cannot have any
			 * of them set.
			 */
			if ((r->base & (r->size - 1)) != 0) {
				printf("hal_bar:   %02u:%02u.%u bar%u base is "
				       "not aligned to its size\n",
				       before[i].bus, before[i].slot,
				       before[i].func, r->slot);
				misaligned++;
			}
		}

	printf("hal_bar: %d device(s), %d region(s)\n", n, regions);

	arm("[1] the HAL found regions to report", regions > 0,
	    "no device on this board has a single BAR, which is not a board "
	    "this test can say anything about");

	arm("[2] every region carries a measured size", unmeasured == 0,
	    "a region arrived with size 0, which is what a HAL that never "
	    "probed reports");

	arm("[3] every size is a single bit", not_power_of_two == 0,
	    "a size that is not a power of two did not come from a BAR");

	arm("[4] every base is aligned to its size", misaligned == 0,
	    "a BAR cannot name an address below its own alignment, so either "
	    "the base or the size is wrong");
}

/*
 * ── The rescan, which must not touch anything ────────────────────────
 */
static void
check_rescan(int n_before)
{
	kern_return_t	kr;
	int		n_after, i;
	unsigned int	b;
	int		changed = 0;

	kr = hal_rescan(hal_port);
	if (kr != KERN_SUCCESS) {
		arm("[5] a rescan leaves every region exactly as it was", 0,
		    "hal_rescan itself failed");
		return;
	}

	n_after = fetch_registry(after, MAX_DEVS);
	if (n_after != n_before) {
		arm("[5] a rescan leaves every region exactly as it was", 0,
		    "the device count changed across a rescan of a bus that "
		    "did not change");
		return;
	}

	for (i = 0; i < n_after; i++) {
		if (after[i].n_bars != before[i].n_bars) {
			changed++;
			continue;
		}
		for (b = 0; b < after[i].n_bars; b++)
			if (after[i].bars[b].base != before[i].bars[b].base
			    || after[i].bars[b].size != before[i].bars[b].size
			    || after[i].bars[b].flags != before[i].bars[b].flags
			    || after[i].bars[b].slot != before[i].bars[b].slot) {
				printf("hal_bar:   %02u:%02u.%u region %u "
				       "changed across a rescan\n",
				       after[i].bus, after[i].slot,
				       after[i].func, b);
				changed++;
			}
	}

	/*
	 * ⚠️ This arm proves the record survived; it does not by itself prove
	 * the DEVICE was left alone, because a probe that ran a second time
	 * would restore what it wrote and produce the same numbers.  That half
	 * is in the HAL's own line -- `(N new, M measured)' -- which prints
	 * zero measured on every rescan and cannot print zero if the probe ran.
	 * Two observations, because one of them cannot distinguish the cases.
	 */
	arm("[5] a rescan leaves every region exactly as it was", changed == 0,
	    "a rescan overwrote the record, which is where a measured size "
	    "goes to die -- a scan has no size to report");
}

/*
 * ── The device above four gigabytes ──────────────────────────────────
 *
 * Returns 0 if there is none, in which case nothing here has run.
 */
static int
check_high_device(int n)
{
	const struct hal_device_info	*dev = NULL;
	const struct pci_bar_region	*high = NULL;
	const struct hal_device_info	*writable_dev = NULL;
	const struct pci_bar_region	*writable = NULL;
	vm_offset_t			buf = 0;
	mach_msg_type_number_t		bytes = 0;
	unsigned int			n_bars = 0;
	kern_return_t			kr;
	int				i;
	unsigned int			b;

	/*
	 * ── Which high region, and it is not "the first one" ─────────
	 *
	 * 🔴 THIS PROGRAM WROTE INTO THE BOOT DISK CONTROLLER'S REGISTERS, and it
	 * did so with a guard in front of it.  The rule was "write only to a
	 * PREFETCHABLE region", on the standard's own reasoning that prefetchable
	 * means memory-like -- no side effects on read, writes may be merged.  On
	 * the board that provides a high device, the two-gigabyte BAR pushes
	 * virtio-blk's modern register aperture above four gigabytes too, and QEMU
	 * marks that aperture prefetchable.  So the first high region belonged to
	 * the disk we had booted from, and the guard said yes.
	 *
	 * 🔑 THE DEVICE'S CLASS IS THE ANSWER THE FLAG WAS PRETENDING TO BE.  A
	 * class 5 memory controller declares itself to BE memory; a mass storage
	 * controller does not, whatever it marks its apertures.  And the second
	 * half of the question is #513's: the HAL is asked whether anything is
	 * DRIVING the device, because "this is memory" and "nobody is using it"
	 * are two facts and only one of them is on the bus.
	 *
	 * ⚠️ Mapping is separated from writing rather than made conditional with
	 * it.  A mapping that is never touched costs the device nothing, so the
	 * arms about reaching a high address still run on whatever is up there;
	 * only the pattern needs somewhere it is allowed to go.
	 */
	for (i = 0; i < n; i++)
		for (b = 0; b < before[i].n_bars; b++) {
			unsigned int state = HAL_DEV_BOUND;

			if (before[i].bars[b].base <= 0xFFFFFFFFull)
				continue;

			if (high == NULL) {
				dev = &before[i];
				high = &before[i].bars[b];
			}

			if (PCI_CLASS_OF(before[i].class_rev)
			    != PCI_CLASS_MEMORY)
				continue;

			if (hal_get_device_state(hal_port, before[i].bus,
						 before[i].slot, before[i].func,
						 &state) != KERN_SUCCESS)
				continue;
			if (state != HAL_DEV_UNCLAIMED)
				continue;

			if (writable == NULL) {
				writable_dev = &before[i];
				writable = &before[i].bars[b];
			}
		}

	if (high == NULL)
		return 0;

	printf("hal_bar: %02u:%02u.%u bar%u is above four gigabytes: ",
	       dev->bus, dev->slot, dev->func, high->slot);
	print_u64("base=", high->base);
	print_u64("  size=", high->size);
	printf("\n");

	if (writable != NULL)
		printf("hal_bar: %02u:%02u.%u bar%u is a RAM controller's, and "
		       "the HAL says nobody is driving it — that is the one "
		       "this writes through\n",
		       writable_dev->bus, writable_dev->slot,
		       writable_dev->func, writable->slot);

	/*
	 * [6] It took two slots, and the slot after it is NOT a region of its
	 * own.  This is the defect in its original form: six slots read as six
	 * regions gives a device one more region than it has, whose "address"
	 * is the upper half of the previous one.
	 */
	arm("[6] the high region says it is 64-bit",
	    (high->flags & PCI_REGION_MEM_64) != 0,
	    "a region above four gigabytes that does not claim two slots is a "
	    "region whose address was assembled from something else");
	{
		int ghost = 0;

		for (b = 0; b < dev->n_bars; b++)
			if (dev->bars[b].slot == high->slot + 1)
				ghost = 1;

		arm("[7] the upper half is not a region of its own", !ghost,
		    "the slot holding the top 32 bits of this address was "
		    "reported as a separate device region");
	}

	/*
	 * [8] The dedicated RPC agrees with the listing.
	 *
	 * 🔑 Two producers, one answer.  hal_list_devices copies the whole
	 * record and hal_get_device_bars builds a buffer of regions on its own;
	 * they read the same table but not by the same code, and the second is
	 * the one every driver actually uses.
	 */
	kr = hal_get_device_bars(hal_port, dev->bus, dev->slot, dev->func,
				 &buf, &bytes, &n_bars);
	if (kr != KERN_SUCCESS) {
		arm("[8] get_device_bars agrees with list_devices", 0,
		    "hal_get_device_bars failed for a device list_devices "
		    "reported");
	} else {
		const struct pci_bar_region *v =
			(const struct pci_bar_region *)buf;
		int same = (n_bars == dev->n_bars);

		for (b = 0; same && b < n_bars; b++)
			same = v[b].base == dev->bars[b].base
			    && v[b].size == dev->bars[b].size
			    && v[b].flags == dev->bars[b].flags
			    && v[b].slot == dev->bars[b].slot;

		arm("[8] get_device_bars agrees with list_devices", same,
		    "the two RPCs that hand out regions disagree about this "
		    "device");

		if (bytes != 0)
			(void) vm_deallocate(mach_task_self(), buf, bytes);
	}

	/*
	 * [9] Mapping it.
	 *
	 * 🔴 ON A TARGET WHOSE ADDRESSES ARE 32 BITS THIS IS A REFUSAL, AND THE
	 * REFUSAL IS THE RESULT.  vm_address_t is exactly 32 bits wide on i386,
	 * so the address cannot be handed to the kernel at all -- and the
	 * interesting thing is that it got HERE: the region record carries a
	 * physical address at a fixed 64 bits on both targets, precisely so a
	 * machine that cannot map such a device can still say that it exists.
	 * A record whose width followed the target would have reported a
	 * different device, or none.
	 */
	if (sizeof(vm_address_t) < sizeof(uint64_t)) {
		arm("[9] a 32-bit machine reports the address whole and "
		    "declines to map it",
		    (uint64_t)(uint32_t)high->base != high->base,
		    "the base survived a round trip through 32 bits, so this "
		    "is not the address the device was placed at");
		printf("hal_bar: this target's addresses are %u bits — the "
		       "mapping arms belong to the 64-bit one\n",
		       (unsigned)(sizeof(vm_address_t) * 8));
		return 1;
	}

	{
		vm_address_t	uva = 0;
		vm_address_t	uva_far = 0;
		vm_size_t	page = 4096;

		kr = device_mmio_map(my_device_port, (vm_address_t)high->base,
				     page, mach_task_self(), &uva);
		arm("[9] the region maps", kr == KERN_SUCCESS && uva != 0,
		    "device_mmio_map refused a physical address above four "
		    "gigabytes");
		if (kr != KERN_SUCCESS)
			return 1;

		printf("hal_bar: mapped at uva=%p\n", (void *)uva);

		/*
		 * [10] The mapping reaches the device.
		 *
		 * 🔴 THIS IS THE ARM THE TRUNCATION DEFECT FAILS.  A physical
		 * address narrowed to 32 bits still MAPS -- vm_map_enter finds
		 * a hole and pmap_enter installs a page -- and what it maps is
		 * whatever lives at the bottom 32 bits of the address, which on
		 * this machine is ordinary RAM.  Every return code is success.
		 * Only reading back a pattern through the mapping can tell the
		 * two apart, and only if the pattern is written through it.
		 *
		 * ⚠️ Through the region chosen above, which may not be the one
		 * just mapped.  See the note on the search: the first high
		 * region on the board that provides one belongs to the disk
		 * this booted from.
		 */
		kr = device_mmio_unmap(my_device_port, uva, page,
				       mach_task_self());
		arm("[10] and it unmaps", kr == KERN_SUCCESS,
		    "device_mmio_unmap could not be given back the address it "
		    "handed out");

		if (writable == NULL) {
			printf("hal_bar: no high region belongs to a RAM "
			       "controller nobody is driving, so nothing is "
			       "written through one — the pattern arms did not "
			       "run\n");
			return 1;
		}

		kr = device_mmio_map(my_device_port,
				     (vm_address_t)writable->base,
				     page, mach_task_self(), &uva);
		if (kr != KERN_SUCCESS) {
			arm("[11] what is written through the mapping reads "
			    "back", 0, "the region that may be written would "
			    "not map");
			return 1;
		}

		{
			volatile uint32_t *w = (volatile uint32_t *)uva;
			uint32_t	pattern = 0x5A3C96E1u;
			uint32_t	readback;

			w[0] = pattern;
			w[1] = ~pattern;
			readback = w[0];

			arm("[11] what is written through the mapping reads "
			    "back", readback == pattern && w[1] == ~pattern,
			    "the mapping succeeded and does not reach the "
			    "memory the device answers for");

			/*
			 * [12] The far end of the region is a different page.
			 * A truncated base would put both mappings inside the
			 * low four gigabytes, where they can land on the same
			 * RAM; the two patterns would then overwrite each
			 * other.
			 */
			if (writable->size > page) {
				kr = device_mmio_map(my_device_port,
					(vm_address_t)(writable->base
						       + writable->size - page),
					page, mach_task_self(), &uva_far);
				if (kr == KERN_SUCCESS) {
					volatile uint32_t *f =
						(volatile uint32_t *)uva_far;

					f[0] = 0xC0FFEE00u;
					arm("[12] the far end of the region is "
					    "somewhere else",
					    f[0] == 0xC0FFEE00u
					    && w[0] == pattern,
					    "writing at the end of the region "
					    "changed what is at its start");
					(void) device_mmio_unmap(my_device_port,
						uva_far, page,
						mach_task_self());
				} else {
					arm("[12] the far end of the region is "
					    "somewhere else", 0,
					    "the last page of the region would "
					    "not map");
				}
			}
		}

		(void) device_mmio_unmap(my_device_port, uva, page,
					 mach_task_self());
	}

	return 1;
}

int
main(void)
{
	kern_return_t	kr;
	mach_port_t	root_wired, root_paged, security;
	int		n;

	kr = bootstrap_ports(bootstrap_port, &my_host_port, &my_device_port,
			     &root_wired, &root_paged, &security);
	if (kr != KERN_SUCCESS)
		return 1;

	printf_init(my_device_port);

	printf("hal_bar: started — what the HAL says about BARs, and whether "
	       "one above four gigabytes can be reached\n");

	kr = netname_look_up(name_server_port, "", "hal", &hal_port);
	if (kr != KERN_SUCCESS) {
		printf("hal_bar: no \"hal\" in the name server (kr=%d) — "
		       "0 of 0 arms passed\n", kr);
		return 1;
	}

	n = fetch_registry(before, MAX_DEVS);
	if (n < 0) {
		printf("hal_bar: 0 of 0 arms passed\n");
		return 1;
	}

	check_sizes(n);
	check_rescan(n);

	if (check_high_device(n) == 0)
		printf("hal_bar: no region above four gigabytes on this "
		       "machine — the high arms did not run; see the header of "
		       "this file for the invocation that provides one\n");

	printf("hal_bar: %d of %d arms passed\n", passed, passed + failed);

	/*
	 * 🔴 END THE TASK.  A return from main() reaches exit() now (#513), and
	 * exit() is what ends a process; leaving the task alive would keep this
	 * program's ports and its share of the registry buffer for the rest of
	 * the boot, for no reason.
	 */
	return failed == 0 ? 0 : 1;
}
