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
 * ahci_driver.c — Userspace AHCI (SATA) driver for Uros
 *
 * Approach B: kernel provides PCI config, DMA alloc, MMIO mapping and
 * IRQ forwarding via device_master RPCs; all AHCI logic runs here.
 *
 * Boot flow:
 *   bootstrap → main() → pci_scan() → ahci_init()
 *   → ahci_hba_reset() → ahci_port_find() → ahci_port_init()
 *   → ahci_identify() → ahci_benchmark() → message loop
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/bootstrap.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <mach/mig_errors.h>
#include <sa_mach.h>
#include <device/device.h>
#include <device/device_types.h>
#include <servers/netname.h>
#include <stdio.h>
#include "device_master.h"
#include "device_server.h"
#include "ahci.h"

/* msgh_id base for IRQ notifications from device_intr_register */
#define IRQ_NOTIFY_MSGH_BASE	3000

/* ================================================================
 * Globals
 * ================================================================ */

static mach_port_t	host_port;
static mach_port_t	device_port;
static mach_port_t	master_device;
static mach_port_t	security_port;
static mach_port_t	root_ledger_wired;
static mach_port_t	root_ledger_paged;
static mach_port_t	irq_port;
static mach_port_t	ahci_service_port;	/* RPC port registered with netname */
static mach_port_t	port_set;	/* IRQ + RPC combined receive set */

/* PCI location of the AHCI controller */
static unsigned int	ahci_bus, ahci_slot, ahci_func;
static unsigned int	ahci_irq;
static unsigned int	ahci_abar_phys;

/*
 * ABAR mapped into our address space via device_mmio_map.
 * All AHCI register accesses go through ahci_read/ahci_write.
 */
static volatile uint32_t *abar;

/* Active SATA port */
static int		active_port = -1;

/* Total disk sectors (from IDENTIFY) */
static uint32_t		disk_sectors;

/*
 * DMA buffers — each has:
 *   kva  : kernel VA (from device_dma_alloc, used as handle for dma_free)
 *   uva  : user VA   (from device_dma_map_user, CPU-accessible here)
 *   pa   : physical address (programmed into AHCI registers)
 */
static unsigned int	clb_kva,  clb_uva,  clb_pa;	/* Command List Base  1 KB */
static unsigned int	fb_kva,   fb_uva,   fb_pa;	/* FIS Base          256 B */
static unsigned int	ct_kva,   ct_uva,   ct_pa;	/* Command Table     4 KB */
static unsigned int	data_kva, data_uva, data_pa;	/* Data buffer       4 KB */

/* ================================================================
 * MMIO accessors
 * ================================================================ */

static inline uint32_t
ahci_read(unsigned int reg)
{
	return *(volatile uint32_t *)((uint8_t *)abar + reg);
}

static inline void
ahci_write(unsigned int reg, uint32_t val)
{
	*(volatile uint32_t *)((uint8_t *)abar + reg) = val;
}

static inline uint32_t
port_read(int port, unsigned int reg)
{
	return ahci_read(AHCI_PORT_BASE + port * AHCI_PORT_SIZE + reg);
}

static inline void
port_write(int port, unsigned int reg, uint32_t val)
{
	ahci_write(AHCI_PORT_BASE + port * AHCI_PORT_SIZE + reg, val);
}

/* ================================================================
 * Timing — RDTSC
 * ================================================================ */

static inline uint64_t
rdtsc(void)
{
	uint64_t tsc;
	__asm__ volatile("rdtsc" : "=A" (tsc));
	return tsc;
}

/* ================================================================
 * PCI scan — find AHCI controller
 * ================================================================ */

