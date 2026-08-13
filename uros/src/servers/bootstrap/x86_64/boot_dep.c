/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What this machine can do with a server image, besides run it (#422).
 *
 * i386's file of this name holds three functions.  One of them, parse_args,
 * is not machine-dependent at all -- it walks argv looking for switches --
 * and had been copied into every machine's directory: i386, POWERMAC, HP700.
 * A fourth copy would have been the cheapest thing to write here and the
 * worst: it lives in bootstrap/parse_args.c now, once, for all of them.
 *
 * The two that remain are genuinely this machine's, and on this machine they
 * both have the same answer.
 */

#include <mach.h>
#include <mach/x86_64/vm_param.h>

#include "bootstrap.h"

/*
 * ── No kernel-loaded servers here, and that is a decision ─────────────
 *
 * Collocation -- linking a server into the kernel's address space so its
 * calls become procedure calls -- is what mapbase, mapend and mapsize are
 * for, and what is_kernel_loadable() answers about.  i386 supports it and
 * this target does not: it is the same argument as the empty device table in
 * x86_64/cpu/conf.c.  The isolation a server gets from its own address space
 * is the point of a microkernel, and the performance answer here is PCID and
 * a faster IPC path (#412, #446), not moving the server inside.
 *
 * ⚠️ So these two report the truth rather than a placeholder.  map_init
 * leaves the range zeroed, which is what "no such window" looks like to the
 * caller, and is_kernel_loadable answers FALSE for every image -- not because
 * the check is unwritten, but because the answer is no.  Returning TRUE and
 * letting the load fail later would be the fake.
 */
void
map_init(struct server **sp, boolean_t doit)
{
	(void) doit;

	(*sp)->mapbase = 0;
	(*sp)->mapend  = 0;
	(*sp)->mapsize = 0;
}

boolean_t
is_kernel_loadable(struct server *sp, struct objfmt *ofmt)
{
	(void) sp;
	(void) ofmt;

	return FALSE;
}
