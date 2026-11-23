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
 * driver_port_test -- the driver's port, asked by a task that holds the master
 * port and no device (#964).
 *
 * A chora the HAL starts holds the driver's port where it held the master port:
 * the routines of device_master.defs a driver calls take it, each bound to the
 * caller's claim, and nothing else does.  This asks the kernel's half of that:
 *
 *   [1] the driver's port, asked on the master port, is a port of its own;
 *   [2] it is not given out on itself;
 *   [3] device_open() refuses it, where the master port opens the console;
 *   [4] device_claim_holder(), which no driver calls, refuses it, where the
 *       master port answers;
 *   [5] device_dma_faults() refuses on it a device the caller does not hold,
 *       where the master port answers;
 *   [6] device_intr_register() refuses on it a line that no claimed device
 *       raises -- the keyboard's line, were the keyboard's server late;
 *   [7] configuration space stays the claim's on it: a device the caller does
 *       not hold is refused.
 *
 * What is refused on one port and answered on the other is the port, not the
 * routine.  [6] is not set beside the master port: there the line would be
 * taken, and kept by this task.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/bootstrap.h>		/* bootstrap_ports */
#include <stdio.h>
#include <string.h>
#include <device/device.h>		/* device_open, device_close */
#include <servers/netname_wait.h>
#include "hal_server.h"			/* struct hal_device_info */
#include "hal.h"			/* hal_list_devices */
#include "device_master.h"		/* device_driver_port, and the rest */

#define ARMS		7
#define LEGACY_LINES	16		/* IRQ_FORWARD_LINES */

static int		passed, asked;
static security_token_t	no_token;

static void
said(int arm, int ok, const char *what)
{
	asked++;
	if (ok)
		passed++;
	printf("driver_port_test: [%d] %s%s\n", arm, what,
	       ok ? "" : " -- WRONG");
}

static void
not_asked(int arm, const char *why)
{
	printf("driver_port_test: [%d] NOT ASKED: %s\n", arm, why);
}

/*
 * A line from 3 to 15 that no device the HAL lists raises, so that no claimed
 * device can: -1 when the HAL lists none or every line is taken.
 */
static int
free_line(void)
{
	mach_port_t		hal = MACH_PORT_NULL;
	vm_offset_t		data = 0;
	mach_msg_type_number_t	cnt = 0;
	const struct hal_device_info *d;
	unsigned int		i, n = 0, used = 0;
	int			line;

	if (netname_wait(name_server_port, "hal", &hal) != KERN_SUCCESS)
		return -1;
	if (hal_list_devices(hal, &data, &cnt, &n) != KERN_SUCCESS) {
		(void)mach_port_deallocate(mach_task_self(), hal);
		return -1;
	}
	(void)mach_port_deallocate(mach_task_self(), hal);
	d = (const struct hal_device_info *)data;
	if (n > cnt / sizeof(*d))
		n = cnt / sizeof(*d);
	for (i = 0; i < n; i++)
		if (d[i].irq < LEGACY_LINES)
			used |= 1u << d[i].irq;
	if (cnt != 0)
		(void)vm_deallocate(mach_task_self(), data, cnt);
	if (n == 0)
		return -1;
	for (line = 3; line < LEGACY_LINES; line++)
		if ((used & (1u << line)) == 0)
			return line;
	return -1;
}

int
main(int argc, char **argv)
{
	mach_port_t	hostp = MACH_PORT_NULL, master = MACH_PORT_NULL;
	mach_port_t	wired = MACH_PORT_NULL, paged = MACH_PORT_NULL;
	mach_port_t	security = MACH_PORT_NULL;
	mach_port_t	drv = MACH_PORT_NULL, again = MACH_PORT_NULL;
	mach_port_t	dev = MACH_PORT_NULL, self = MACH_PORT_NULL;
	natural_t	held = 0, confined = 0, count = 0, lost = 0, undrained = 0;
	natural_t	value = 0;
	vm_address_t	address = 0;
	kern_return_t	kr, kr_m;
	char		line[160];
	int		irq;

	(void)argc;
	(void)argv;
	printf("driver_port_test: started\n");

	if (bootstrap_ports(bootstrap_port, &hostp, &master, &wired, &paged,
			    &security) != KERN_SUCCESS ||
	    !MACH_PORT_VALID(master)) {
		printf("driver_port_test: no master device port from bootstrap "
		       "-- WRONG, its line in bootstrap.conf is -D\n");
		printf("driver_port_test: 0 of %d arms passed\n", ARMS);
		return 1;
	}

	/* [1] */
	kr = device_driver_port(master, &drv);
	snprintf(line, sizeof line, "the driver's port, asked on the master "
		 "port: 0x%x, %s", kr, kr != KERN_SUCCESS ? "none" :
		 drv == master ? "the master port itself" : "a port of its own");
	said(1, kr == KERN_SUCCESS && MACH_PORT_VALID(drv) && drv != master,
	     line);
	if (kr != KERN_SUCCESS || !MACH_PORT_VALID(drv) || drv == master) {
		printf("driver_port_test: %d of %d arms passed\n", passed,
		       ARMS);
		return 1;
	}

	/* [2] */
	kr = device_driver_port(drv, &again);
	snprintf(line, sizeof line, "the driver's port, asked on itself: %s, "
		 "0x%x", kr == KERN_SUCCESS ? "given" : "refused", kr);
	said(2, kr != KERN_SUCCESS, line);
	if (kr == KERN_SUCCESS)
		(void)mach_port_deallocate(mach_task_self(), again);

	/* [3] */
	kr = device_open(drv, MACH_PORT_NULL, D_READ | D_WRITE, no_token,
			 (char *)"console", &dev);
	if (kr == KERN_SUCCESS) {
		(void)device_close(dev);
		(void)mach_port_deallocate(mach_task_self(), dev);
		dev = MACH_PORT_NULL;
	}
	kr_m = device_open(master, MACH_PORT_NULL, D_READ | D_WRITE, no_token,
			   (char *)"console", &dev);
	if (kr_m == KERN_SUCCESS) {
		(void)device_close(dev);
		(void)mach_port_deallocate(mach_task_self(), dev);
	}
	snprintf(line, sizeof line, "device_open(\"console\") on the driver's "
		 "port: %s, 0x%x; on the master port: 0x%x",
		 kr == KERN_SUCCESS ? "opened" : "refused", kr, kr_m);
	if (kr_m != KERN_SUCCESS && kr != KERN_SUCCESS)
		not_asked(3, "the master port did not open the console either");
	else
		said(3, kr != KERN_SUCCESS && kr_m == KERN_SUCCESS, line);

	/* [4], [5]: 00:00.0, the host bridge, which no task claims. */
	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			       &self) != KERN_SUCCESS ||
	    mach_port_insert_right(mach_task_self(), self, self,
				   MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
		printf("driver_port_test: no port of its own -- WRONG\n");
		printf("driver_port_test: %d of %d arms passed\n", passed,
		       ARMS);
		return 1;
	}
	kr = device_claim_holder(drv, 0, self, &held);
	kr_m = device_claim_holder(master, 0, self, &held);
	snprintf(line, sizeof line, "device_claim_holder(00:00.0), which no "
		 "driver calls, on the driver's port: %s, 0x%x; on the master "
		 "port: 0x%x", kr == KERN_SUCCESS ? "answered" : "refused", kr,
		 kr_m);
	if (kr_m != KERN_SUCCESS && kr != KERN_SUCCESS)
		not_asked(4, "the master port did not answer either");
	else
		said(4, kr != KERN_SUCCESS && kr_m == KERN_SUCCESS, line);

	kr = device_dma_faults(drv, 0, &confined, &count, &address, &lost,
			       &undrained);
	kr_m = device_dma_faults(master, 0, &confined, &count, &address,
				 &lost, &undrained);
	snprintf(line, sizeof line, "device_dma_faults(00:00.0), no device of "
		 "this task's, on the driver's port: %s, 0x%x; on the master "
		 "port: 0x%x", kr == KERN_SUCCESS ? "answered" : "refused", kr,
		 kr_m);
	if (kr_m != KERN_SUCCESS && kr != KERN_SUCCESS)
		not_asked(5, "the master port did not answer either");
	else
		said(5, kr == KERN_NO_ACCESS && kr_m == KERN_SUCCESS, line);

	/* [6] */
	irq = free_line();
	if (irq < 0)
		not_asked(6, "the HAL lists no device, or every line from 3 "
			  "to 15 is a device's");
	else {
		kr = device_intr_register(drv, (unsigned int)irq, self,
					  MACH_MSG_TYPE_MAKE_SEND);
		snprintf(line, sizeof line, "device_intr_register(%d), a line "
			 "no device raises, on the driver's port: %s, 0x%x",
			 irq, kr == KERN_SUCCESS ? "taken" : "refused", kr);
		said(6, kr == KERN_NO_ACCESS, line);
		if (kr == KERN_SUCCESS)
			(void)device_intr_unregister(master,
						     (unsigned int)irq);
	}

	/* [7] */
	kr = device_pci_config_read(drv, 0, 0, 0, 0, &value);
	snprintf(line, sizeof line, "device_pci_config_read(00:00.0, 0), no "
		 "device of this task's, on the driver's port: %s, 0x%x",
		 kr == KERN_SUCCESS ? "answered" : "refused", kr);
	said(7, kr != KERN_SUCCESS, line);

	printf("driver_port_test: %d of %d arms passed\n", passed, asked);
	return 0;
}
