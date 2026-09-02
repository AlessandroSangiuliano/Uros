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
 * ahci_module.c — AHCI driver module: match, probe, HBA init, IDENTIFY
 *
 * Implements the block_driver_ops interface for AHCI (SATA) controllers.
 * All AHCI HW logic from the original ahci_driver.c is here and in
 * ahci_io.c.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <sa_mach.h>
#include <device/device.h>
#include <device/device_types.h>
#include <stdio.h>
#include "../block_server.h"
#include "ahci_module.h"
#include <device/pci.h>
#include "device_master.h"

/*
 * The configuration header comes from <device/pci.h> (#427).  It used to be
 * declared here AND in ahci.h, four lines apart in the same driver.
 */

/* Static state — we support one AHCI controller for now */
/*
 * 🔴 ONE STATE PER CONTROLLER, and it used to be ONE STATE.
 *
 * `probe' hands the framework a pointer through *priv, and the framework
 * keeps it in ctrl->priv for the life of that controller.  A single static
 * instance meant every controller was handed the SAME pointer, so the second
 * probe overwrote the first controller's ABAR, its port table, its DMA
 * addresses and its disk geometry -- while the first controller's partitions
 * were already registered and pointing at it.
 *
 * ⚠️ Invisible on i386 and on the default board, where there is exactly one
 * ahci controller and a singleton is indistinguishable from a correct
 * allocation.  q35 is what exposed it: that chipset has its own AHCI at
 * 0:31.2 besides the one the command line adds, so a boot there probes twice.
 *
 * 🔑 And the way it announced itself was not a crash.  The second probe found
 * no drive, failed, and left the shared state describing a port with nothing
 * on it -- so the FIRST controller's next read timed out with SSTS=0 and the
 * command engine stopped, on a port that had reported a disk and a running
 * engine minutes earlier.  The register dump is what named it: hardware that
 * says "no device" about a device it had just identified is not hardware
 * that changed its mind.
 *
 * The slot is taken but not committed until probe succeeds, so a controller
 * that fails to come up does not consume one.  Probes are sequential.
 */
static struct ahci_state ahci_st[MAX_CONTROLLERS];
static unsigned ahci_n_states;


/* ================================================================
 * PCI match — AHCI SATA controller (class 01:06:01)
 * ================================================================ */

static int
ahci_match(unsigned int vendor_device, unsigned int class_rev)
{
	return ((class_rev >> 24) & 0xFF) == PCI_CLASS_STORAGE &&
	       ((class_rev >> 16) & 0xFF) == PCI_SUBCLASS_SATA &&
	       ((class_rev >>  8) & 0xFF) == PCI_PROGIF_AHCI;
}

/* ================================================================
 * Port start / stop helpers
 * ================================================================ */

static void
ahci_port_stop(struct ahci_state *st, int port)
{
	int i;
	uint32_t cmd;

	cmd = port_read(st, port, PORT_CMD);
	port_write(st, port, PORT_CMD, cmd & ~PORT_CMD_ST);
	for (i = 0; i < 500; i++)
		if (!(port_read(st, port, PORT_CMD) & PORT_CMD_CR))
			break;

	cmd = port_read(st, port, PORT_CMD);
	port_write(st, port, PORT_CMD, cmd & ~PORT_CMD_FRE);
	for (i = 0; i < 500; i++)
		if (!(port_read(st, port, PORT_CMD) & PORT_CMD_FR))
			break;
}

static void
ahci_port_start(struct ahci_state *st, int port)
{
	uint32_t cmd;
	int i;

	cmd = port_read(st, port, PORT_CMD);
	port_write(st, port, PORT_CMD, cmd | PORT_CMD_FRE | PORT_CMD_SUD);

	for (i = 0; i < 500000; i++)
		if (!(port_read(st, port, PORT_TFD) &
		      (PORT_TFD_STS_BSY | PORT_TFD_STS_DRQ)))
			break;

	cmd = port_read(st, port, PORT_CMD);
	port_write(st, port, PORT_CMD, cmd | PORT_CMD_ST);
}

/* ================================================================
 * HBA reset
 * ================================================================ */

