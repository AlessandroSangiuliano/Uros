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
 * device/device_master.c
 *
 * Kernel primitives for userspace drivers: PCI config access,
 * interrupt forwarding, and DMA buffer allocation.
 *
 * All RPCs require the master_device_port (privileged).
 */

#include <pci.h>

#include <mach/kern_return.h>
#include <mach/mach_types.h>
#include <mach/boolean.h>
#include <mach/vm_param.h>
#include <kern/misc_protos.h>
#include <kern/spl.h>
#include <kern/ipc_mig.h>
#include <kern/ipc_tt.h>
#include <kern/task.h>
#include <ipc/ipc_port.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/pmap.h>
#include <device/device_master.h>
#include <device/device_port.h>

#if NPCI > 0
#include <i386/pci/pci.h>
#include <i386/pci/pcibios.h>
#endif

#include <chips/busses.h>
#include <i386/ipl.h>
#include <i386/pio.h>

/* ================================================================
 * Interrupt forwarding
 * ================================================================ */

struct irq_forward irq_forward_table[IRQ_FORWARD_MAX];

/*
 * Saved original interrupt handlers, so we can restore on unregister.
 */
static intr_t	irq_orig_handler[IRQ_FORWARD_MAX];
static int	irq_orig_unit[IRQ_FORWARD_MAX];
static int	irq_orig_spl[IRQ_FORWARD_MAX];

/*
 * IRQ handler installed by device_intr_register().
 * Sends a simple notification message to the registered port.
 * Runs at interrupt level — mach_msg_send_from_kernel is safe
 * here because it uses ipc_mqueue_send_always (no blocking).
 */
static void
irq_forward_handler(int irq)
{
	struct irq_forward *fwd = &irq_forward_table[irq];
	mach_msg_header_t msg;

	if (!fwd->active || fwd->notify_port == IP_NULL)
		return;

	msg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	msg.msgh_size = sizeof(msg);
	msg.msgh_remote_port = fwd->notify_port;
	msg.msgh_local_port = MACH_PORT_NULL;
	msg.msgh_id = IRQ_NOTIFY_MSGH_BASE + irq;

	mach_msg_send_from_kernel(&msg, sizeof(msg));
}

void
device_master_init(void)
{
	int i;

	for (i = 0; i < IRQ_FORWARD_MAX; i++) {
		irq_forward_table[i].notify_port = IP_NULL;
		irq_forward_table[i].active = 0;
	}
}

/* ================================================================
 * MIG server routines (ds_master_ prefix from device_master.defs)
 * ================================================================ */

/*
 * Validate that the caller holds the master device port.
 */
static kern_return_t
check_master_port(ipc_port_t port)
{
	if (port == IP_NULL)
		return KERN_INVALID_ARGUMENT;
	if (port != master_device_port)
		return KERN_INVALID_ARGUMENT;
	return KERN_SUCCESS;
}

/* ---- PCI configuration space ---- */

kern_return_t
ds_master_device_pci_config_read(
	ipc_port_t		master_port,
	unsigned int		bus,
	unsigned int		slot,
	unsigned int		func,
	unsigned int		reg,
	unsigned int		*data)
{
#if NPCI > 0
	pcici_t tag;
	kern_return_t kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (bus > 255 || slot > 31 || func > 7 || (reg & 3))
		return KERN_INVALID_ARGUMENT;

	tag = pcitag((unsigned char)bus,
		     (unsigned char)slot,
		     (unsigned char)func);
	if (!tag.cfg1)
		return KERN_FAILURE;

	*data = pci_conf_read(tag, reg);
	return KERN_SUCCESS;
#else
	return KERN_FAILURE;
#endif
}

kern_return_t
ds_master_device_pci_config_write(
	ipc_port_t		master_port,
	unsigned int		bus,
	unsigned int		slot,
	unsigned int		func,
	unsigned int		reg,
	unsigned int		data)
{
#if NPCI > 0
	pcici_t tag;
	kern_return_t kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (bus > 255 || slot > 31 || func > 7 || (reg & 3))
		return KERN_INVALID_ARGUMENT;

	tag = pcitag((unsigned char)bus,
		     (unsigned char)slot,
		     (unsigned char)func);
	if (!tag.cfg1)
		return KERN_FAILURE;

	pci_conf_write(tag, reg, data);
	return KERN_SUCCESS;
#else
	return KERN_FAILURE;
#endif
}

