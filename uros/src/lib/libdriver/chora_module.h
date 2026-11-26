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
 * chora_module.h -- what a module of chora exports (#963).
 *
 * A module is a shared object in the pool of its device's class,
 * modules/<class>/<name>.so, and exports one table, <name>_chora_ops.  chora
 * fetches that one file, with modload_load_named(), and finds the table by
 * that name.
 *
 * A module says which devices it drives with CHORA_DRIVES(), in
 * chora_drives.h: the HAL reads that from the module's file and starts a chora
 * with the module for each device it names (#964).  A module that declares
 * nothing is started only by whoever names it.
 */

#ifndef _CHORA_MODULE_H_
#define _CHORA_MODULE_H_

#include "chora_drives.h"

#include "libdriver.h"

#define CHORA_MODULE_SUFFIX	"_chora_ops"

struct chora_module_ops {
	const char	*name;
	/*
	 * Drive the device chora has claimed for it.  0 when the module
	 * drives it; it says what it did through driver_log().
	 */
	int		(*attach)(struct driver *d);
};

#endif /* _CHORA_MODULE_H_ */