static int
pci_scan_ahci(void)
{
	unsigned int bus, slot, func;
	unsigned int vendor_device, class_rev;
	kern_return_t kr;

	printf("ahci: scanning PCI bus...\n");

	for (bus = 0; bus < 256; bus++) {
		for (slot = 0; slot < 32; slot++) {
			for (func = 0; func < 8; func++) {
				kr = device_pci_config_read(master_device,
					bus, slot, func, PCI_VENDOR_ID,
					&vendor_device);
				if (kr != KERN_SUCCESS)
					continue;
				if (vendor_device == 0xFFFFFFFF ||
				    vendor_device == 0)
					continue;

				kr = device_pci_config_read(master_device,
					bus, slot, func, PCI_CLASS_REV,
					&class_rev);
				if (kr != KERN_SUCCESS)
					continue;

				if (((class_rev >> 24) & 0xFF) == PCI_CLASS_STORAGE &&
				    ((class_rev >> 16) & 0xFF) == PCI_SUBCLASS_SATA &&
				    ((class_rev >>  8) & 0xFF) == PCI_PROGIF_AHCI) {
					ahci_bus  = bus;
					ahci_slot = slot;
					ahci_func = func;
					printf("ahci: found controller at PCI %u:%u.%u "
					       "vendor:device=0x%08X\n",
					       bus, slot, func, vendor_device);
					return 1;
				}
			}
		}
	}

	printf("ahci: no AHCI controller found\n");
	return 0;
}

/* ================================================================
 * Port start / stop helpers
 * ================================================================ */

static void
port_stop(int port)
{
	int i;
	uint32_t cmd;

	/* Clear ST — stop command list processing */
	cmd = port_read(port, PORT_CMD);
	port_write(port, PORT_CMD, cmd & ~PORT_CMD_ST);
	for (i = 0; i < 500; i++)
		if (!(port_read(port, PORT_CMD) & PORT_CMD_CR))
			break;

	/* Clear FRE — stop FIS receive */
	cmd = port_read(port, PORT_CMD);
	port_write(port, PORT_CMD, cmd & ~PORT_CMD_FRE);
	for (i = 0; i < 500; i++)
		if (!(port_read(port, PORT_CMD) & PORT_CMD_FR))
			break;
}

static void
port_start(int port)
{
	uint32_t cmd;
	int i;

	/*
	 * Enable FIS Receive first so the device can send its signature FIS.
	 * Do NOT wait for BSY/DRQ here: right after HBA reset PORT_TFD may
	 * show BSY until the device responds, causing an infinite spin.
	 */
	cmd = port_read(port, PORT_CMD);
	port_write(port, PORT_CMD, cmd | PORT_CMD_FRE | PORT_CMD_SUD);

	/* Wait up to 500 ms for BSY/DRQ to clear before starting DMA engine */
	for (i = 0; i < 500000; i++)
		if (!(port_read(port, PORT_TFD) &
		      (PORT_TFD_STS_BSY | PORT_TFD_STS_DRQ)))
			break;

	/* Start command list processing */
	cmd = port_read(port, PORT_CMD);
	port_write(port, PORT_CMD, cmd | PORT_CMD_ST);
}

/* ================================================================
 * HBA reset and global init
 * ================================================================ */

static int
ahci_hba_reset(void)
{
	int i;

	/* BIOS/OS handoff if supported */
	if (ahci_read(AHCI_CAP2) & CAP2_BOH) {
		ahci_write(AHCI_BOHC,
			   ahci_read(AHCI_BOHC) | BOHC_OOS);
		for (i = 0; i < 25000; i++)
			if (!(ahci_read(AHCI_BOHC) & BOHC_BOS))
				break;
	}

	/* Enable AHCI mode before reset */
	ahci_write(AHCI_GHC, GHC_AE);

	/* HBA reset */
	ahci_write(AHCI_GHC, ahci_read(AHCI_GHC) | GHC_HR);
	for (i = 0; i < 1000000; i++)
		if (!(ahci_read(AHCI_GHC) & GHC_HR))
			break;
	if (ahci_read(AHCI_GHC) & GHC_HR) {
		printf("ahci: HBA reset timed out\n");
		return -1;
	}

	/* Re-enable AHCI after reset */
	ahci_write(AHCI_GHC, GHC_AE);

	printf("ahci: HBA reset OK  version=0x%08X  cap=0x%08X\n",
	       ahci_read(AHCI_VS), ahci_read(AHCI_CAP));
	return 0;
}

/* ================================================================
 * Port discovery
 * ================================================================ */

