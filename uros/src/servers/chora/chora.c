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
 * chora -- one program for every device, and one task per device (#963, #773).
 *
 *   chora CLASS MODULE BB:DD.F MODULE-PORT
 *
 * The HAL starts it on a device it found (#964): a task given the drivers'
 * bootstrap port, loaded by exec_server from chora's image with a manifest
 * naming the one function, and given the module the HAL chose for the device,
 * as bytes on a port of its own whose name is MODULE-PORT (chora_given.h).
 * chora takes the module it was given, first: a chora given none claims
 * nothing.  Then it opens the device through libdriver, which names the task
 * MODULE@BB:DD.F and claims the device; loads the module as CLASS/MODULE.so,
 * asking nobody else for one; and hands it the device.  Then it stays, for as
 * long as the module drives it.
 *
 * Whatever refuses, chora says which step and ends: a chora that drives no
 * device has no reason to be.
 */

#include <mach.h>
#include <mach/thread_switch.h>
#include <mach/module_pool.h>		/* MODULE_NAME_MAX */
#include <stdio.h>
#include <string.h>

#include "libdriver.h"
#include "chora_module.h"
#include "chora_given.h"
#include "modload.h"

/* The interrupt thread keeps a pointer to it for the task's whole life. */
static struct driver	the_device;

/* Two hex digits, a colon, two, a dot, one: "00:03.0". */
static int
parse_bdf(const char *s, natural_t *bdf)
{
	unsigned int	v[3] = { 0, 0, 0 };
	int		field = 0, digits = 0;
	const char	*p;

	for (p = s; *p != '\0'; p++) {
		unsigned int	d;

		if (*p == ':' && field == 0 && digits > 0) {
			field = 1;
			digits = 0;
			continue;
		}
		if (*p == '.' && field == 1 && digits > 0) {
			field = 2;
			digits = 0;
			continue;
		}
		if (*p >= '0' && *p <= '9')
			d = (unsigned int)(*p - '0');
		else if (*p >= 'a' && *p <= 'f')
			d = (unsigned int)(*p - 'a' + 10);
		else if (*p >= 'A' && *p <= 'F')
			d = (unsigned int)(*p - 'A' + 10);
		else
			return -1;
		if (++digits > 2)
			return -1;
		v[field] = v[field] * 16 + d;
	}
	if (field != 2 || digits == 0 || v[1] > 31 || v[2] > 7)
		return -1;
	*bdf = (natural_t)((v[0] << 8) | (v[1] << 3) | v[2]);
	return 0;
}

/* "0x" and up to eight hexadecimal digits: a port's name in this task. */
static int
parse_port(const char *s, mach_port_t *port)
{
	unsigned long	v = 0;
	int		digits = 0;
	const char	*p;

	if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
		return -1;
	for (p = s + 2; *p != '\0'; p++) {
		unsigned int	d;

		if (*p >= '0' && *p <= '9')
			d = (unsigned int)(*p - '0');
		else if (*p >= 'a' && *p <= 'f')
			d = (unsigned int)(*p - 'a' + 10);
		else if (*p >= 'A' && *p <= 'F')
			d = (unsigned int)(*p - 'A' + 10);
		else
			return -1;
		if (++digits > 8)
			return -1;
		v = v * 16 + d;
	}
	if (digits == 0 || !MACH_PORT_VALID((mach_port_t)v))
		return -1;
	*port = (mach_port_t)v;
	return 0;
}

/*
 * The module given on `box' (chora_given.h), received without waiting: it was
 * queued before this task ran.  Its bytes, this task's from here, or -1 said
 * as `who', the name the task is about to take.  The port goes either way,
 * and nothing can be sent on it again.
 */
static int
take_given_module(const char *who, mach_port_t box, vm_offset_t *data,
		  vm_size_t *size)
{
	chora_given_module_rcv_t	r;
	kern_return_t			kr;

	memset(&r, 0, sizeof r);
	kr = mach_msg(&r.m.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof r,
		      box, 0, MACH_PORT_NULL);
	(void)mach_port_mod_refs(mach_task_self(), box, MACH_PORT_RIGHT_RECEIVE,
				 -1);
	if (kr != KERN_SUCCESS) {
		printf("%s: no module given on port 0x%x (0x%x): whoever "
		       "started this gave it none\n", who, box, kr);
		return -1;
	}
	if (r.m.head.msgh_id != CHORA_GIVEN_MODULE_ID ||
	    (r.m.head.msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0 ||
	    r.m.body.msgh_descriptor_count != 1 ||
	    r.m.module.type != MACH_MSG_OOL_DESCRIPTOR ||
	    r.m.module.size == 0 || r.m.module.size > CHORA_GIVEN_MODULE_MAX) {
		printf("%s: the message on port 0x%x is no module: id %d, %u "
		       "descriptors, %u bytes\n", who, box,
		       (int)r.m.head.msgh_id,
		       (unsigned)r.m.body.msgh_descriptor_count,
		       (unsigned)r.m.module.size);
		mach_msg_destroy(&r.m.head);
		return -1;
	}
	*data = (vm_offset_t)r.m.module.address;
	*size = (vm_size_t)r.m.module.size;
	return 0;
}

int
main(int argc, char **argv)
{
	const struct chora_module_ops	*ops;
	char				file[MODULE_NAME_MAX];
	char				who[MODULE_NAME_MAX + 16];
	natural_t			bdf = 0;
	mach_port_t			box = MACH_PORT_NULL;
	vm_offset_t			data = 0;
	vm_size_t			size = 0;
	int				n;

	if (argc != 5 || parse_bdf(argv[3], &bdf) != 0 ||
	    parse_port(argv[4], &box) != 0) {
		printf("chora: usage: chora CLASS MODULE BB:DD.F MODULE-PORT\n");
		return 1;
	}
	n = snprintf(file, sizeof file, "%s.so", argv[2]);
	if (n < 0 || (size_t)n >= sizeof file) {
		printf("chora: %s.so is longer than a module's name\n", argv[2]);
		return 1;
	}
	(void)snprintf(who, sizeof who, "%s@%s", argv[2], argv[3]);
	if (take_given_module(who, box, &data, &size) != 0)
		return 1;
	if (driver_open(&the_device, argv[2], bdf) != 0)
		return 1;
	modload_init(the_device.name);
	ops = modload_load_given(argv[1], file, data, size,
				 CHORA_MODULE_SUFFIX);
	if (ops == NULL) {
		driver_log(&the_device, "%s/%s, the module it was given, did not "
			   "load", argv[1], file);
		return 1;
	}
	if (ops->attach(&the_device) != 0) {
		driver_log(&the_device, "%s did not take the device", ops->name);
		return 1;
	}
	driver_log(&the_device, "%s drives the device", ops->name);

	/* What happens from here happens on the module's interrupt thread. */
	for (;;)
		(void)thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 60000);
}
