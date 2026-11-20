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
 *   chora CLASS MODULE BB:DD.F
 *
 * Whoever found the device starts it -- chora_test now, the HAL in #964 --
 * with exec_load(), which gives this task a capability port derived from its
 * parent's: what chora's manifest asks for, any PCI class, cut to what the
 * parent may hand on (#743).  chora then opens the device through libdriver,
 * which names the task MODULE@BB:DD.F and claims the device; fetches the one
 * module it was told from its class's pool; and hands the module the device.
 * Then it stays, for as long as the module drives it.
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

int
main(int argc, char **argv)
{
	const struct chora_module_ops	*ops;
	char				file[MODULE_NAME_MAX];
	natural_t			bdf = 0;
	int				n;

	if (argc != 4 || parse_bdf(argv[3], &bdf) != 0) {
		printf("chora: usage: chora CLASS MODULE BB:DD.F\n");
		return 1;
	}
	if (driver_open(&the_device, argv[2], bdf) != 0)
		return 1;

	n = snprintf(file, sizeof file, "%s.so", argv[2]);
	if (n < 0 || (size_t)n >= sizeof file) {
		driver_log(&the_device, "%s.so is longer than a module's name",
			   argv[2]);
		return 1;
	}
	modload_init(the_device.name);
	ops = modload_load_named(argv[1], file, CHORA_MODULE_SUFFIX);
	if (ops == NULL) {
		driver_log(&the_device, "modules/%s/%s did not load", argv[1],
			   file);
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
