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
 * libdriver.c -- the calls a module of chora makes (#963).  See libdriver.h.
 */

#include "libdriver.h"

#include <mach.h>
#include <mach/bootstrap.h>		/* bootstrap_ports() */
#include <mach/mach_interface.h>	/* urmach_task_set_name() */
#include <mach/message.h>
#include <mach/thread_switch.h>
#include <mach_init.h>			/* name_server_port */
#include <servers/netname.h>
#include <libcap.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "device_master.h"		/* the user half of device_master.defs */
#include "hal.h"			/* the user half of hal.defs */

/* How long driver_open() waits for the HAL's name: 200 depressed yields. */
#define DRIVER_HAL_TRIES	200

#define DRIVER_PAGE		4096u

/* ------------------------------------------------------------------ */
/*  Lines under the task's name                                         */
/* ------------------------------------------------------------------ */

/*
 * One printf() for the whole line, so that two threads of a driver -- the
 * module's and its interrupt thread -- cannot put their halves in each
 * other's lines.
 */
void
driver_log(const struct driver *d, const char *fmt, ...)
{
	char	line[256];
	int	n;
	va_list	ap;

	n = snprintf(line, sizeof line, "%s: ", d->name[0] ? d->name : "chora");
	if (n < 0 || (size_t)n >= sizeof line)
		n = 0;
	va_start(ap, fmt);
	(void)vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
	va_end(ap);
	printf("%s\n", line);
}

/* ------------------------------------------------------------------ */
/*  Opening the device: name, master port, the HAL, the capability, the */
/*  claim                                                               */
/* ------------------------------------------------------------------ */

/*
 * The task's name is the device's, module@bb:dd.f, set before anything that
 * could fail says so -- a refusal below is a line under the name it refuses.
 * A name that does not fit is refused by the kernel, not cut (#963's first
 * step), and so it is here, before asking.
 */
static int
name_the_task(struct driver *d, const char *module)
{
	int		n;
	kern_return_t	kr;

	n = snprintf(d->name, sizeof d->name, "%s@%02x:%02x.%x", module,
		     d->bus, d->slot, d->func);
	if (n < 0 || (size_t)n >= sizeof d->name) {
		d->name[0] = '\0';
		printf("chora: %s@%02x:%02x.%x is longer than a task's name, "
		       "%u characters\n", module, d->bus, d->slot, d->func,
		       (unsigned)(URMACH_TASK_NAME_MAX - 1));
		return -1;
	}
	kr = urmach_task_set_name(mach_task_self(), d->name,
				  (mach_msg_type_number_t)n);
	if (kr != KERN_SUCCESS) {
		driver_log(d, "the kernel would not name this task: 0x%x", kr);
		return -1;
	}
	return 0;
}

/*
 * The master device port, from bootstrap: what the line that started this
 * task's parent was given (-D), which a task created by it inherits with its
 * bootstrap port.  What it opens is the claim's to decide: configuration
 * space, regions and interrupts are refused to a task that holds no claim.
 */
static int
take_master_device(struct driver *d)
{
	mach_port_t	host = MACH_PORT_NULL, root_wired = MACH_PORT_NULL;
	mach_port_t	root_paged = MACH_PORT_NULL, security = MACH_PORT_NULL;
	kern_return_t	kr;

	kr = bootstrap_ports(bootstrap_port, &host, &d->master_device,
			     &root_wired, &root_paged, &security);
	if (MACH_PORT_VALID(host))
		(void)mach_port_deallocate(mach_task_self(), host);
	if (MACH_PORT_VALID(root_wired))
		(void)mach_port_deallocate(mach_task_self(), root_wired);
	if (MACH_PORT_VALID(root_paged))
		(void)mach_port_deallocate(mach_task_self(), root_paged);
	if (MACH_PORT_VALID(security))
		(void)mach_port_deallocate(mach_task_self(), security);
	if (kr != KERN_SUCCESS || !MACH_PORT_VALID(d->master_device)) {
		driver_log(d, "no master device port (bootstrap_ports 0x%x): "
			   "whoever started this was not given one", kr);
		return -1;
	}
	return 0;
}

/*
 * What the device is, from the HAL, which enumerates for everybody: reading
 * configuration space is itself something only a device's owner may do, so
 * a driver cannot find out what to claim by reading it (#511).
 */
static int
ask_the_hal(struct driver *d)
{
	mach_port_t	hal = MACH_PORT_NULL;
	unsigned int	vendor_device = 0, class_rev = 0, irq = 0;
	kern_return_t	kr;
	int		tries;

	for (tries = 0; tries < DRIVER_HAL_TRIES; tries++) {
		if (netname_look_up(name_server_port, "", (char *)"hal",
				    &hal) == KERN_SUCCESS)
			break;
		(void)thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 1);
	}
	if (!MACH_PORT_VALID(hal)) {
		driver_log(d, "the HAL never appeared");
		return -1;
	}
	kr = hal_get_device_info(hal, d->bus, d->slot, d->func,
				 &vendor_device, &class_rev, &irq);
	(void)mach_port_deallocate(mach_task_self(), hal);
	if (kr != KERN_SUCCESS) {
		driver_log(d, "the HAL knows no device at %02x:%02x.%x (0x%x)",
			   d->bus, d->slot, d->func, kr);
		return -1;
	}
	d->vendor_device = vendor_device;
	d->class_rev = class_rev;
	return 0;
}