static int
ahci_hba_reset(struct ahci_state *st)
{
	int i;

	if (ahci_read(st, AHCI_CAP2) & CAP2_BOH) {
		ahci_write(st, AHCI_BOHC,
			   ahci_read(st, AHCI_BOHC) | BOHC_OOS);
		for (i = 0; i < 25000; i++)
			if (!(ahci_read(st, AHCI_BOHC) & BOHC_BOS))
				break;
	}

	ahci_write(st, AHCI_GHC, GHC_AE);

	ahci_write(st, AHCI_GHC, ahci_read(st, AHCI_GHC) | GHC_HR);
	for (i = 0; i < 1000000; i++)
		if (!(ahci_read(st, AHCI_GHC) & GHC_HR))
			break;
	if (ahci_read(st, AHCI_GHC) & GHC_HR) {
		printf("ahci: HBA reset timed out\n");
		return -1;
	}

	ahci_write(st, AHCI_GHC, GHC_AE);

	printf("ahci: HBA reset OK  version=0x%08X  cap=0x%08X\n",
	       ahci_read(st, AHCI_VS), ahci_read(st, AHCI_CAP));
	return 0;
}

/* ================================================================
 * Port discovery
 * ================================================================ */

static int
ahci_port_scan(struct ahci_state *st)
{
	uint32_t pi = ahci_read(st, AHCI_PI);
	int port;

	st->n_ports = 0;
	for (port = 0; port < AHCI_MAX_PORTS; port++) {
		if (!(pi & (1u << port)))
			continue;
		if ((port_read(st, port, PORT_SSTS) & SSTS_DET_MASK)
		    != SSTS_DET_PRESENT)
			continue;
		if (st->n_ports >= MAX_AHCI_PORTS) {
			/*
			 * 🔴 A disk dropped without a word is a disk that does
			 * not exist as far as anything above here can tell.
			 * This used to `break' in silence, so a controller
			 * with more ports than the driver holds simply had
			 * fewer disks, and nothing anywhere said which ones.
			 */
			printf("ahci: PI reports more ports than the %u this "
			       "driver holds — port %d and any after it are "
			       "NOT being used\n",
			       (unsigned)MAX_AHCI_PORTS, port);
			break;
		}

		printf("ahci: device on port %d  sig=0x%08X\n",
		       port, port_read(st, port, PORT_SIG));
		st->ports[st->n_ports].hba_port = port;
		st->n_ports++;
	}

	if (st->n_ports == 0) {
		printf("ahci: no device found (PI=0x%08X)\n", pi);
		return -1;
	}
	printf("ahci: found %d port(s)\n", st->n_ports);
	return 0;
}

/* ================================================================
 * Port initialisation
 * ================================================================ */

static int
ahci_port_init(struct ahci_state *st, int port_idx)
{
	struct ahci_port_info *pi = &st->ports[port_idx];
	int hba_port = pi->hba_port;
	kern_return_t kr;
	vm_address_t dma_kva, dma_uva, dma_pa;

	kr = device_dma_alloc(st->master_device, AHCI_BDF(st), 4096,
			      &dma_kva, &dma_pa);
	if (kr != KERN_SUCCESS) {
		printf("ahci: port %d CLB/FB alloc failed\n", hba_port);
		return -1;
	}
	kr = device_dma_map_user(st->master_device, dma_kva, 4096,
				 mach_task_self(), &dma_uva);
	if (kr != KERN_SUCCESS) {
		printf("ahci: port %d CLB/FB map failed\n", hba_port);
		return -1;
	}

	pi->clb_dma  = dma_pa;
	pi->clb_uva = dma_uva;
	pi->fb_dma   = dma_pa  + 1024;
	pi->fb_uva  = dma_uva + 1024;
	pi->dma_kva = dma_kva;

	memset((void *)dma_uva, 0, 4096);

	ahci_port_stop(st, hba_port);

	/*
	 * 🔴 THE UPPER HALVES ARE WRITTEN, not zeroed (#520).  They were
	 * literal zeros, which on i386 was the only value they could have had
	 * -- a physical address is 32 bits there, so the field and the constant
	 * agreed.  Here the allocator may return a frame above 4 GiB and the
	 * controller would have fetched its command list from the bottom of
	 * memory instead: not a failure, a DMA to somewhere real.
	 *
	 * ⚠️ Order matters.  The specification has software write the low half
	 * and then the upper, and the port must be stopped for either -- which
	 * ahci_port_stop() above has just done.
	 */
	port_write(st, hba_port, PORT_CLB,  ahci_pa_lo(pi->clb_dma));
	port_write(st, hba_port, PORT_CLBU, ahci_pa_hi(pi->clb_dma));
	port_write(st, hba_port, PORT_FB,   ahci_pa_lo(pi->fb_dma));
	port_write(st, hba_port, PORT_FBU,  ahci_pa_hi(pi->fb_dma));

	port_write(st, hba_port, PORT_SERR, ~0u);
	port_write(st, hba_port, PORT_IS,   ~0u);

	port_write(st, hba_port, PORT_IE,
		   PORT_IE_DHRE | PORT_IE_SDBE | PORT_IE_TFEE);

	ahci_port_start(st, hba_port);

	printf("ahci: port %d initialised  cmd=0x%08X  tfd=0x%08X\n",
	       hba_port,
	       port_read(st, hba_port, PORT_CMD),
	       port_read(st, hba_port, PORT_TFD));
	return 0;
}

