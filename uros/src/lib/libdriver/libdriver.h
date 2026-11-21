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
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * libdriver -- what a module of chora is written against (#963, #773).
 *
 * chora is one program for every device: it is started on one device, claims
 * it, loads the module that drives it and hands the module a `struct driver'.
 * The module does the rest through these calls -- the device's configuration
 * space, its regions mapped, its interrupt delivered to a handler on a thread
 * of its own, its DMA buffers (#978) -- and says what it does with
 * driver_log(), under the task's name, which is the device's.
 *
 * The module is a shared object loaded into chora, and resolves these names
 * against chora's own symbols, so it links against nothing.
 */

#ifndef _LIBDRIVER_H_
#define _LIBDRIVER_H_

#include <mach.h>
#include <stdint.h>
#include <mach/task_name.h>	/* URMACH_TASK_NAME_MAX */
#include <mach/cap_types.h>	/* struct uros_cap */

/*
 * The device a chora drives.  Filled by driver_open(), read by the module;
 * the interrupt half is driver_interrupt()'s.
 */
struct driver {
	char		name[URMACH_TASK_NAME_MAX];	/* module@bb:dd.f */
	unsigned int	bus, slot, func;
	natural_t	bdf;			/* bus << 8 | slot << 3 | func */
	uint32_t	vendor_device;		/* PCI 0x00, as the HAL read it */
	uint32_t	class_rev;		/* PCI 0x08, as the HAL read it */
	mach_port_t	master_device;
	struct uros_cap	token;			/* the capability the claim rests on */

	uint64_t	dma_reach;		/* driver_dma_reach()'s; all ones */

	mach_port_t	irq_port;		/* MACH_PORT_NULL: none asked for */
	natural_t	irq_slot;		/* what device_intr_enable() takes */
	int		irq_message;		/* 1: a message; 0: the INTx line */
	void	      (*irq_handler)(struct driver *, void *);
	void	       *irq_arg;
};

/*
 * chora's half: open `bdf' for `module' -- name this task module@bb:dd.f,
 * take the master device port from bootstrap, ask the HAL what the device is,
 * ask this task's capability port for it, and claim it.  0 when the device is
 * claimed, and the claim said; -1 with a line saying which step refused.
 */
int		driver_open(struct driver *d, const char *module, natural_t bdf);

/* A line under the task's name: "module@bb:dd.f: ...". */
void		driver_log(const struct driver *d, const char *fmt, ...)
			__attribute__((format(printf, 2, 3)));

/* The claimed device's configuration space, a 32-bit register at `reg'. */
kern_return_t	driver_config_read(const struct driver *d, unsigned int reg,
				   uint32_t *value);
kern_return_t	driver_config_write(const struct driver *d, unsigned int reg,
				    uint32_t value);

/*
 * Map the device's region `index' -- its position among the regions the
 * device decodes, not a BAR slot -- into this task.  The kernel says where
 * and how wide; an I/O region is refused, since it is not memory.  *va is the
 * region's first byte.  driver_unmap_region() takes back what this gave.
 */
kern_return_t	driver_map_region(const struct driver *d, unsigned int index,
				  volatile void **va, vm_size_t *size);
kern_return_t	driver_unmap_region(const struct driver *d, volatile void *va,
				    vm_size_t size);

/*
 * Deliver the device's interrupt to `handler', on a thread of its own: a
 * message where the device has one -- entry 0 of its MSI-X table, or its plain
 * MSI where it has no table (#977) -- and its INTx line otherwise.
 * After each call of the handler the slot is unmasked, so the handler must
 * have quieted the device by then -- a level-triggered line still asserted
 * would ring again at once.  d->irq_message says whether it got a message,
 * and a module that routes the device's causes to an entry needs to know.
 * Which kind of message is the kernel's to choose (device_master.defs).
 */
kern_return_t	driver_interrupt(struct driver *d,
				 void (*handler)(struct driver *, void *),
				 void *arg);

/*
 * A DMA buffer of the device's (#978): physically contiguous, mapped in this
 * task, and the address the DEVICE must be given, which is not that mapping.
 * Where the machine confines the device it is an IOVA, a number that means
 * something only in the device's own domain; elsewhere the physical address.
 * The module needs to know neither, only to give the device this one
 * (device_dma_alloc's contract, device_master.defs).
 *
 * ⚠️ The device reaches it only as a bus master, bit 2 of its command
 * register: the kernel sets it where it arms the device's MSI or MSI-X
 * (pci_msi.c, pci_msix.c), and a module on an INTx line sets it itself, with
 * driver_config_write().
 */
struct driver_dma {
	volatile void	*va;		/* this task's mapping */
	vm_size_t	size;
	uint64_t	device_addr;	/* what the device is programmed with */
	vm_address_t	kva;		/* the kernel's, which device_dma_free takes */
};

/*
 * How far the device's DMA reaches, the highest address it can use, inclusive
 * (#417): its DMA mask, as Linux's dma_set_mask() takes it.  The kernel places
 * every address it gives the device from then on below it -- the buffers of
 * driver_dma_alloc(), and the pages other servers lend the device -- an IOVA
 * placed below it where the machine translates, memory taken below it where
 * it does not.  A device whose DMA has fewer bits than the machine's says so
 * before its first buffer: AHCI without S64A, QEMU's edu at its default 28.
 * Until then it reaches everything.
 */
kern_return_t	driver_dma_reach(struct driver *d, uint64_t reach);

/*
 * A buffer of `size' bytes for the device, zeroed, within its reach.  One the
 * kernel placed beyond the reach declared is given back and refused, said:
 * that is the kernel's promise broken, and a device would cut the address.
 */
kern_return_t	driver_dma_alloc(struct driver *d, vm_size_t size,
				 struct driver_dma *out);

/*
 * Give it back: this task's mapping, then the buffer.  The device must have
 * stopped using it: what it writes after this lands in memory the kernel may
 * have given to someone else, or in nothing.  A refusal of the kernel's is
 * said, since the buffer then stays allocated.
 */
void		driver_dma_free(struct driver *d, struct driver_dma *dma);

/*
 * What the kernel knows of the device's refused DMA (#599): a reading, which
 * means something only against an earlier one.  Where the machine confines
 * the device, an address it was never granted is refused and counted; where
 * nothing confines it, nothing is refused, and no reading can say a transfer
 * was not.
 */
struct driver_dma_faults {
	int		confined;	/* 0: nothing confines the device */
	natural_t	count;		/* refusals since boot, modulo 2^32 */
	uint64_t	address;	/* the last refused */
	natural_t	lost;		/* times refusals may have gone uncounted */
};

kern_return_t	driver_dma_faults(const struct driver *d,
				  struct driver_dma_faults *now);

/*
 * Between two readings, device_master.defs' verdict: REFUSED where `count'
 * moved, UNKNOWN where only `lost' did, NOT_REFUSED where neither, and
 * NOT_CONFINED where the later reading says nothing confines the device.
 */
enum driver_dma_verdict {
	DRIVER_DMA_NOT_CONFINED,
	DRIVER_DMA_NOT_REFUSED,
	DRIVER_DMA_REFUSED,
	DRIVER_DMA_UNKNOWN
};

enum driver_dma_verdict	driver_dma_verdict(
				const struct driver_dma_faults *before,
				const struct driver_dma_faults *after);

#endif /* _LIBDRIVER_H_ */