int
driver_open(struct driver *d, const char *module, natural_t bdf)
{
	kern_return_t	kr;

	memset(d, 0, sizeof *d);
	d->bdf  = bdf;
	d->bus  = (unsigned int)(bdf >> 8) & 0xFF;
	d->slot = (unsigned int)(bdf >> 3) & 0x1F;
	d->func = (unsigned int)bdf & 0x7;
	d->irq_port = MACH_PORT_NULL;

	if (bdf > 0xFFFF) {
		printf("chora: %s: 0x%x is not a bus, slot and function\n",
		       module, (unsigned)bdf);
		return -1;
	}
	if (name_the_task(d, module) != 0 || take_master_device(d) != 0
	    || ask_the_hal(d) != 0)
		return -1;

	/*
	 * The capability, from this task's own port: the one exec_server
	 * derived for it from what its parent may hand on (#743).  chora asks
	 * for any PCI class, and holds only the class it was handed.
	 */
	kr = cap_request(RESOURCE_PCI_DEVICE, (uint64_t)(d->class_rev >> 8),
			 CAP_OP_PCI_DMA_MAP | CAP_OP_PCI_MMIO_MAP
			 | CAP_OP_PCI_IRQ, 0, &d->token);
	if (kr != KERN_SUCCESS) {
		driver_log(d, "no capability for class 0x%06x: cap_request "
			   "0x%x", (unsigned)(d->class_rev >> 8), kr);
		return -1;
	}

	kr = device_claim(d->master_device, d->bdf, (char *)&d->token,
			  sizeof d->token);
	if (kr != KERN_SUCCESS) {
		driver_log(d, "the claim on %02x:%02x.%x was refused: 0x%x",
			   d->bus, d->slot, d->func, kr);
		return -1;
	}
	driver_log(d, "claimed %02x:%02x.%x, %04x:%04x of class 0x%06x",
		   d->bus, d->slot, d->func, d->vendor_device & 0xFFFF,
		   d->vendor_device >> 16, (unsigned)(d->class_rev >> 8));
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Configuration space and regions                                     */
/* ------------------------------------------------------------------ */

kern_return_t
driver_config_read(const struct driver *d, unsigned int reg, uint32_t *value)
{
	unsigned int	v = 0;
	kern_return_t	kr;

	kr = device_pci_config_read(d->master_device, d->bus, d->slot,
				    d->func, reg, &v);
	if (kr == KERN_SUCCESS)
		*value = v;
	return kr;
}

kern_return_t
driver_config_write(const struct driver *d, unsigned int reg, uint32_t value)
{
	return device_pci_config_write(d->master_device, d->bus, d->slot,
				       d->func, reg, value);
}

kern_return_t
driver_map_region(const struct driver *d, unsigned int index,
		  volatile void **va, vm_size_t *size)
{
	vm_address_t	uva = 0;
	vm_size_t	rsize = 0;
	kern_return_t	kr;

	kr = device_region_map(d->master_device, d->bdf, index,
			       mach_task_self(), &uva, &rsize);
	if (kr != KERN_SUCCESS)
		return kr;
	*va = (volatile void *)uva;
	*size = rsize;
	return KERN_SUCCESS;
}

/*
 * The kernel answered the region's first byte and its exact size; what it
 * mapped is the whole pages around them, and that is what goes back.
 */
kern_return_t
driver_unmap_region(const struct driver *d, volatile void *va, vm_size_t size)
{
	vm_address_t	start = (vm_address_t)va & ~(vm_address_t)(DRIVER_PAGE - 1);
	vm_address_t	end = ((vm_address_t)va + size + DRIVER_PAGE - 1)
			      & ~(vm_address_t)(DRIVER_PAGE - 1);

	return device_mmio_unmap(d->master_device, start, end - start,
				 mach_task_self());
}

/* ------------------------------------------------------------------ */
/*  The interrupt, on a thread of its own                               */
/* ------------------------------------------------------------------ */

/*
 * One message per interrupt, a bare header (device_master.c), and after the
 * handler the slot unmasked: a no-op for a message-signalled slot, the line
 * given back for a level-triggered one, whose device the handler has quieted.
 */
static void *
interrupt_thread(void *arg)
{
	struct driver	*d = arg;
	struct {
		mach_msg_header_t	h;
		mach_msg_trailer_t	t;
	}		msg;
	kern_return_t	kr;

	for (;;) {
		kr = mach_msg(&msg.h, MACH_RCV_MSG, 0, sizeof msg, d->irq_port,
			      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		if (kr != MACH_MSG_SUCCESS) {
			driver_log(d, "the interrupt port answered 0x%x, and "
				   "its thread stops", kr);
			return NULL;
		}
		d->irq_handler(d, d->irq_arg);
		(void)device_intr_enable(d->master_device, d->irq_slot);
	}
}

kern_return_t
driver_interrupt(struct driver *d, void (*handler)(struct driver *, void *),
		 void *arg)
{
	pthread_t	th;
	uint32_t	cfg = 0;
	natural_t	slot = 0;
	kern_return_t	kr, msix;
	int		rc;

	if (MACH_PORT_VALID(d->irq_port))
		return KERN_INVALID_ARGUMENT;	/* one interrupt per driver */

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&d->irq_port);
	if (kr != KERN_SUCCESS)
		return kr;

	/*
	 * MSI-X entry 0 where the device has a table.  MAKE_SEND: the kernel
	 * keeps a send right made from this receive right, so none is inserted
	 * here.
	 */
	msix = device_msi_register(d->master_device, d->bus, d->slot, d->func,
				   0, d->irq_port, MACH_MSG_TYPE_MAKE_SEND,
				   &slot);
	if (msix == KERN_SUCCESS) {
		d->irq_slot = slot;
		d->irq_msix = 1;
	} else {
		unsigned int line;

		kr = driver_config_read(d, 0x3C, &cfg);
		line = cfg & 0xFF;
		if (kr == KERN_SUCCESS && (line == 0 || line >= 16))
			kr = KERN_INVALID_ARGUMENT;
		if (kr == KERN_SUCCESS)
			kr = device_intr_register(d->master_device, line,
						  d->irq_port,
						  MACH_MSG_TYPE_MAKE_SEND);
		if (kr != KERN_SUCCESS) {
			driver_log(d, "no interrupt: MSI-X answered 0x%x, and "
				   "INTx line %u 0x%x", msix, line, kr);
			(void)mach_port_mod_refs(mach_task_self(), d->irq_port,
						 MACH_PORT_RIGHT_RECEIVE, -1);
			d->irq_port = MACH_PORT_NULL;
			return kr;
		}
		d->irq_slot = line;
		d->irq_msix = 0;
	}

	d->irq_handler = handler;
	d->irq_arg = arg;
	rc = pthread_create(&th, NULL, interrupt_thread, d);
	if (rc != 0) {
		driver_log(d, "no thread for the interrupt: %d", rc);
		(void)device_intr_unregister(d->master_device, d->irq_slot);
		(void)mach_port_mod_refs(mach_task_self(), d->irq_port,
					 MACH_PORT_RIGHT_RECEIVE, -1);
		d->irq_port = MACH_PORT_NULL;
		return KERN_RESOURCE_SHORTAGE;
	}
	(void)pthread_detach(th);
	return KERN_SUCCESS;
}