/* ================================================================
 * IDENTIFY DEVICE
 * ================================================================ */

static int
ahci_identify(struct ahci_state *st, int port_idx)
{
	struct ahci_port_info *pi = &st->ports[port_idx];
	struct ata_fis_h2d fis;
	uint16_t *buf = (uint16_t *)st->data_uva;
	char model[41];
	uint32_t lba28;
	uint64_t sectors;
	unsigned int i;

	memset(&fis, 0, sizeof(fis));
	fis.fis_type = FIS_TYPE_H2D;
	fis.flags    = FIS_H2D_FLAG_CMD;
	fis.command  = ATA_CMD_IDENTIFY;
	fis.device   = 0;

	if (ahci_submit_cmd(st, port_idx, &fis, st->data_dma, 512, 0) < 0) {
		printf("ahci: IDENTIFY failed on port %d\n", pi->hba_port);
		return -1;
	}

	for (i = 0; i < 20; i++) {
		uint16_t w = buf[27 + i];
		model[i * 2]     = (char)((w >> 8) & 0xFF);
		model[i * 2 + 1] = (char)(w & 0xFF);
	}
	model[40] = '\0';
	for (i = 39; i > 0 && model[i] == ' '; i--)
		model[i] = '\0';

	/*
	 * Capacity.  Words 60-61 hold a 28-bit sector count, which the ATA
	 * spec saturates at 0x0FFFFFFF for anything past 128 GiB — a 1 TB
	 * disk reported exactly that and came out as ~131071 MB (#399).  When
	 * the device announces LBA48 the real count lives in words 100-103,
	 * so prefer that.  The I/O path already issues READ/WRITE DMA EXT and
	 * NCQ commands, so nothing else has to change to address it.
	 */
	lba28 = ((uint32_t)buf[ATA_ID_LBA28_CAP + 1] << 16) |
		 buf[ATA_ID_LBA28_CAP];
	sectors = lba28;

	if (buf[ATA_ID_CMD_SET_2] & ATA_ID_CMD_SET_2_LBA48) {
		uint64_t lba48 = 0;
		for (i = 0; i < 4; i++)
			lba48 |= (uint64_t)buf[ATA_ID_LBA48_CAP + i] << (16 * i);
		if (lba48 > sectors)
			sectors = lba48;
	}

	/*
	 * The block stack carries sector counts as uint32_t, so it can
	 * address just under 2 TiB.  Report the truth and clamp rather than
	 * truncating silently; a bigger disk stays usable up to the limit.
	 */
	if (sectors > 0xFFFFFFFFull) {
		printf("ahci: port %d has %llu sectors; clamping to the "
		       "32-bit sector limit (~2 TiB addressable)\n",
		       pi->hba_port, (unsigned long long)sectors);
		sectors = 0xFFFFFFFFull;
	}
	pi->disk_sectors = (uint32_t)sectors;

	printf("ahci: port %d model: %s\n", pi->hba_port, model);
	printf("ahci: port %d sectors: %u  capacity: ~%u MB%s\n",
	       pi->hba_port, pi->disk_sectors, pi->disk_sectors / 2048,
	       (sectors > lba28) ? " (LBA48)" : "");

	{
		uint32_t cap = ahci_read(st, AHCI_CAP);
		int hba_ncq = (cap & CAP_SNCQ) != 0;
		unsigned int hba_slots =
			((cap >> CAP_NCS_SHIFT) & CAP_NCS_MASK) + 1;
		int dev_ncq = (buf[ATA_ID_SATA_CAP] & ATA_ID_SATA_CAP_NCQ) != 0;
		unsigned int dev_qdepth = (buf[ATA_ID_QUEUE_DEPTH] & 0x1F) + 1;

		if (hba_ncq && dev_ncq) {
			pi->ncq_supported = 1;
			pi->ncq_depth = hba_slots < dev_qdepth
				    ? hba_slots : dev_qdepth;
			printf("ahci: port %d NCQ depth=%u\n",
			       pi->hba_port, pi->ncq_depth);
		} else {
			pi->ncq_supported = 0;
			pi->ncq_depth = 0;
		}

		if (hba_slots > st->batch_slots)
			st->batch_slots = hba_slots;
	}

	return 0;
}

/* ================================================================
 * Reallocate DMA buffers for detected batch_slots
 * ================================================================ */