static int
ahci_port_find(void)
{
	uint32_t pi = ahci_read(AHCI_PI);
	int port;

	for (port = 0; port < AHCI_MAX_PORTS; port++) {
		if (!(pi & (1u << port)))
			continue;
		if ((port_read(port, PORT_SSTS) & SSTS_DET_MASK)
		    == SSTS_DET_PRESENT) {
			printf("ahci: device on port %d  sig=0x%08X\n",
			       port, port_read(port, PORT_SIG));
			return port;
		}
	}

	printf("ahci: no device found (PI=0x%08X)\n", pi);
	return -1;
}

/* ================================================================
 * Port initialisation
 * ================================================================ */

static int
ahci_port_init(int port)
{
	port_stop(port);

	/* Set command list and FIS receive areas */
	port_write(port, PORT_CLB,  clb_pa);
	port_write(port, PORT_CLBU, 0);
	port_write(port, PORT_FB,   fb_pa);
	port_write(port, PORT_FBU,  0);

	/* Clear errors and interrupt status */
	port_write(port, PORT_SERR, ~0u);
	port_write(port, PORT_IS,   ~0u);

	port_start(port);

	printf("ahci: port %d initialised  cmd=0x%08X  tfd=0x%08X\n",
	       port,
	       port_read(port, PORT_CMD),
	       port_read(port, PORT_TFD));
	return 0;
}

/* ================================================================
 * Command submission (polling, slot 0 only)
 * ================================================================ */

static int
ahci_submit_cmd(int port, struct ata_fis_h2d *fis,
		unsigned int buf_pa, unsigned int buf_size,
		int write)
{
	struct ahci_cmd_hdr   *hdr = (struct ahci_cmd_hdr *)clb_uva;
	struct ahci_cmd_table *tbl = (struct ahci_cmd_table *)ct_uva;
	int i;

	/* Wait for port idle */
	for (i = 0; i < 1000000; i++)
		if (!(port_read(port, PORT_TFD) &
		      (PORT_TFD_STS_BSY | PORT_TFD_STS_DRQ)))
			break;

	/* Clear interrupt status */
	port_write(port, PORT_IS, ~0u);

	/* Fill command header for slot 0 */
	hdr[0].opts  = CMD_HDR_CFL(5);
	if (write)
		hdr[0].opts |= CMD_HDR_W;
	hdr[0].prdtl = (buf_size > 0) ? 1 : 0;
	hdr[0].prdbc = 0;
	hdr[0].ctba  = ct_pa;
	hdr[0].ctbau = 0;
	for (i = 0; i < 4; i++)
		hdr[0].rsvd[i] = 0;

	/* Fill command table */
	memset(tbl, 0, sizeof(struct ahci_cmd_table));
	memcpy(tbl->cfis, fis, 20);

	if (buf_size > 0) {
		tbl->prdt[0].dba  = buf_pa;
		tbl->prdt[0].dbau = 0;
		tbl->prdt[0].rsvd = 0;
		tbl->prdt[0].dbc  = PRDT_DBC(buf_size) | PRDT_IOC;
	}

	/* Issue command on slot 0 */
	port_write(port, PORT_CI, 1u);

	/* Poll for completion (~5 s timeout) */
	for (i = 0; i < 5000000; i++) {
		uint32_t is = port_read(port, PORT_IS);
		if (is & PORT_IS_TFES) {
			printf("ahci: task file error  IS=0x%08X  TFD=0x%08X\n",
			       is, port_read(port, PORT_TFD));
			port_write(port, PORT_IS, PORT_IS_TFES);
			return -1;
		}
		if (!(port_read(port, PORT_CI) & 1u))
			return 0;
	}

	printf("ahci: command timed out  CI=0x%08X\n",
	       port_read(port, PORT_CI));
	return -1;
}

/* ================================================================
 * IDENTIFY DEVICE
 * ================================================================ */

