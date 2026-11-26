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
 * And it declares no device (#964): the HAL starts a module only for a device
 * the module names, so nobody starts this one.
 *
 * A chora fetches the one module it was named, and a task that drives one
 * device must not hold the code of the other drivers of its class.  With one
 * module in a class, a chora that fetched the whole class would look exactly
 * like one that fetched its module; with this one beside chora_probe it would
 * load this too.  It drives nothing: a chora that was told to attach it
 * would have been told wrongly, and says so.
 *
 * 🔑 AND IT SAYS SO WHEN IT IS LOADED, NOT WHEN SOMEBODY LOOKS.  modload says
 * each module it loads, but a line that only a reader looking for it would
 * find is not a check: a chora that fetched its class drove the card, its
 * test passed and the boot passed.  So libdl runs chora_absent_loaded() when
 * it loads this file -- DT_INIT, which the link names, since libdl calls
 * DT_INIT and not .init_array, where a constructor would go -- and the WRONG
 * it prints fails the boot, whichever program loaded it.
 *
 * 🔴 WHICHEVER PROGRAM: SO IT ASKS NOTHING OF LIBDRIVER (#964).  libdl
 * resolves every symbol of a module before it runs DT_INIT, and refuses the
 * module if one is missing.  This file called driver_log(), which only a
 * program linked with libdriver exports, so in any other program it was refused
 * before its WRONG, and said nothing but a dlopen failure.  The HAL is such a
 * program, and reading a pool by loading it is the mistake #964 must not make:
 * the ablation that made the HAL load its pools was not caught.  printf is
 * every program's.
 */

#include <stdio.h>

#include "libdriver.h"
#include "chora_module.h"

void	chora_absent_loaded(void);

void
chora_absent_loaded(void)
{
	printf("chora_absent: WRONG -- loaded, and no chora is ever told to "
	       "load it\n");
}

static int
absent_attach(struct driver *d)
{
	printf("%s: WRONG -- chora_absent was attached, and no chora is ever "
	       "told to load it\n", d->name);
	return -1;
}

const struct chora_module_ops chora_absent_chora_ops = {
	.name	= "chora_absent",
	.attach	= absent_attach,
};