static int
ahci_realloc_batch_buffers(struct ahci_state *st)
{
	kern_return_t kr;
	unsigned int ct_size;
	unsigned int n_pages;
	mach_msg_type_number_t pa_count = 1024;

	if (st->batch_slots <= 1)
		return 0;

	ct_size = st->batch_slots * CT_STRIDE;
	ct_size = (ct_size + 4095u) & ~4095u;

	/* Release old user mapping before freeing kernel DMA buffer */
	vm_deallocate(mach_task_self(), st->ct_uva, 4096);
	device_dma_free(st->master_device, AHCI_BDF(st), st->ct_kva, 4096);

	kr = device_dma_alloc(st->master_device, AHCI_BDF(st), ct_size,
			      &st->ct_kva, &st->ct_dma);
	if (kr != KERN_SUCCESS) {
		printf("ahci: CT realloc (%u bytes) failed\n", ct_size);
		return -1;
	}
	kr = device_dma_map_user(st->master_device, st->ct_kva, ct_size,
				 mach_task_self(), &st->ct_uva);
	if (kr != KERN_SUCCESS) {
		printf("ahci: CT remap failed\n");
		return -1;
	}

	/* Release old user mapping before freeing kernel DMA buffer */
	vm_deallocate(mach_task_self(), st->data_uva, 4096);
	device_dma_free(st->master_device, AHCI_BDF(st), st->data_kva, 4096);

	n_pages = st->batch_slots * PRDT_PER_SLOT;
	if (n_pages > AHCI_MAX_SG_PAGES)
		n_pages = AHCI_MAX_SG_PAGES;

	/*
	 * #520: the list comes back OUT OF LINE now, so `pa_list' is memory
	 * the kernel handed over and this task owns.  It is copied into the
	 * driver's own array and released at once -- keeping it would mean a
	 * live vm_allocate for the life of the controller, to hold what fits
	 * in a struct field.
	 */
	{
		vm_address_t *pa_list = NULL;

		pa_count = 0;
		kr = device_dma_alloc_sg(st->master_device, AHCI_BDF(st), n_pages,
					 mach_task_self(),
					 &st->data_kva, &st->data_uva,
					 &pa_list, &pa_count);
		if (kr != KERN_SUCCESS) {
			printf("ahci: scatter-gather alloc (%u pages) "
			       "failed (kr=%d)\n", n_pages, kr);
			return -1;
		}

		if (pa_count > AHCI_MAX_SG_PAGES)
			pa_count = AHCI_MAX_SG_PAGES;

		for (unsigned p = 0; p < pa_count; p++)
			st->data_dma_list[p] = pa_list[p];

		/*
		 * ⚠️ Released whatever happens next.  Out-of-line memory a
		 * receiver forgets is a leak of one allocation per mount,
		 * which is the shape of leak that never gets noticed.
		 */
		(void) vm_deallocate(mach_task_self(),
				     (vm_address_t)pa_list,
				     (vm_size_t)pa_count
				     * sizeof(vm_address_t));
	}

	st->data_n_pages = n_pages;
	st->data_dma = st->data_dma_list[0];

	st->batch_data_size = st->batch_slots * SLOT_DATA_SIZE;
	st->ra_sectors = st->batch_slots * SECTORS_PER_SLOT;

	printf("ahci: DMA reallocated: ct=%u bytes  data=%u pages "
	       "(%u KB, scatter-gather, %u slots)\n",
	       ct_size, n_pages, n_pages * 4, st->batch_slots);

	return 0;
}

/* ================================================================
 * The isolation, demonstrated rather than asserted (#432 stage 3d)
 * ================================================================ */

/*
 * Ask this controller to read a sector into a page it was never granted, and
 * require the transfer to FAIL.
 *
 * 🔴 THIS IS THE DONE-WHEN OF #432 AND NOT A DECORATION.  Everything else in
 * that issue can be true of a kernel that quietly polices nothing: the tables
 * are built, they read back, the engine says translation is on, and every
 * driver goes on working -- which is also exactly what happens if the domain
 * is never consulted.  The only observation that tells the two apart is a DMA
 * that is REFUSED, and it has to be one this code caused on purpose.
 *
 * 🔑 THE TARGET PAGE IS SAFE, AND CHOOSING IT WAS THE WHOLE DESIGN.  It is a
 * page allocated with DEVICE_DMA_NO_BDF -- real memory, wired, owned by this
 * server, and in NO device's domain.  So:
 *
 *   - if the isolation works, the read is refused and nothing is written;
 *   - if it does not, one sector lands in a page of OUR OWN that nothing
 *     reads.
 *
 * ⚠️ Every other candidate is worse.  An address past the top of memory fails
 * for a reason that is not the IOMMU; a page granted to another device
 * corrupts that device's buffer when the isolation is absent; an arbitrary
 * physical address corrupts the kernel.  A test whose failure mode is "the
 * machine is now wrong somewhere else" cannot be run on every boot.
 *
 * ⚠️ And it says so and moves on when the machine does not remap.  A default
 * boot, an i386, a board with no engine: device_dma_faults answers zero
 * because nothing was refused, which is indistinguishable from "the transfer
 * worked" -- so the verdict is read from whether the machine CAN refuse,
 * asked before the attempt rather than inferred from it.
 */
