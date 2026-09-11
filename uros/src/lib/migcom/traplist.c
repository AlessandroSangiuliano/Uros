/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The kernel traps a generated stub may try before it builds a message (#543).
 *
 * ── Why this is compiled in and not an option ────────────────────────────
 *
 * libmach carries a trap wrapper for 118 kernel entry points and the kernel
 * implements every one, and until now nothing called them: the ms_*.c layer
 * that was supposed to was never compiled.  The fast path therefore goes where
 * every caller already arrives -- inside the stub MIG generates -- and for that
 * MIG has to know which traps exist.
 *
 * 🔴 THE FIRST VERSION TOOK A -traplist OPTION AND THAT WAS WRONG.  This build
 * invokes migcom 82 times from more than twenty hand-written CMake rules, with
 * no single funnel between them; add_mig_defs() is one caller among many, and
 * its own comment says so.  An option would have had to be added to all of
 * them, and the next server added to the tree would have gone without --
 * silently, producing stubs with no fast path while everything else had one.
 * That is the same shape as the thing this issue exists to fix.
 *
 * 🔑 So the list is a table generated from syscall_sw.h when migcom is built,
 * and migcom cannot be invoked without it.  Impossible rather than
 * remembered.  CMake regenerates it whenever that header changes, so it cannot
 * drift from the traps it describes.
 *
 * ⚠️ Two independent checks, because neither is enough alone.  The argument
 * count lets a routine be declined when its stub takes a different number of
 * parameters than the trap; and the emitted call is compiled against the
 * prototypes in <mach/mach_syscalls.h>, so a TYPE mismatch is a build error
 * rather than a wrongly-laid-out call frame.  The count cannot see types, and
 * the compiler never sees a call that was never emitted.
 */

#include <string.h>

#include "global.h"

struct trap_entry {
	const char	*te_name;
	int		 te_argc;
};

static const struct trap_entry traps[] = {
#include "traps.inc"
};

#define	NTRAPS	((int) (sizeof traps / sizeof traps[0]))

/*
 * Is there a trap called `name' taking exactly `argc' arguments?
 *
 * ⚠️ The argument count is part of the question and not a detail.  A trap under
 * the right name with the wrong arity is precisely what must NOT be emitted:
 * the compiler catches a type mismatch, but a call with too few arguments
 * against a prototype MIG never saw is how a frame gets read past its end.
 */
boolean_t
TrapListHas(const char *name, int argc)
{
	int	i;

	for (i = 0; i < NTRAPS; i++)
		if (traps[i].te_argc == argc &&
		    strcmp(traps[i].te_name, name) == 0)
			return TRUE;
	return FALSE;
}

boolean_t
TrapListActive(void)
{
	return NTRAPS > 0;
}