static int
ahci_identify(int port)
{
	struct ata_fis_h2d fis;
	uint16_t *buf = (uint16_t *)data_uva;
	char model[41];
	uint32_t lba28;
	unsigned int i;

	memset(&fis, 0, sizeof(fis));
	fis.fis_type = FIS_TYPE_H2D;
	fis.flags    = FIS_H2D_FLAG_CMD;
	fis.command  = ATA_CMD_IDENTIFY;
	fis.device   = 0;

	if (ahci_submit_cmd(port, &fis, data_pa, 512, 0) < 0) {
		printf("ahci: IDENTIFY failed\n");
		return -1;
	}

	/* Model string: words 27-46, big-endian bytes within each word */
	for (i = 0; i < 20; i++) {
		uint16_t w = buf[27 + i];
		model[i * 2]     = (char)((w >> 8) & 0xFF);
		model[i * 2 + 1] = (char)(w & 0xFF);
	}
	model[40] = '\0';
	for (i = 39; i > 0 && model[i] == ' '; i--)
		model[i] = '\0';

	/* LBA28 total addressable sectors (words 60-61) */
	lba28 = ((uint32_t)buf[61] << 16) | buf[60];
	disk_sectors = lba28;

	printf("ahci: model    : %s\n", model);
	printf("ahci: sectors  : %u  capacity: ~%u MB\n",
	       lba28, lba28 / 2048);

	return 0;
}

/* ================================================================
 * READ DMA EXT (LBA48)
 * ================================================================ */

static int
ahci_read_sectors(int port, uint32_t lba, uint16_t count)
{
	struct ata_fis_h2d fis;

	memset(&fis, 0, sizeof(fis));
	fis.fis_type         = FIS_TYPE_H2D;
	fis.flags            = FIS_H2D_FLAG_CMD;
	fis.command          = ATA_CMD_READ_DMA_EXT;
	fis.device           = ATA_DEV_LBA;
	fis.lba_lo           = (lba >>  0) & 0xFF;
	fis.lba_mid          = (lba >>  8) & 0xFF;
	fis.lba_hi           = (lba >> 16) & 0xFF;
	fis.lba_lo_exp       = (lba >> 24) & 0xFF;
	fis.lba_mid_exp      = 0;
	fis.lba_hi_exp       = 0;
	fis.sector_count     = count & 0xFF;
	fis.sector_count_exp = (count >> 8) & 0xFF;

	return ahci_submit_cmd(port, &fis, data_pa, (unsigned int)count * 512, 0);
}

/* ================================================================
 * Sequential read benchmark
 * ================================================================ */

#define BENCH_SECTORS		256u	/* 128 KB total */
#define BENCH_SECTORS_PER_CMD	8u	/* 4 KB per command (= data buf size) */

static void
ahci_benchmark(int port)
{
	uint64_t t0, t1, elapsed;
	uint32_t lba, i;
	unsigned int cmds = BENCH_SECTORS / BENCH_SECTORS_PER_CMD;
	unsigned int bytes = BENCH_SECTORS * 512u;

	printf("ahci: benchmark: %u sectors (%u KB) x %u-sector cmds\n",
	       BENCH_SECTORS, BENCH_SECTORS / 2, BENCH_SECTORS_PER_CMD);

	/* Warm-up: one command before measuring */
	if (ahci_read_sectors(port, 0, BENCH_SECTORS_PER_CMD) < 0) {
		printf("ahci: benchmark warm-up failed\n");
		return;
	}

	t0 = rdtsc();
	for (i = 0, lba = 0; i < cmds; i++, lba += BENCH_SECTORS_PER_CMD) {
		if (ahci_read_sectors(port, lba, BENCH_SECTORS_PER_CMD) < 0) {
			printf("ahci: benchmark read error at LBA %u\n", lba);
			return;
		}
	}
	t1 = rdtsc();

	elapsed = t1 - t0;

	/*
	 * Use only the lower 32 bits for division — in QEMU, reading
	 * 256 sectors takes well under 2^32 TSC cycles (~1 GHz TSC).
	 */
	{
		unsigned int cyc = (unsigned int)(elapsed & 0xFFFFFFFFu);

		printf("ahci: %u sectors (%u KB) in %u TSC cycles\n",
		       BENCH_SECTORS, BENCH_SECTORS / 2, cyc);
		printf("ahci: cycles/sector : %u\n", cyc / BENCH_SECTORS);
		/* MB/s = bytes * 1000 / cycles  (1 GHz TSC assumed) */
		if (cyc > 0)
			printf("ahci: ~%u MB/s  (assumes 1 GHz TSC)\n",
			       bytes * 1000u / cyc);
	}
}

