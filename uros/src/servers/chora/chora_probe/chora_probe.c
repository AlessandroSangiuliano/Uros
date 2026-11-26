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
 * chora_probe -- a test module of chora, for the network card: it maps a
 * region and takes one interrupt, and drives nothing (#963).
 *
 * No driver uses the card (#479), and irq_claim_test already makes it ring:
 * an Intel 8254x/8257x raises a link status change written into its ICS
 * register.  So: the card's first region, its registers, mapped; every cause
 * masked and cleared; the interrupt asked of libdriver; LSC unmasked and set;
 * and up to a second for the handler.  chora said the claim, and this says
 * the other two.
 *
 * On a card with an MSI-X table -- QEMU's 82574L, on q35 -- the "other"
 * causes go to the entry IVAR names, so IVAR points them at entry 0, which is
 * the one libdriver takes.  On one without -- QEMU's 82540EM, on the 1996
 * board -- the cause asserts the INTx line, and the line stays asserted until
 * ICR is read, which the handler does before libdriver unmasks it.
 *
 * ⚠️ A test module: what it was asked to see and did not is a WRONG, which
 * fails the boot.  chora itself never says WRONG -- a refused claim is what a
 * second chora on a device is supposed to meet.
 */

#include "libdriver.h"
#include "chora_module.h"

#include <mach.h>
#include <mach/thread_switch.h>

#define E1000_ICR		0x000C0u	/* cause read; reading clears */
#define E1000_ICS		0x000C8u	/* cause set */
#define E1000_IMS		0x000D0u	/* mask set */
#define E1000_IMC		0x000D8u	/* mask clear */
#define E1000_IVAR		0x000E4u	/* 82574L: each cause's MSI-X entry */
#define E1000_ICR_LSC		0x00000004u
#define E1000_ICR_OTHER		0x01000000u	/* 82574L: the "other" causes */
#define E1000_IVAR_OTHER_ENTRY0	(0x8u << 16)	/* "other": entry 0, valid */

#define PROBE_WAIT_STEPS	100		/* of 10 ms: a second */

static volatile uint32_t	*regs;
static volatile int		rang;
static volatile uint32_t	rang_icr;

static void
reg_write(unsigned int off, uint32_t value)
{
	regs[off / 4] = value;
}

static uint32_t
reg_read(unsigned int off)
{
	return regs[off / 4];
}

/* On the interrupt thread: what rang, read out of ICR, which clears it. */
static void
probe_interrupt(struct driver *d, void *arg)
{
	(void)d;
	(void)arg;
	rang_icr = reg_read(E1000_ICR);
	rang++;
}

/* Every cause masked, and every cause cleared: read, and written as ones. */
static void
quiet(void)
{
	reg_write(E1000_IMC, 0xFFFFFFFFu);
	(void)reg_read(E1000_ICR);
	reg_write(E1000_ICR, 0xFFFFFFFFu);
}

static int
probe_attach(struct driver *d)
{
	volatile void	*va = NULL;
	vm_size_t	size = 0;
	kern_return_t	kr;
	int		i;

	kr = driver_map_region(d, 0, &va, &size);
	if (kr != KERN_SUCCESS) {
		driver_log(d, "WRONG -- region 0, the registers, would not map: "
			   "0x%x", kr);
		return -1;
	}
	regs = (volatile uint32_t *)va;
	driver_log(d, "mapped region 0, %lu bytes at 0x%lx", (unsigned long)size,
		   (unsigned long)(vm_address_t)va);

	quiet();
	kr = driver_interrupt(d, probe_interrupt, NULL);
	if (kr != KERN_SUCCESS) {
		driver_log(d, "WRONG -- no interrupt to take: 0x%x", kr);
		return -1;
	}
	if (d->irq_message)
		reg_write(E1000_IVAR, E1000_IVAR_OTHER_ENTRY0);

	reg_write(E1000_IMS, E1000_ICR_LSC | (d->irq_message ? E1000_ICR_OTHER : 0));
	reg_write(E1000_ICS, E1000_ICR_LSC);
	for (i = 0; i < PROBE_WAIT_STEPS && rang == 0; i++)
		(void)thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 10);

	quiet();
	if (d->irq_message)
		reg_write(E1000_IVAR, 0);

	if (rang == 0) {
		driver_log(d, "WRONG -- told to ring, the card sent nothing in a "
			   "second to %s %u", d->irq_message ? "message slot" :
			   "INTx line", (unsigned)d->irq_slot);
		return -1;
	}
	driver_log(d, "took an interrupt, %s %u, ICR 0x%08x",
		   d->irq_message ? "message slot" : "INTx line",
		   (unsigned)d->irq_slot, (unsigned)rang_icr);
	return 0;
}

/*
 * The two cards it was written for (#964): QEMU's 82540EM, on the 1996 board,
 * and its 82574L, on q35.  By vendor and device, not by class: what it does
 * to the card -- ICS, IMS and IVAR -- is Intel's, and another network card's
 * registers are not these.
 */
CHORA_DRIVES(CHORA_DRIVES_PCI_ID(0x8086, 0x100e),
	     CHORA_DRIVES_PCI_ID(0x8086, 0x10d3));

const struct chora_module_ops chora_probe_chora_ops = {
	.name	= "chora_probe",
	.attach	= probe_attach,
};
