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
 * chora_absent -- a module of the class `net' that no chora is told to load
 * (#963).
 *
 * A chora fetches the one module it was named, and a task that drives one
 * device must not hold the code of the other drivers of its class.  With one
 * module in a class, a chora that fetched the whole class would look exactly
 * like one that fetched its module; with this one beside chora_probe it would
 * load this too, and modload says every module it loads.  It drives nothing:
 * a chora that was told to attach it would have been told wrongly, and says
 * so.
 */

#include "libdriver.h"
#include "chora_module.h"

static int
absent_attach(struct driver *d)
{
	driver_log(d, "WRONG -- chora_absent was attached, and no chora is "
		   "ever told to load it");
	return -1;
}

const struct chora_module_ops chora_absent_chora_ops = {
	.name	= "chora_absent",
	.attach	= absent_attach,
};