/* ================================================================
 * Full AHCI initialisation
 * ================================================================ */

static int
ahci_init(void)
{
	kern_return_t kr;
	unsigned int bar5, cmd_reg, irq_reg;

	/* Read and save BAR5 (ABAR physical address) */
	kr = device_pci_config_read(master_device,
		ahci_bus, ahci_slot, ahci_func, PCI_BAR5, &bar5);
	if (kr != KERN_SUCCESS) {
		printf("ahci: failed to read BAR5\n");
		return -1;
	}
	ahci_abar_phys = bar5 & ~0xFu;
	printf("ahci: ABAR phys = 0x%08X\n", ahci_abar_phys);

	/* Enable PCI bus master + memory space */
	kr = device_pci_config_read(master_device,
		ahci_bus, ahci_slot, ahci_func, PCI_COMMAND, &cmd_reg);
	if (kr == KERN_SUCCESS) {
		cmd_reg |= PCI_CMD_MEM_ENABLE | PCI_CMD_BUS_MASTER;
		device_pci_config_write(master_device,
			ahci_bus, ahci_slot, ahci_func, PCI_COMMAND, cmd_reg);
	}

	/* Read IRQ */
	kr = device_pci_config_read(master_device,
		ahci_bus, ahci_slot, ahci_func, PCI_INTERRUPT_LINE, &irq_reg);
	if (kr == KERN_SUCCESS) {
		ahci_irq = irq_reg & 0xFF;
		printf("ahci: IRQ = %u\n", ahci_irq);
	}

	/* Map ABAR into our address space */
	kr = device_mmio_map(master_device,
		ahci_abar_phys, AHCI_ABAR_SIZE,
		mach_task_self(), (unsigned int *)&abar);
	if (kr != KERN_SUCCESS) {
		printf("ahci: device_mmio_map failed (kr=%d)\n", kr);
		return -1;
	}
	printf("ahci: ABAR mapped at uva=0x%08X\n", (unsigned int)abar);

	/* Allocate DMA buffers and map them into our address space */
	kr = device_dma_alloc(master_device, 4096, &clb_kva, &clb_pa);
	if (kr != KERN_SUCCESS) { printf("ahci: CLB alloc failed\n"); return -1; }
	kr = device_dma_map_user(master_device, clb_kva, 4096,
				 mach_task_self(), &clb_uva);
	if (kr != KERN_SUCCESS) { printf("ahci: CLB map failed\n"); return -1; }

	kr = device_dma_alloc(master_device, 4096, &fb_kva, &fb_pa);
	if (kr != KERN_SUCCESS) { printf("ahci: FB alloc failed\n"); return -1; }
	kr = device_dma_map_user(master_device, fb_kva, 4096,
				 mach_task_self(), &fb_uva);
	if (kr != KERN_SUCCESS) { printf("ahci: FB map failed\n"); return -1; }

	kr = device_dma_alloc(master_device, 4096, &ct_kva, &ct_pa);
	if (kr != KERN_SUCCESS) { printf("ahci: CT alloc failed\n"); return -1; }
	kr = device_dma_map_user(master_device, ct_kva, 4096,
				 mach_task_self(), &ct_uva);
	if (kr != KERN_SUCCESS) { printf("ahci: CT map failed\n"); return -1; }

	kr = device_dma_alloc(master_device, 4096, &data_kva, &data_pa);
	if (kr != KERN_SUCCESS) { printf("ahci: data alloc failed\n"); return -1; }
	kr = device_dma_map_user(master_device, data_kva, 4096,
				 mach_task_self(), &data_uva);
	if (kr != KERN_SUCCESS) { printf("ahci: data map failed\n"); return -1; }

	printf("ahci: DMA: clb pa=0x%08X fb pa=0x%08X ct pa=0x%08X data pa=0x%08X\n",
	       clb_pa, fb_pa, ct_pa, data_pa);

	/* HBA reset + AHCI enable */
	if (ahci_hba_reset() < 0)
		return -1;

	/* Find first port with a device */
	active_port = ahci_port_find();
	if (active_port < 0)
		return -1;

	/* Initialise that port */
	if (ahci_port_init(active_port) < 0)
		return -1;

	/* Register IRQ notification */
	if (ahci_irq > 0 && ahci_irq < 16) {
		kr = device_intr_register(master_device, ahci_irq, irq_port,
					  MACH_MSG_TYPE_MAKE_SEND);
		if (kr == KERN_SUCCESS)
			printf("ahci: IRQ %u registered\n", ahci_irq);
	}

	return 0;
}

