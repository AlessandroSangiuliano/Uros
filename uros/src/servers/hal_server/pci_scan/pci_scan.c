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
 * pci_scan.c — PCI discovery module for the HAL server.
 *
 * Brute-force enumeration of bus 0..255, slot 0..31, func 0..7 via
 * device_pci_config_read.  Each valid device is written into the
 * caller-supplied array as a fully populated hal_device_info record
 * (BARs and IRQ line included).
 *
 * The scan is pure discovery — it reads configuration space and writes
 * nothing.  The MEASUREMENT below it is not, and that is the whole reason the
 * two are separate entry points: a region's size cannot be read, only probed,
 * and probing writes.  Ownership of the device is still handed off to a driver
 * server afterwards, which enables memory/bus-master for itself; what it no
 * longer has to do is find out where its device is and how big it is.
 *
 * ⚠️ The comment here used to say this module "never programs BARs or writes
 * command registers", and it was true of every line above.  It stopped being
 * true the moment a size had to be a number rather than a constant in a
 * driver's header, and a file that describes what it used to do is worse than
 * one that describes nothing.
 */

#include <mach.h>
#include <device/device.h>
#include <stdio.h>
#include <string.h>
#include "device_master.h"
#include "hal_server.h"

/*
 * The configuration header's offsets come from <device/pci.h> now (#427).
 * They used to be four private copies across this tree; see that header.
 */

static mach_port_t	pci_master_device;

static int
pci_scan_init(mach_port_t master_device)
{
	pci_master_device = master_device;
	return 0;
}

/*
 * Read one PCI device's configuration into `dev` (vendor, class, BARs,
 * IRQ).  Returns 1 if a device is present, 0 if the slot is empty,
 * and -1 on a kernel RPC error.
 */
static int
read_pci_device(unsigned int bus, unsigned int slot, unsigned int func,
		struct hal_device_info *dev)
{
	unsigned int vendor_device, class_rev, irq_reg, bar, hdr;
	uint32_t raw_bars[PCI_NUM_BAR_SLOTS];
	unsigned int nslots;
	kern_return_t kr;
	int i;

	kr = device_pci_config_read(pci_master_device,
				    bus, slot, func,
				    PCI_VENDOR_ID, &vendor_device);
	if (kr != KERN_SUCCESS)
		return -1;
	if (vendor_device == 0xFFFFFFFFu || vendor_device == 0)
		return 0;

	memset(dev, 0, sizeof(*dev));
	dev->bus = bus;
	dev->slot = slot;
	dev->func = func;
	dev->vendor_device = vendor_device;

	kr = device_pci_config_read(pci_master_device,
				    bus, slot, func,
				    PCI_CLASS_REV, &class_rev);
	dev->class_rev = (kr == KERN_SUCCESS) ? class_rev : 0;

	kr = device_pci_config_read(pci_master_device,
				    bus, slot, func,
				    PCI_INTERRUPT_LINE, &irq_reg);
	dev->irq = (kr == KERN_SUCCESS) ? (irq_reg & 0xFFu) : 0;

	/*
	 * How many of the six slots are BARs at all.
	 *
	 * 🔴 A PCI-to-PCI bridge has TWO, and 0x18 onwards holds its bus
	 * numbers and the I/O and memory windows it forwards.  Decoding six on
	 * one of those turns a bus topology into addresses; and once the size
	 * is MEASURED rather than assumed, writing all ones into slot 2 of a
	 * bridge reprograms the window every device behind it is reached
	 * through.  So the count is settled from the header type before
	 * anything reads a slot, let alone writes one.
	 *
	 * ⚠️ A header type this code does not know gets ZERO slots rather than
	 * six.  CardBus is the only other one defined and its layout is not
	 * this one; a type nobody has seen is a device saying something we
	 * cannot act on, and six is the answer that would be acted on.
	 */
	kr = device_pci_config_read(pci_master_device,
				    bus, slot, func,
				    PCI_HDR_DWORD, &hdr);
	if (kr != KERN_SUCCESS)
		return -1;

	switch (PCI_HEADER_TYPE_OF(hdr) & PCI_HEADER_TYPE_MASK) {
	case PCI_HEADER_TYPE_NORMAL:
		nslots = PCI_NUM_BAR_SLOTS;
		break;
	case PCI_HEADER_TYPE_BRIDGE:
		nslots = PCI_NUM_BAR_SLOTS_BRIDGE;
		break;
	default:
		printf("pci_scan: %02u:%02u.%u header type 0x%02x is not one "
		       "this reads BARs from\n", bus, slot, func,
		       PCI_HEADER_TYPE_OF(hdr) & PCI_HEADER_TYPE_MASK);
		nslots = 0;
		break;
	}