/* ---- Interrupt forwarding ---- */

kern_return_t
ds_master_device_intr_register(
	ipc_port_t		master_port,
	unsigned int		irq,
	ipc_port_t		notify_port)
{
	kern_return_t kr;
	spl_t s;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (irq >= IRQ_FORWARD_MAX)
		return KERN_INVALID_ARGUMENT;

	if (notify_port == IP_NULL)
		return KERN_INVALID_ARGUMENT;

	if (irq_forward_table[irq].active)
		return KERN_RESOURCE_SHORTAGE;	/* already registered */

	/*
	 * Save the original handler and install our forwarder.
	 */
	s = splhigh();

	reset_irq((int)irq,
		  &irq_orig_unit[irq],
		  &irq_orig_spl[irq],
		  &irq_orig_handler[irq]);

	irq_forward_table[irq].notify_port = notify_port;
	irq_forward_table[irq].active = 1;

	take_irq((int)irq, (int)irq, SPL6, (intr_t)irq_forward_handler);

	splx(s);

	return KERN_SUCCESS;
}

kern_return_t
ds_master_device_intr_unregister(
	ipc_port_t		master_port,
	unsigned int		irq)
{
	kern_return_t kr;
	spl_t s;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (irq >= IRQ_FORWARD_MAX)
		return KERN_INVALID_ARGUMENT;

	if (!irq_forward_table[irq].active)
		return KERN_INVALID_ARGUMENT;

	s = splhigh();

	/*
	 * Restore the original handler.
	 */
	take_irq((int)irq,
		 irq_orig_unit[irq],
		 irq_orig_spl[irq],
		 irq_orig_handler[irq]);

	irq_forward_table[irq].notify_port = IP_NULL;
	irq_forward_table[irq].active = 0;

	splx(s);

	return KERN_SUCCESS;
}

/* ---- DMA buffer allocation ---- */

kern_return_t
ds_master_device_dma_alloc(
	ipc_port_t		master_port,
	unsigned int		size,
	unsigned int		*vaddr_out,
	unsigned int		*paddr_out)
{
	kern_return_t kr;
	vm_offset_t kva;
	vm_offset_t pa;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (size == 0)
		return KERN_INVALID_ARGUMENT;

	size = round_page(size);

	/*
	 * Allocate wired, physically contiguous memory.
	 * kmem_alloc_contig guarantees contiguity for DMA.
	 */
	kr = kmem_alloc_contig(kernel_map, &kva, size,
			       PAGE_MASK, 0);
	if (kr != KERN_SUCCESS)
		return kr;

	/*
	 * Extract the physical address.
	 */
	pa = pmap_extract(kernel_pmap, kva);
	if (pa == 0) {
		kmem_free(kernel_map, kva, size);
		return KERN_FAILURE;
	}

	*vaddr_out = (unsigned int)kva;
	*paddr_out = (unsigned int)pa;
	return KERN_SUCCESS;
}

kern_return_t
ds_master_device_dma_free(
	ipc_port_t		master_port,
	unsigned int		vaddr,
	unsigned int		size)
{
	kern_return_t kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (size == 0)
		return KERN_INVALID_ARGUMENT;

	size = round_page(size);

	kmem_free(kernel_map, (vm_offset_t)vaddr, size);
	return KERN_SUCCESS;
}

/* ---- User-space DMA and MMIO mapping ---- */

/*
 * Helper: reserve a VA range in a task's map and wire in physical pages.
 */
static kern_return_t
map_phys_into_task(task_t		task,
		   vm_offset_t		phys_base,	/* page-aligned */
		   vm_size_t		size,		/* page-aligned */
		   vm_offset_t		*uva_out)
{
	vm_map_t	map = task->map;
	vm_offset_t	uva = 0;
	kern_return_t	kr;
	vm_offset_t	i;

	kr = vm_map_enter(map, &uva, size, 0, TRUE,
			  VM_OBJECT_NULL, (vm_offset_t)0, FALSE,
			  VM_PROT_READ | VM_PROT_WRITE,
			  VM_PROT_READ | VM_PROT_WRITE,
			  VM_INHERIT_NONE);
	if (kr != KERN_SUCCESS)
		return kr;

	for (i = 0; i < size; i += PAGE_SIZE)
		pmap_enter(map->pmap, uva + i, phys_base + i,
			   VM_PROT_READ | VM_PROT_WRITE, TRUE);

	*uva_out = uva;
	return KERN_SUCCESS;
}

