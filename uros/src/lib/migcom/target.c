/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The targets migcom knows how to generate for (#416).
 */

#include <string.h>

#include "target.h"

/*
 * Every number below is derived from one thing — how wide an address is — in
 * exactly the way <mach/message.h> derives the same layout for the compiler
 * (#413).  They are written out rather than computed so that reading this
 * file tells you what a target's messages look like, and so that a target
 * whose layout is not that shape has somewhere to say so.
 *
 *	header      six 32-bit fields, on every target
 *	body        one 32-bit count, aligned like an address
 *	descriptor  an address, a 32-bit count, a 32-bit flags word
 */
static const mig_target_t Targets[] = {
	{
		.mt_name		= "i386",
		.mt_header_size		= 24,
		.mt_body_size		= 4,
		.mt_ndr_size		= 8,
		.mt_descriptor_size	= 12,
		.mt_descriptor_align	= 4,
		/*
		 * Four, and not eight.  The i386 System V ABI aligns a 64-bit
		 * scalar to four bytes, so no field in a message ever needs
		 * padding in front of it and adding up sizes happens to be
		 * the same computation as laying out a structure.  That
		 * coincidence is the reason this tool got away with counting
		 * for thirty years.
		 */
		.mt_max_align		= 4,
	},
	{
		.mt_name		= "x86_64",
		.mt_header_size		= 24,
		.mt_body_size		= 8,
		.mt_ndr_size		= 8,
		.mt_descriptor_size	= 16,
		.mt_descriptor_align	= 8,
		.mt_max_align		= 8,
	},
};

#define	NTARGETS	(sizeof Targets / sizeof Targets[0])

/*
 * i386 by default, because every invocation that predates this option means
 * i386 and there is no honest way to guess.  Defaulting to the host would be
 * the same mistake in a new place.
 */
const mig_target_t *Target = &Targets[0];

boolean_t
TargetSelect(const char *name)
{
	unsigned i;

	for (i = 0; i < NTARGETS; i++)
		if (strcmp(Targets[i].mt_name, name) == 0) {
			Target = &Targets[i];
			return TRUE;
		}

	return FALSE;
}

const char *
TargetNames(void)
{
	static char names[128];
	unsigned i;

	names[0] = '\0';
	for (i = 0; i < NTARGETS; i++) {
		if (i != 0)
			strncat(names, ", ", sizeof names - strlen(names) - 1);
		strncat(names, Targets[i].mt_name,
			sizeof names - strlen(names) - 1);
	}

	return names;
}

u_int
TargetAlignUp(u_int offset, u_int align)
{
	if (align < 2)
		return offset;

	return (offset + align - 1) & ~(align - 1);
}

u_int
TargetScalarAlign(u_int bytes)
{
	u_int align = 1;

	while (align < bytes && align < Target->mt_max_align)
		align <<= 1;

	return align;
}