static void
ahci_iommu_selftest(struct ahci_state *st)
{
	vm_address_t	kva = 0, pa = 0;
	natural_t	confined = 0, before = 0, after = 0;
	vm_address_t	refused = 0;
	kern_return_t	kr;
	int		rc;

	if (st->n_ports == 0)
		return;

	/*
	 * 🔑 ASKED BEFORE, so that "no faults" afterwards means something.  A
	 * count read only after the attempt cannot tell a refusal that
	 * happened from one that happened earlier, and cannot tell either from
	 * a machine that never refuses anything.
	 */
	kr = device_dma_faults(st->master_device, AHCI_BDF(st),
			       &confined, &before, &refused);
	if (kr != KERN_SUCCESS)
		return;

	/*
	 * 🔴 AND IF THIS DEVICE IS NOT CONFINED, THERE IS NOTHING TO
	 * DEMONSTRATE.  A default boot, an i386, a board with no engine: the
	 * DMA below would succeed, and printing "nothing is policing its DMA"
	 * there is true and useless -- an alarm on every boot of every machine
	 * that never asked to be policed.  Said once, quietly, and skipped.
	 */
	if (!confined) {
		printf("ahci: [iommu] this controller is not in a domain — "
		       "its DMA reaches all of memory, as it always has\n");
		return;
	}

	kr = device_dma_alloc(st->master_device, DEVICE_DMA_NO_BDF, 4096,
			      &kva, &pa);
	if (kr != KERN_SUCCESS)
		return;

	printf("ahci: [iommu] asking port 0 to read one sector into "
	       "0x%08lX, a page granted to no device\n", (unsigned long)pa);

	{
		struct ata_fis_h2d fis;

		memset(&fis, 0, sizeof(fis));
		fis.fis_type	= FIS_TYPE_H2D;
		fis.flags	= FIS_H2D_FLAG_CMD;
		fis.command	= ATA_CMD_READ_DMA_EXT;
		fis.device	= ATA_DEV_LBA;
		fis.sector_count = 1;

		rc = ahci_submit_cmd(st, 0, &fis, pa, 512, 0);
	}

	kr = device_dma_faults(st->master_device, AHCI_BDF(st),
			       &confined, &after, &refused);

	/*
	 * 🔥 AND THE COMMAND REPORTS SUCCESS.  rc comes back 0: the controller
	 * cleared CI, raised no task-file error, and told this driver the read
	 * was done -- while not one byte reached memory.  That is not a defect
	 * in the engine or in AHCI; a DMA write refused on the bus is not an
	 * ATA error, and there is nowhere for the device to put it.
	 *
	 * 🔑 WHICH IS THE ARGUMENT FOR device_dma_faults() IN ONE LINE.  Every
	 * symptom available to a driver says the transfer worked, so a driver
	 * that could not ask the kernel would go looking for a corrupt disk.
	 */
	if (kr == KERN_SUCCESS && after > before && refused != 0)
		printf("ahci: [iommu] REFUSED at 0x%08lX — and the command "
		       "returned %d, so the DEVICE never noticed: the domain "
		       "is enforced, and only the kernel can say so\n",
		       (unsigned long)refused, rc);
	/*
	 * ⚠️ REFUSED, AND THE ENGINE DID NOT SAY WHERE.  Reported apart from
	 * the case above rather than printed as "at 0x00000000", which is an
	 * address and would be read as one.  It is what QEMU's amd-iommu does:
	 * the refusal is real and the address quadword of its event is zero.
	 */
	else if (kr == KERN_SUCCESS && after > before)
		printf("ahci: [iommu] REFUSED %u time(s), and the command "
		       "returned %d — but the engine recorded no address, so "
		       "the refusal is known and the page is not\n",
		       (unsigned)(after - before), rc);
	else if (rc < 0)
		printf("ahci: [iommu] the transfer failed (%d) and no refusal "
		       "was recorded — blocked, but not by anything that "
		       "said so\n", rc);
	else
		printf("ahci: [iommu] the transfer SUCCEEDED into a page this "
		       "device was never granted, though the kernel says it "
		       "is confined — THE DOMAIN IS BUILT AND NOT ENFORCED\n");

	device_dma_free(st->master_device, DEVICE_DMA_NO_BDF, kva, 4096);
}