	/*
	 * The BARs: read the slots this header has, then decode them into
	 * regions (#427).
	 *
	 * 🔑 Two steps and not one, and the split is the fix.  What comes off
	 * the bus is six 32-bit slots; what a driver needs is a list of
	 * regions, and the two counts are not the same number because a 64-bit
	 * memory BAR occupies two slots.  This loop reads, and knows nothing
	 * about what the values mean; pci_bars_decode() decides what they are,
	 * and it is a pure function precisely so that decision can be checked
	 * without a device -- see tests/pci_bar_test.
	 *
	 * ⚠️ A slot whose read fails becomes zero, which the decode treats as
	 * an unimplemented BAR.  That is the honest reading: a value that did
	 * not arrive is not a region, and inventing one would hand a driver an
	 * address that no device answered for.
	 */
	memset(raw_bars, 0, sizeof(raw_bars));
	for (i = 0; i < (int)nslots; i++) {
		kr = device_pci_config_read(pci_master_device,
					    bus, slot, func,
					    PCI_BAR(i), &bar);
		raw_bars[i] = (kr == KERN_SUCCESS) ? bar : 0;
	}

	/*
	 * The slots as they were read, before anything decided what they mean.
	 *
	 * 🔑 The observation and the interpretation are two different things,
	 * and a discovery server that keeps only the second cannot answer the
	 * first question an operator asks: "why is there no region for BAR1?"
	 * There are two answers with the same symptom -- the device does not
	 * implement that slot, or the decode threw it away -- and the region
	 * list alone cannot tell them apart.
	 *
	 * ⚠️ Logged here rather than stored in the record.  The wire record
	 * carries what a client acts on, and nothing acts on raw slots; adding
	 * a field for them would recreate, one release later, exactly the
	 * produced-and-never-consumed array this issue removed.  A diagnostic
	 * belongs where and when the observation is made.
	 *
	 * A device with no BARs at all says nothing: the dump's `regions=0'
	 * already covers it, and six zeros per host bridge is noise.
	 */
	{
		int any = 0;

		for (i = 0; i < PCI_NUM_BAR_SLOTS; i++)
			if (raw_bars[i] != 0)
				any = 1;

		if (any)
			printf("pci_scan: %02u:%02u.%u raw bars: "
			       "%08x %08x %08x %08x %08x %08x\n",
			       bus, slot, func,
			       raw_bars[0], raw_bars[1], raw_bars[2],
			       raw_bars[3], raw_bars[4], raw_bars[5]);
	}

	dev->n_bars = pci_bars_decode(raw_bars, nslots,
				      dev->bars, HAL_MAX_BARS);

	return 1;
}

/*
 * ── Measuring a region, which is the one thing here that WRITES ──────
 *
 * 🔑 A SIZE IS NOT A FIELD.  Configuration space says where a device is, never
 * how much of the address space it answers for; that is discovered by writing
 * all ones into the BAR and reading back which bits the device refused to take.
 * The bits it hardwires to zero are the ones below its size.
 *
 * 🔴 SO THE MEASUREMENT MOVES THE DEVICE, for as long as it takes to read the
 * answer back.  Between the write and the restore the BAR names an address the
 * device does not belong at, and if it were decoding it would answer for
 * somebody else's memory.  Hence the command register: the device's decoding is
 * switched OFF around the whole pass and put back afterwards, which is what
 * makes this safe rather than merely quick.
 *
 * ⚠️ And it is why this runs once, at first discovery.  hal_rescan is a public
 * RPC that block_server calls on every boot; a probe on that path would disable
 * a disk that is in use, on demand, from a caller that only wanted to know
 * whether anything new appeared.  hal_run_discovery() calls this only for a BDF
 * the registry had never seen.
 */