/* ================================================================
 * Device server — MIG server stubs for device.defs (subsystem 2800)
 *
 * The ahci_driver acts as a userspace device server: clients (e.g.
 * ext2_server) obtain our service port via netname_look_up and send
 * device_read / device_get_status RPCs.  The MIG-generated
 * device_server() demux calls these ds_* implementations.
 * ================================================================ */

#define DATA_BUF_SIZE	4096u	/* DMA data buffer size */
#define SECTOR_SIZE	512u

kern_return_t
ds_device_open(mach_port_t master, mach_port_t reply,
	       mach_msg_type_name_t reply_poly,
	       mach_port_t ledger, dev_mode_t mode,
	       security_token_t sec_token, dev_name_t name,
	       mach_port_t *device)
{
	if (active_port < 0)
		return D_NO_SUCH_DEVICE;
	*device = ahci_service_port;
	return KERN_SUCCESS;
}

kern_return_t
ds_device_close(mach_port_t device)
{
	return KERN_SUCCESS;
}

kern_return_t
ds_device_read(mach_port_t device, mach_port_t reply,
	       mach_msg_type_name_t reply_poly,
	       dev_mode_t mode, recnum_t recnum,
	       io_buf_len_t bytes_wanted,
	       io_buf_ptr_t *data, mach_msg_type_number_t *data_count)
{
	kern_return_t kr;
	vm_offset_t buf;
	unsigned int total, offset, lba, chunk, chunk_sects;

	if (active_port < 0)
		return D_NO_SUCH_DEVICE;
	if (bytes_wanted <= 0)
		return D_INVALID_SIZE;

	/* Round up to sector boundary */
	total = (unsigned int)bytes_wanted;
	if (total % SECTOR_SIZE)
		total = (total + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);

	kr = vm_allocate(mach_task_self(), &buf, total, TRUE);
	if (kr != KERN_SUCCESS)
		return D_NO_MEMORY;

	/* Read in DATA_BUF_SIZE chunks via the single DMA buffer */
	lba = recnum;
	for (offset = 0; offset < total; offset += chunk) {
		chunk = total - offset;
		if (chunk > DATA_BUF_SIZE)
			chunk = DATA_BUF_SIZE;
		chunk_sects = chunk / SECTOR_SIZE;

		if (ahci_read_sectors(active_port, lba, chunk_sects) < 0) {
			vm_deallocate(mach_task_self(), buf, total);
			return D_IO_ERROR;
		}
		memcpy((void *)(buf + offset), (void *)data_uva, chunk);
		lba += chunk_sects;
	}

	*data = (io_buf_ptr_t)buf;
	*data_count = (mach_msg_type_number_t)bytes_wanted;
	return KERN_SUCCESS;
}

kern_return_t
ds_device_read_inband(mach_port_t device, mach_port_t reply,
		      mach_msg_type_name_t reply_poly,
		      dev_mode_t mode, recnum_t recnum,
		      io_buf_len_t bytes_wanted,
		      io_buf_ptr_inband_t data,
		      mach_msg_type_number_t *data_count)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_device_write(mach_port_t device, mach_port_t reply,
		mach_msg_type_name_t reply_poly,
		dev_mode_t mode, recnum_t recnum,
		io_buf_ptr_t data, mach_msg_type_number_t data_count,
		io_buf_len_t *bytes_written)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_device_write_inband(mach_port_t device, mach_port_t reply,
		       mach_msg_type_name_t reply_poly,
		       dev_mode_t mode, recnum_t recnum,
		       io_buf_ptr_inband_t data,
		       mach_msg_type_number_t data_count,
		       io_buf_len_t *bytes_written)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_device_get_status(mach_port_t device, dev_flavor_t flavor,
		     dev_status_t status,
		     mach_msg_type_number_t *status_count)
{
	if (flavor == DEV_GET_SIZE) {
		status[DEV_GET_SIZE_DEVICE_SIZE] = (int)disk_sectors;
		status[DEV_GET_SIZE_RECORD_SIZE] = (int)SECTOR_SIZE;
		*status_count = DEV_GET_SIZE_COUNT;
		return KERN_SUCCESS;
	}
	return D_INVALID_OPERATION;
}

