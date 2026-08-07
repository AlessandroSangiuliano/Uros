/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * How a port name divides into an index and a generation — x86-64 (#413).
 *
 * ── What the division is for ──────────────────────────────────────────
 *
 * A name is one 32-bit word carrying which slot of the task's entry table it
 * refers to, and how many times that slot has been handed out.  The
 * generation is what stops a name a task kept after deallocating it from
 * quietly meaning whatever port landed in the slot next, so the count of
 * generation values is the number of recycles a stale name has to survive
 * before it becomes a lie about which port it names.  That is a capability
 * property, and it is worth spending bits on.
 *
 * ── Why the split moves here, and moves at all ────────────────────────
 *
 * i386 divides it 24 and 8, and worse than that sounds: the generation
 * advances by four rather than by one, so it takes **64** of its 256 values.
 * A name is reused every 64 recycles of a slot.
 *
 * The 24 bits of index are not paying for anything.  A task's names come out
 * of its entry table, `ipc_table_fill` grows that table through 511 sizes and
 * stops, and on this target it stops at **484,608 entries** — 14.8 MiB of
 * table, and a nineteen-bit number.  (On i386 the same 14.8 MiB is 967,168
 * entries, because an entry is half the size.)  Everything above bit 20 has
 * never been reachable on either target.
 *
 * So the index gives up two bits it cannot use and the generation takes them,
 * and the two bits i386 holds at zero for a fast-path tag that was never
 * implemented are spent as well:
 *
 *	i386     24 index, 8 generation, 2 of them frozen ->    64 recycles
 *	x86-64   22 index, 10 generation, none frozen     ->  1024 recycles
 *
 * Sixteen times the window, four million reachable slots against half a
 * million the table can produce, and not one bit spent anywhere — the bits
 * the generation grows into are free in `ie_bits` too (see ipc/ipc_entry.h,
 * where the collision flag sits immediately below the generation field and
 * moves down with it).
 *
 * ⚠️ This is not the sixty-four-bit port name, and does not decide against
 * one.  A wider name buys a window of millions instead of a thousand, for
 * eight bytes on every message header; that is a question for when there is
 * something to measure it with.  This is the part that costs nothing.
 */

#ifndef	_MACH_X86_64_PORT_NAME_H_
#define _MACH_X86_64_PORT_NAME_H_

/* Ten bits of generation, twenty-two of index. */
#define	MACH_PORT_GEN_BITS	10

/*
 * ...and none of them held still.  The generation advances by one, so all
 * 1024 values are used, and no bit of a name is reserved for anything.
 */
#define	MACH_PORT_GEN_SKIP	0

#endif	/* _MACH_X86_64_PORT_NAME_H_ */
