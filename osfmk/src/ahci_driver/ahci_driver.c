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
 * First userspace driver: uses device_master RPC primitives
 * (PCI config, interrupt forwarding, DMA allocation) to drive
 * an AHCI controller entirely from user space.
 *
 * Boot flow:
 *   bootstrap loads this server -> main() -> pci_scan() -> ahci_init()
 *   -> identify first SATA disk -> print model/capacity -> message loop
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/bootstrap.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <sa_mach.h>
#include <device/device.h>
#include <stdio.h>
#include "device_master.h"

#include "ahci.h"

/* ================================================================
 * Globals
 * ================================================================ */

static mach_port_t	host_port;
static mach_port_t	device_port;
static mach_port_t	master_device;	/* master_device_port from bootstrap */
static mach_port_t	security_port;
static mach_port_t	root_ledger_wired;
static mach_port_t	root_ledger_paged;
static mach_port_t	irq_port;	/* notification port for AHCI IRQ */

/* PCI location of the AHCI controller */
static unsigned int	ahci_bus, ahci_slot, ahci_func;
static unsigned int	ahci_irq;
static unsigned int	ahci_abar_phys;	/* BAR5 physical address */

/* DMA buffers */
static unsigned int	clb_va, clb_pa;		/* Command List Base (1 KB) */
static unsigned int	fb_va, fb_pa;		/* FIS Base (256 bytes) */
static unsigned int	ct_va, ct_pa;		/* Command Table (256 + PRDT) */
static unsigned int	data_va, data_pa;	/* Data buffer (4 KB) */

/* Number of ports implemented */
static int		num_ports;
static int		active_port = -1;	/* first port with a device */

/*
 * Note on MMIO access:
 * For now we use PCI config space to detect the controller and
 * allocate DMA buffers.  Actual MMIO register access requires
 * mapping the ABAR into our address space — needs a future
 * device_master_mmio_map() primitive.
 */

/* ================================================================
 * PCI Scanning — find an AHCI controller
 * ================================================================ */

static int
pci_scan_ahci(void)
{
	unsigned int bus, slot, func;
	unsigned int vendor_device;
	unsigned int class_rev;
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

				/* No device */
				if (vendor_device == 0xFFFFFFFF ||
				    vendor_device == 0)
					continue;

				/* Check class code */
				kr = device_pci_config_read(master_device,
					bus, slot, func, PCI_CLASS_REV,
					&class_rev);
				if (kr != KERN_SUCCESS)
					continue;

				unsigned int class = (class_rev >> 24) & 0xFF;
				unsigned int subclass = (class_rev >> 16) & 0xFF;
				unsigned int progif = (class_rev >> 8) & 0xFF;

				if (class == PCI_CLASS_STORAGE &&
				    subclass == PCI_SUBCLASS_SATA &&
				    progif == PCI_PROGIF_AHCI) {
					ahci_bus = bus;
					ahci_slot = slot;
					ahci_func = func;

					printf("ahci: found controller at PCI 0x%X\n",
					       (bus << 16) | (slot << 8) | func);
					printf("ahci: vendor:device = 0x%08X\n", vendor_device);
					return 1;
				}
			}
		}
	}

	printf("ahci: no AHCI controller found\n");
	return 0;
}

/* ================================================================
 * AHCI Controller Initialization
 * ================================================================ */

static volatile uint32_t *abar;	/* mapped MMIO base */

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