kern_return_t
ds_device_set_status(mach_port_t device, dev_flavor_t flavor,
		     dev_status_t status,
		     mach_msg_type_number_t status_count)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_device_map(mach_port_t device, vm_prot_t prot,
	      vm_offset_t offset, vm_size_t size,
	      mach_port_t *pager, boolean_t unmap)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_device_set_filter(mach_port_t device, mach_port_t receive_port,
		     int priority, filter_array_t filter,
		     mach_msg_type_number_t filter_count)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_io_done_queue_create(mach_port_t host, mach_port_t *queue)
{
	return D_INVALID_OPERATION;
}

kern_return_t
ds_io_done_queue_terminate(mach_port_t queue)
{
	return D_INVALID_OPERATION;
}

/* ================================================================
 * Combined demux: device RPC + IRQ notifications
 * ================================================================ */

static boolean_t
ahci_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
	/* Try MIG device server demux first */
	if (device_server(in, out))
		return TRUE;

	/* IRQ notification — acknowledge, no reply */
	if (in->msgh_id >= IRQ_NOTIFY_MSGH_BASE) {
		ahci_write(AHCI_IS, ~0u);
		port_write(active_port, PORT_IS, ~0u);
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

	printf("\n=== AHCI userspace driver (real I/O) ===\n");

	/* IRQ notification port */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &irq_port);
	if (kr != KERN_SUCCESS) {
		printf("ahci: irq port alloc failed\n");
		return 1;
	}
	kr = mach_port_insert_right(mach_task_self(), irq_port, irq_port,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("ahci: irq port right failed\n");
		return 1;
	}

	/* Service port for device RPC (device_read, etc.) */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &ahci_service_port);
	if (kr != KERN_SUCCESS) {
		printf("ahci: service port alloc failed\n");
		return 1;
	}
	kr = mach_port_insert_right(mach_task_self(),
		ahci_service_port, ahci_service_port,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("ahci: service port right failed\n");
		return 1;
	}

	/* Port set: receive on both IRQ and RPC ports */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_PORT_SET, &port_set);
	if (kr != KERN_SUCCESS) {
		printf("ahci: port set alloc failed\n");
		return 1;
	}
	mach_port_move_member(mach_task_self(), irq_port, port_set);
	mach_port_move_member(mach_task_self(), ahci_service_port, port_set);

	/* Find and initialise the controller */
	if (!pci_scan_ahci()) {
		printf("ahci: no controller, exiting\n");
		return 1;
	}
	if (ahci_init() < 0) {
		printf("ahci: init failed\n");
		return 1;
	}

	/* Identify the attached disk */
	ahci_identify(active_port);

	/* Run sequential read benchmark */
	ahci_benchmark(active_port);

	/* Register with the name server so other tasks can find us */
	kr = netname_check_in(name_server_port, "ahci_driver",
			      mach_task_self(), ahci_service_port);
	if (kr != KERN_SUCCESS)
		printf("ahci: netname_check_in failed (kr=%d) "
		       "— continuing without registration\n", kr);
	else
		printf("ahci: registered as 'ahci_driver' with name server\n");

	printf("ahci: init complete, entering message loop\n");

	/*
	 * Message loop: combined demux handles both device RPCs
	 * (device_read, device_get_status, etc.) and IRQ notifications.
	 */
	mach_msg_server(ahci_demux, 8192, port_set,
			MACH_MSG_OPTION_NONE);

	/* mach_msg_server never returns on success */
	printf("ahci: mach_msg_server exited unexpectedly\n");
	return 1;
}