static int
pci_scan_measure(struct hal_device_info *dev)
{
	unsigned int	cmd_status, cmd, r;
	kern_return_t	kr;

	if (dev == NULL)
		return -1;
	if (pci_master_device == MACH_PORT_NULL)
		return -1;
	if (dev->n_bars == 0)
		return 0;

	kr = device_pci_config_read(pci_master_device,
				    dev->bus, dev->slot, dev->func,
				    PCI_COMMAND, &cmd_status);
	if (kr != KERN_SUCCESS)
		return -1;

	/*
	 * ⚠️ THE UPPER HALF OF THIS DWORD IS THE STATUS REGISTER, AND ITS BITS
	 * ARE CLEARED BY WRITING ONE.  So the value read back cannot be written
	 * back: a device with a signalled error would have that error erased by
	 * the act of putting its command register the way it was.  Only the low
	 * sixteen bits are ever written here, and zeros in the top half reach
	 * status bits as "leave alone".
	 */
	cmd = cmd_status & 0xFFFFu;

	kr = device_pci_config_write(pci_master_device,
				     dev->bus, dev->slot, dev->func,
				     PCI_COMMAND,
				     cmd & ~(PCI_CMD_IO_ENABLE
					     | PCI_CMD_MEM_ENABLE));
	if (kr != KERN_SUCCESS)
		return -1;

	for (r = 0; r < dev->n_bars; r++) {
		struct pci_bar_region	*reg = &dev->bars[r];
		unsigned int		nslots = pci_bar_region_slots(reg);
		unsigned int		lo = 0, hi = 0;
		unsigned int		probe_lo = 0, probe_hi = 0;
		unsigned int		check = 0;

		/*
		 * ⚠️ Both halves are written before either is read back, and
		 * both are restored after.  A 64-bit region's size can be
		 * larger than four gigabytes, so the answer is not in the lower
		 * slot at all -- and restoring the lower half before reading
		 * the upper one would measure a BAR that is half moved.
		 */
		(void) device_pci_config_read(pci_master_device,
					      dev->bus, dev->slot, dev->func,
					      PCI_BAR(reg->slot), &lo);
		if (nslots == 2)
			(void) device_pci_config_read(pci_master_device,
					      dev->bus, dev->slot, dev->func,
					      PCI_BAR(reg->slot + 1), &hi);

		(void) device_pci_config_write(pci_master_device,
					      dev->bus, dev->slot, dev->func,
					      PCI_BAR(reg->slot), 0xFFFFFFFFu);
		if (nslots == 2)
			(void) device_pci_config_write(pci_master_device,
					      dev->bus, dev->slot, dev->func,
					      PCI_BAR(reg->slot + 1),
					      0xFFFFFFFFu);

		(void) device_pci_config_read(pci_master_device,
					      dev->bus, dev->slot, dev->func,
					      PCI_BAR(reg->slot), &probe_lo);
		if (nslots == 2)
			(void) device_pci_config_read(pci_master_device,
					      dev->bus, dev->slot, dev->func,
					      PCI_BAR(reg->slot + 1), &probe_hi);

		(void) device_pci_config_write(pci_master_device,
					      dev->bus, dev->slot, dev->func,
					      PCI_BAR(reg->slot), lo);
		if (nslots == 2)
			(void) device_pci_config_write(pci_master_device,
					      dev->bus, dev->slot, dev->func,
					      PCI_BAR(reg->slot + 1), hi);

		/*
		 * 🔥 The restore is READ BACK, because it is the step whose
		 * failure is silent and permanent.  Everything else here can go
		 * wrong and cost a wrong size; this one leaves the device at an
		 * address nobody will look for it at, and the driver that finds
		 * nothing there hours later has no way back to this loop.
		 */
		(void) device_pci_config_read(pci_master_device,
					      dev->bus, dev->slot, dev->func,
					      PCI_BAR(reg->slot), &check);
		if (check != lo)
			printf("pci_scan: %02u:%02u.%u bar%u DID NOT TAKE ITS "
			       "ADDRESS BACK: wrote 0x%08x, reads 0x%08x\n",
			       dev->bus, dev->slot, dev->func, reg->slot,
			       lo, check);

		reg->size = pci_bar_size_from_probe(reg, (uint32_t)probe_lo,
						    (uint32_t)probe_hi);
	}

	(void) device_pci_config_write(pci_master_device,
				       dev->bus, dev->slot, dev->func,
				       PCI_COMMAND, cmd);
	return 0;
}

static int
pci_scan_scan(struct hal_device_info *out, unsigned int max)
{
	unsigned int bus, slot, func;
	unsigned int count = 0;

	if (pci_master_device == MACH_PORT_NULL) {
		printf("pci_scan: init not called\n");
		return -1;
	}

	for (bus = 0; bus < 256; bus++) {
		for (slot = 0; slot < 32; slot++) {
			for (func = 0; func < 8; func++) {
				int r;

				if (count >= max)
					return (int)count;

				r = read_pci_device(bus, slot, func,
						    &out[count]);
				if (r < 0)
					return -1;
				if (r == 0)
					continue;

				count++;
			}
		}
	}

	return (int)count;
}

/*
 * Symbol looked up by libmodload after loading "pci_scan.so":
 *   basename("pci_scan.so") + "_discovery_ops" → "pci_scan_discovery_ops"
 */
const struct hal_discovery_ops pci_scan_discovery_ops = {
	.name = "pci_scan",
	.init = pci_scan_init,
	.scan = pci_scan_scan,
	.measure = pci_scan_measure,
};