/*
 * device_dma_map_user — map a kernel DMA buffer into a user task.
 * kva: kernel VA returned by device_dma_alloc (physically contiguous).
 * Returns user VA accessible by the driver process.
 */
kern_return_t
ds_master_device_dma_map_user(
	ipc_port_t		master_port,
	unsigned int		kva,
	unsigned int		size,
	ipc_port_t		task_port,
	unsigned int		*uva_out)
{
	task_t		task;
	vm_offset_t	uva;
	vm_offset_t	pa;
	kern_return_t	kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (kva == 0 || size == 0)
		return KERN_INVALID_ARGUMENT;

	task = convert_port_to_task(task_port);
	if (task == TASK_NULL)
		return KERN_INVALID_ARGUMENT;

	pa = pmap_extract(kernel_pmap, (vm_offset_t)kva);
	if (pa == 0) {
		task_deallocate(task);
		return KERN_FAILURE;
	}

	kr = map_phys_into_task(task, pa, round_page(size), &uva);
	task_deallocate(task);
	if (kr != KERN_SUCCESS)
		return kr;

	*uva_out = (unsigned int)uva;
	return KERN_SUCCESS;
}

/*
 * device_mmio_map — map a physical MMIO range (PCI BAR) into a user task.
 * phys_addr need not be page-aligned; the returned uva has the same
 * page offset as phys_addr.
 */
kern_return_t
ds_master_device_mmio_map(
	ipc_port_t		master_port,
	unsigned int		phys_addr,
	unsigned int		size,
	ipc_port_t		task_port,
	unsigned int		*uva_out)
{
	task_t		task;
	vm_offset_t	phys_base;
	vm_offset_t	page_offset;
	vm_offset_t	uva;
	vm_size_t	round_sz;
	kern_return_t	kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (phys_addr == 0 || size == 0)
		return KERN_INVALID_ARGUMENT;

	task = convert_port_to_task(task_port);
	if (task == TASK_NULL)
		return KERN_INVALID_ARGUMENT;

	phys_base   = trunc_page((vm_offset_t)phys_addr);
	page_offset = (vm_offset_t)phys_addr - phys_base;
	round_sz    = round_page(page_offset + size);

	kr = map_phys_into_task(task, phys_base, round_sz, &uva);
	task_deallocate(task);
	if (kr != KERN_SUCCESS)
		return kr;

	*uva_out = (unsigned int)(uva + page_offset);
	return KERN_SUCCESS;
}

/*
 * device_mmio_unmap — remove an MMIO mapping from a user task.
 */
kern_return_t
ds_master_device_mmio_unmap(
	ipc_port_t		master_port,
	unsigned int		uva,
	unsigned int		size,
	ipc_port_t		task_port)
{
	task_t		task;
	kern_return_t	kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	task = convert_port_to_task(task_port);
	if (task == TASK_NULL)
		return KERN_INVALID_ARGUMENT;

	kr = vm_map_remove(task->map,
			   trunc_page((vm_offset_t)uva),
			   round_page((vm_offset_t)uva + size),
			   FALSE);
	task_deallocate(task);
	return kr;
}

/* ---- I/O port access ---- */

kern_return_t
ds_master_device_io_port_read(
	ipc_port_t		master_port,
	unsigned int		port,
	unsigned int		size,
	unsigned int		*data_out)
{
	kern_return_t kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	switch (size) {
	case 1:
		*data_out = inb((i386_ioport_t)port);
		break;
	case 2:
		*data_out = inw((i386_ioport_t)port);
		break;
	case 4:
		*data_out = inl((i386_ioport_t)port);
		break;
	default:
		return KERN_INVALID_ARGUMENT;
	}
	return KERN_SUCCESS;
}

kern_return_t
ds_master_device_io_port_write(
	ipc_port_t		master_port,
	unsigned int		port,
	unsigned int		size,
	unsigned int		data)
{
	kern_return_t kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	switch (size) {
	case 1:
		outb((i386_ioport_t)port, (unsigned char)data);
		break;
	case 2:
		outw((i386_ioport_t)port, (unsigned short)data);
		break;
	case 4:
		outl((i386_ioport_t)port, (unsigned long)data);
		break;
	default:
		return KERN_INVALID_ARGUMENT;
	}
	return KERN_SUCCESS;
}
