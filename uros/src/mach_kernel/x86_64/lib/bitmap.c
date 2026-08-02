/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Bit arrays, for the scheduler's run-queue bitmap (#453).
 *
 * Three names the machine-independent tree calls: set a bit, clear a bit,
 * find the lowest set bit in an array of `int'.  The run queue uses them on
 * every enqueue and every dispatch, so the last of the three is on the
 * scheduler's hot path.
 *
 * ── Why these are C and not assembly ──────────────────────────────────
 *
 * i386 writes them in locore.S with `btsl', `btrl' and a hand-rolled scan
 * loop.  Here the compiler emits the same instructions from `|=', `&=' and
 * __builtin_ctz -- and does better with the last one, because __builtin_ctz
 * becomes TZCNT where the processor has BMI1 and BSF where it does not,
 * chosen at compile time from -march rather than fixed in a source file.
 *
 * ⚠️ The unit is `int', which is what the interface says and what the run
 * queue declares -- `int bitmap[NRQBM]'.  Widening to 64-bit words would
 * halve the scan and is the obvious modern move, and it is not made here:
 * the array is machine-independent and a machine that scanned it in units
 * the owner did not use would read past the end on the last word.  That is
 * #454's kind of change, not this one's.
 */

#include <kern/misc_protos.h>
#include <kern/sched.h>

#define	BITS_PER_INT	(8 * (int) sizeof(int))

void
setbit(int which, int *bitmap)
{
	bitmap[which / BITS_PER_INT] |= 1 << (which % BITS_PER_INT);
}

void
clrbit(int which, int *bitmap)
{
	bitmap[which / BITS_PER_INT] &= ~(1 << (which % BITS_PER_INT));
}

/*
 * The lowest set bit in the array, or -1 when there is none.
 *
 * ⚠️ NRQBM words and no length argument: the interface has none, so the
 * bound comes from the only array anyone passes.  That is a real constraint
 * rather than an assumption -- it is why this file includes <kern/sched.h>,
 * so that a change to NRQBM reaches here instead of leaving a scan that
 * stops early or runs past.
 */
int
ffsbit(int *bitmap)
{
	int	i;

	for (i = 0; i < NRQBM; i++)
		if (bitmap[i] != 0)
			return i * BITS_PER_INT
			       + __builtin_ctz((unsigned int) bitmap[i]);

	return -1;
}