static int
ahci_init(void)
{
	kern_return_t kr;
	unsigned int bar5, irq_reg, cap, pi, vs;
	int port;

	/* Read BAR5 (ABAR) */
	kr = device_pci_config_read(master_device,
		ahci_bus, ahci_slot, ahci_func, PCI_BAR5, &bar5);
	if (kr != KERN_SUCCESS) {
		printf("ahci: failed to read BAR5\n");
		return -1;
	}

	ahci_abar_phys = bar5 & ~0xF;	/* mask type bits */
	printf("ahci: ABAR = 0x%08X\n", ahci_abar_phys);

	/* Enable PCI bus master + memory access */
	unsigned int cmd;
	kr = device_pci_config_read(master_device,
		ahci_bus, ahci_slot, ahci_func, PCI_COMMAND, &cmd);
	if (kr == KERN_SUCCESS) {
		cmd |= PCI_CMD_MEM_ENABLE | PCI_CMD_BUS_MASTER;
		device_pci_config_write(master_device,
			ahci_bus, ahci_slot, ahci_func, PCI_COMMAND, cmd);
	}

	/* Read IRQ line */
	kr = device_pci_config_read(master_device,
		ahci_bus, ahci_slot, ahci_func, PCI_INTERRUPT_LINE, &irq_reg);
	if (kr == KERN_SUCCESS) {
		ahci_irq = irq_reg & 0xFF;
		printf("ahci: IRQ = %u\n", ahci_irq);
	}

	/*
	 * Map ABAR into our address space.
	 * For the stub kernel approach, we use device_dma_alloc as a
	 * placeholder. Real MMIO mapping needs device_map() on the
	 * physical BAR address — this requires extending the kernel
	 * to support mapping arbitrary physical pages.
	 *
	 * TODO: Add device_master_mmio_map(phys, size) -> va primitive.
	 * For now, on i386 with flat memory model the kernel can access
	 * MMIO at the physical address directly via phystokv().
	 * We print the ABAR and demonstrate PCI enumeration + DMA alloc.
	 */
	printf("ahci: MMIO mapping not yet implemented (needs device_master_mmio_map)\n");
	printf("ahci: controller detected, PCI config accessible\n");

	/* Read AHCI version and capabilities via PCI config space
	 * (AHCI spec allows reading VS at ABAR+0x10, but we can't
	 *  MMIO yet — demonstrate the PCI path works) */

	/* Allocate DMA buffers for when MMIO is available */
	kr = device_dma_alloc(master_device, 4096, &clb_va, &clb_pa);
	if (kr != KERN_SUCCESS) {
		printf("ahci: failed to allocate CLB DMA buffer\n");
		return -1;
	}
	printf("ahci: CLB DMA va=0x%08X pa=0x%08X\n", clb_va, clb_pa);

	kr = device_dma_alloc(master_device, 4096, &fb_va, &fb_pa);
	if (kr != KERN_SUCCESS) {
		printf("ahci: failed to allocate FB DMA buffer\n");
		return -1;
	}
	printf("ahci: FB  DMA va=0x%08X pa=0x%08X\n", fb_va, fb_pa);

	kr = device_dma_alloc(master_device, 4096, &ct_va, &ct_pa);
	if (kr != KERN_SUCCESS) {
		printf("ahci: failed to allocate CT DMA buffer\n");
		return -1;
	}

	kr = device_dma_alloc(master_device, 4096, &data_va, &data_pa);
	if (kr != KERN_SUCCESS) {
		printf("ahci: failed to allocate data DMA buffer\n");
		return -1;
	}

	printf("ahci: DMA buffers allocated successfully\n");

	/* Register IRQ */
	if (ahci_irq > 0 && ahci_irq < 16) {
		kr = device_intr_register(master_device, ahci_irq, irq_port,
			MACH_MSG_TYPE_COPY_SEND);
		if (kr == KERN_SUCCESS) {
			printf("ahci: IRQ registered: %u\n", ahci_irq);
		} else {
			printf("ahci: IRQ registration failed (non-fatal)\n");
		}
	}

	printf("ahci: initialization complete\n");
	printf("ahci: waiting for MMIO mapping support to proceed\n");

	return 0;
}

/* ================================================================
 * Main entry point
 * ================================================================ */

int
main(int argc, char **argv)
{
	kern_return_t kr;

	/*
	 * Get privileged ports from the bootstrap server.
	 * bootstrap_ports() returns the host, device, ledger and
	 * security ports.  The device_port is the master_device_port
	 * needed for device_master RPCs.
	 */
	kr = bootstrap_ports(bootstrap_port,
			     &host_port,
			     &device_port,
			     &root_ledger_wired,
			     &root_ledger_paged,
			     &security_port);
	if (kr != KERN_SUCCESS)
		_exit(1);

	/* Initialize console I/O (device_open("console")) */
	printf_init(device_port);
	panic_init(host_port);

	master_device = device_port;

	printf("\n=== AHCI userspace driver starting ===\n");

	/* Allocate IRQ notification port */
	kr = mach_port_allocate(mach_task_self(),
		MACH_PORT_RIGHT_RECEIVE, &irq_port);
	if (kr != KERN_SUCCESS) {
		printf("ahci: failed to allocate IRQ port\n");
		return 1;
	}
	kr = mach_port_insert_right(mach_task_self(), irq_port, irq_port,
		MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		printf("ahci: failed to insert send right for IRQ port\n");
		return 1;
	}

	/* Scan PCI for AHCI controller */
	if (!pci_scan_ahci()) {
		printf("ahci: no controller, exiting\n");
		return 1;
	}

	/* Initialize the controller */
	if (ahci_init() < 0) {
		printf("ahci: initialization failed\n");
		return 1;
	}

	/*
	 * In the future, this is where the message loop goes:
	 * receive device RPCs and IRQ notifications, dispatch I/O.
	 *
	 * For now, the driver just demonstrates:
	 * 1. PCI bus scan via device_pci_config_read
	 * 2. DMA buffer allocation via device_dma_alloc
	 * 3. IRQ registration via device_intr_register
	 */
	printf("ahci: driver idle (no message loop yet)\n");

	/* Suspend ourselves — we have nothing more to do without MMIO */
	task_suspend(mach_task_self());
	return 0;
}