/* ================================================================
 * Probe — full AHCI controller initialisation
 * ================================================================ */

static int
ahci_probe(unsigned int bus, unsigned int slot, unsigned int func,
	   mach_port_t master_dev, mach_port_t irq,
	   const struct pci_bar_region *bars, unsigned int n_bars,
	   void **priv)
{
	struct ahci_state *st;

	if (ahci_n_states >= MAX_CONTROLLERS) {
		printf("ahci: %u controllers already, and the framework holds "
		       "no more — refusing to probe %u:%u.%u\n",
		       ahci_n_states, bus, slot, func);
		return -1;
	}
	st = &ahci_st[ahci_n_states];
	memset(st, 0, sizeof(*st));
	kern_return_t kr;
	const struct pci_bar_region *abar_region;
	uint64_t abar_phys;
	unsigned int cmd_reg, irq_reg;
	int i;

	st->pci_bus  = bus;
	st->pci_slot = slot;
	st->pci_func = func;
	st->master_device = master_dev;
	st->batch_slots = 1;

	/*
	 * The ABAR, from the regions the HAL decoded (#427).
	 *
	 * 🔑 Found by the SLOT it starts at and not by index.  AHCI puts the
	 * ABAR in slot 5, and slot 5 is not region 5: this controller reports
	 * two regions, an I/O one at slot 4 and this one, so indexing would
	 * have read the wrong one on the machine it was written for.
	 *
	 * ⚠️ Slot 5 can only be 32 bits wide -- a 64-bit BAR there would need
	 * a slot 6, which a type 0 header does not have -- so this driver
	 * never meets the two-slot case.  The lookup goes through the decode
	 * anyway, because the reason it is safe is a property of the standard
	 * and not something this file should be asserting on its own.
	 */
	abar_region = NULL;
	for (i = 0; i < (int)n_bars; i++)
		if (bars[i].slot == 5 && !(bars[i].flags & PCI_REGION_IO))
			abar_region = &bars[i];

	if (abar_region == NULL) {
		printf("ahci: no memory region at BAR slot 5 among %u "
		       "region(s) — this is not an AHCI controller, or the "
		       "HAL did not decode it\n", n_bars);
		return -1;
	}
	abar_phys = abar_region->base;
	printf("ahci: ABAR phys = 0x%08X%08X\n",
	       (unsigned int)(abar_phys >> 32),
	       (unsigned int)(abar_phys & 0xFFFFFFFFu));

	/* Enable PCI bus master + memory space */
	kr = device_pci_config_read(master_dev, bus, slot, func,
				    PCI_COMMAND, &cmd_reg);
	if (kr == KERN_SUCCESS) {
		cmd_reg |= PCI_CMD_MEM_ENABLE | PCI_CMD_BUS_MASTER;
		device_pci_config_write(master_dev, bus, slot, func,
					PCI_COMMAND, cmd_reg);
	}

	/* Read IRQ */
	kr = device_pci_config_read(master_dev, bus, slot, func,
				    PCI_INTERRUPT_LINE, &irq_reg);
	if (kr == KERN_SUCCESS) {
		st->ahci_irq = irq_reg & 0xFF;
		printf("ahci: IRQ = %u\n", st->ahci_irq);
	}

	/* Map ABAR */
	/*
	 * #427: no casts on either side any more.  This was
	 *
	 *	device_mmio_map(..., bar5 & ~0xFu, ..., (unsigned int *)&st->abar)
	 *
	 * -- a four-byte store into a pointer that is eight bytes wide on the
	 * target this is being ported to.  The driver was written correctly
	 * against a contract that was wrong, and the contract is what changed:
	 * the RPC now carries vm_address_t in and out, so the physical address
	 * goes in whole and the user address comes back whole.
	 */
	kr = device_mmio_map(master_dev, (vm_address_t)abar_phys,
			     AHCI_ABAR_SIZE,
			     mach_task_self(), (vm_address_t *)&st->abar);
	if (kr != KERN_SUCCESS) {
		printf("ahci: device_mmio_map failed (kr=%d)\n", kr);
		return -1;
	}
	/*
	 * ⚠️ %p and not %08X.  It was the latter, which on i386 printed the
	 * whole address and here would print its bottom half -- the exact
	 * "printed a vm_offset_t with %x" that <mach/x86_64/vm_types.h> names.
	 * A diagnostic that lies about an address is worse than none: it is the
	 * line somebody reads while looking for why a mapping is wrong.
	 */
	printf("ahci: ABAR mapped at uva=%p\n", (void *)st->abar);

	/* Initial CT buffer (1 page) */
	kr = device_dma_alloc(master_dev, AHCI_BDF(st), 4096,
			      &st->ct_kva, &st->ct_dma);
	if (kr != KERN_SUCCESS) { printf("ahci: CT alloc failed\n"); return -1; }
	kr = device_dma_map_user(master_dev, st->ct_kva, 4096,
				 mach_task_self(), &st->ct_uva);
	if (kr != KERN_SUCCESS) { printf("ahci: CT map failed\n"); return -1; }

	/* Initial data buffer (1 page) */
	kr = device_dma_alloc(master_dev, AHCI_BDF(st), 4096,
			      &st->data_kva, &st->data_dma);
	if (kr != KERN_SUCCESS) { printf("ahci: data alloc failed\n"); return -1; }
	kr = device_dma_map_user(master_dev, st->data_kva, 4096,
				 mach_task_self(), &st->data_uva);
	if (kr != KERN_SUCCESS) { printf("ahci: data map failed\n"); return -1; }

	st->data_dma_list[0] = st->data_dma;
	st->data_n_pages = 1;

	printf("ahci: DMA: ct at 0x%08lX, data at 0x%08lX — the addresses\n\t       the CONTROLLER is programmed with, not where the memory is\n",
	       (unsigned long) st->ct_dma,
	       (unsigned long) st->data_dma);

	/* HBA reset + AHCI enable */
	if (ahci_hba_reset(st) < 0)
		return -1;

	/* Find and init ports */
	if (ahci_port_scan(st) < 0)
		return -1;

	for (i = 0; i < st->n_ports; i++) {
		if (ahci_port_init(st, i) < 0)
			return -1;
	}

	/* Register IRQ */
	if (st->ahci_irq > 0 && st->ahci_irq < 16) {
		kr = device_intr_register(master_dev, st->ahci_irq, irq,
					  MACH_MSG_TYPE_MAKE_SEND);
		if (kr == KERN_SUCCESS)
			printf("ahci: IRQ %u registered\n", st->ahci_irq);
	}

	/* Identify disks and detect batch slots */
	for (i = 0; i < st->n_ports; i++)
		ahci_identify(st, i);

	st->batch_data_size = st->batch_slots * SLOT_DATA_SIZE;
	st->ra_sectors = st->batch_slots * SECTORS_PER_SLOT;
	printf("ahci: batch_slots=%u  data=%u KB  readahead=%u sectors\n",
	       st->batch_slots, st->batch_data_size / 1024, st->ra_sectors);

	if (ahci_realloc_batch_buffers(st) < 0) {
		printf("ahci: DMA realloc failed, falling back to 1 slot\n");
		st->batch_slots = 1;
		st->batch_data_size = SLOT_DATA_SIZE;
		st->ra_sectors = SECTORS_PER_SLOT;
	}

	/*
	 * Enable AHCI global interrupt delivery (#222).  Safe now that the
	 * kernel masks the line at the PIC on each forwarded notification
	 * and the userspace handler re-unmasks via device_intr_enable after
	 * clearing PORT_IS — no level-triggered storm.
	 */
	ahci_write(st, AHCI_GHC, ahci_read(st, AHCI_GHC) | GHC_IE);

	ahci_iommu_selftest(st);

	ahci_n_states++;		/* committed: this controller came up */

	*priv = st;
	return 0;
}

