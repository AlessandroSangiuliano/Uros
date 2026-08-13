/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The console character device, which every machine implements and every
 * machine's conf.c names (#426).
 *
 * ⚠️ These three used to be declared by hand inside i386/AT386/conf.c, three
 * lines above the table that takes their addresses -- the same habit that put
 * twenty private declarations of the Mach traps around the tree.  A device
 * table is exactly where it costs: the entries are FUNCTION POINTERS, so a
 * declaration that disagrees with the definition is not a diagnostic, it is a
 * call through a pointer with the wrong argument list.
 *
 * The implementations are per-machine -- i386/AT386/console_cdev.c and
 * x86_64/cpu/console_cdev.c -- and the interface is not, so it lives here,
 * with the device layer that calls through it.
 *
 * There is no read handler on purpose.  The console is a printf sink; input
 * belongs to whoever owns the keyboard, which is a user-space server on both
 * targets.  conf.c must name NO_READ and not NULL_READ -- see the comment on
 * either implementation for the null-buffer path a "successful" read walks
 * into.
 */

#ifndef	_DEVICE_CONSOLE_CDEV_H_
#define _DEVICE_CONSOLE_CDEV_H_

#include <device/device_types.h>
#include <device/io_req.h>

extern io_return_t	consoleopen(dev_t, dev_mode_t, io_req_t);
extern void		consoleclose(dev_t);
extern io_return_t	consolewrite(dev_t, io_req_t);

#endif	/* _DEVICE_CONSOLE_CDEV_H_ */