/* ================================================================
 * get_disks — return disk info for each port
 * ================================================================ */

static int
ahci_get_disks(void *priv, struct blk_disk_info *info, int max_disks)
{
	struct ahci_state *st = (struct ahci_state *)priv;
	int i, count;

	count = st->n_ports;
	if (count > max_disks)
		count = max_disks;

	for (i = 0; i < count; i++) {
		info[i].total_sectors    = st->ports[i].disk_sectors;
		info[i].ncq_supported    = st->ports[i].ncq_supported;
		info[i].ncq_depth        = st->ports[i].ncq_depth;
		info[i].max_transfer_bytes = st->batch_data_size;
	}

	return count;
}

/* ================================================================
 * Module ops callbacks — delegate to ahci_io.c
 * ================================================================ */

static int
ahci_mod_read_sectors(void *priv, int disk, uint32_t lba,
		      unsigned int count,
		      vm_offset_t *buf, unsigned int *buf_size)
{
	struct ahci_state *st = (struct ahci_state *)priv;
	unsigned int total = count * SECTOR_SIZE;
	unsigned int offset, chunk;

	for (offset = 0; offset < total; offset += chunk) {
		unsigned int batch_sects;

		chunk = total - offset;
		if (chunk > st->batch_data_size)
			chunk = st->batch_data_size;
		batch_sects = chunk / SECTOR_SIZE;

		if (ahci_submit_batch(st, disk,
				      lba + offset / SECTOR_SIZE,
				      batch_sects, 0) < 0)
			return -1;
	}

	/*
	 * Data is in the DMA buffer (st->data_uva).
	 * Allocate a user buffer and copy it out.
	 */
	{
		kern_return_t kr;
		vm_offset_t out;

		kr = vm_allocate(mach_task_self(), &out, total, TRUE);
		if (kr != KERN_SUCCESS)
			return -1;
		memcpy((void *)out, (void *)st->data_uva, total);
		*buf = out;
		*buf_size = total;
	}
	return 0;
}

static int
ahci_mod_write_sectors(void *priv, int disk, uint32_t lba,
		       unsigned int count,
		       vm_offset_t buf, unsigned int size)
{
	struct ahci_state *st = (struct ahci_state *)priv;
	unsigned int total = count * SECTOR_SIZE;
	unsigned int offset, chunk;

	for (offset = 0; offset < total; offset += chunk) {
		unsigned int batch_sects;

		chunk = total - offset;
		if (chunk > st->batch_data_size)
			chunk = st->batch_data_size;
		batch_sects = chunk / SECTOR_SIZE;

		memcpy((void *)st->data_uva,
		       (void *)(buf + offset), chunk);
		if (ahci_submit_batch(st, disk,
				      lba + offset / SECTOR_SIZE,
				      batch_sects, 1) < 0)
			return -1;
	}
	return 0;
}

static void
ahci_mod_irq_handler(void *priv)
{
	struct ahci_state *st = (struct ahci_state *)priv;
	int i;

	ahci_write(st, AHCI_IS, ~0u);
	for (i = 0; i < st->n_ports; i++)
		port_write(st, st->ports[i].hba_port, PORT_IS, ~0u);

	/*
	 * Device-side status cleared above; re-unmask the line at the PIC
	 * so the next assertion (level-triggered PCI) can fire (#222).
	 */
	(void)device_intr_enable(st->master_device, st->ahci_irq);
}

static int
ahci_mod_read_phys(void *priv, int disk, uint32_t lba,
		   unsigned int count,
		   vm_address_t *phys_addrs, unsigned int n_pa,
		   unsigned int total_bytes)
{
	struct ahci_state *st = (struct ahci_state *)priv;
	return ahci_submit_phys(st, disk, lba, count, 0,
				phys_addrs, n_pa, total_bytes);
}

static int
ahci_mod_write_phys(void *priv, int disk, uint32_t lba,
		    unsigned int count,
		    vm_address_t *phys_addrs, unsigned int n_pa,
		    unsigned int total_bytes)
{
	struct ahci_state *st = (struct ahci_state *)priv;
	return ahci_submit_phys(st, disk, lba, count, 1,
				phys_addrs, n_pa, total_bytes);
}

static int
ahci_mod_write_batch(void *priv, int disk,
		     uint32_t *lbas, unsigned int *sizes, unsigned int n,
		     vm_offset_t buf, unsigned int buf_size)
{
	struct ahci_state *st = (struct ahci_state *)priv;
	unsigned int i, data_off = 0;

	for (i = 0; i < n; i++) {
		unsigned int sz = sizes[i];
		unsigned int wr_total, lba, off, chunk;

		if (sz == 0)
			continue;
		if (data_off + sz > buf_size)
			return -1;

		wr_total = (sz + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
		lba = lbas[i];

		for (off = 0; off < wr_total; off += chunk) {
			unsigned int sects, cpy;

			chunk = wr_total - off;
			if (chunk > st->batch_data_size)
				chunk = st->batch_data_size;
			sects = chunk / SECTOR_SIZE;

			cpy = (off + chunk <= sz) ? chunk
						  : (sz > off ? sz - off : 0);
			if (cpy < chunk)
				memset((void *)st->data_uva, 0, chunk);
			if (cpy > 0)
				memcpy((void *)st->data_uva,
				       (void *)(buf + data_off + off), cpy);
			if (ahci_submit_batch(st, disk, lba,
					      sects, 1) < 0)
				return -1;
			lba += sects;
		}
		data_off += sz;
	}
	return 0;
}

/* ================================================================
 * Module ops vtable
 * ================================================================ */

const struct block_driver_ops ahci_module_ops = {
	.name			= "ahci",
	.match			= ahci_match,
	.probe			= ahci_probe,
	.get_disks		= ahci_get_disks,
	.read_sectors		= ahci_mod_read_sectors,
	.write_sectors		= ahci_mod_write_sectors,
	.irq_handler		= ahci_mod_irq_handler,
	.read_sectors_phys	= ahci_mod_read_phys,
	.write_sectors_phys	= ahci_mod_write_phys,
	.write_batch		= ahci_mod_write_batch,
};
