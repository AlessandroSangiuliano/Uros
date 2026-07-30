/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * How a port name divides into an index and a generation — i386 (#413).
 *
 * A name is one 32-bit word carrying two things: which slot of the task's
 * entry table it refers to, and how many times that slot has been handed out
 * before.  The generation is what stops a name that a task kept after
 * deallocating it from silently meaning whatever port landed in the slot
 * next — so the number of generation values is the number of recycles a
 * stale name has to survive before it becomes a lie.
 *
 * ⚠️ This file states what i386 has always done, unchanged and on purpose.
 * It is the configuration that runs, its numbers are load-bearing in code
 * nobody is going to re-test for a port to another architecture, and #413's
 * business is x86-64.  The values below are the ones the masks in
 * mach/port.h and ipc/ipc_entry.h have carried since the beginning; moving
 * them here changes where they are written and nothing else.
 */

#ifndef	_MACH_I386_PORT_NAME_H_
#define _MACH_I386_PORT_NAME_H_

/*
 * Eight bits of generation, twenty-four of index.
 *
 * The index is far larger than anything reachable: a task's names come out
 * of its entry table, and that table stops growing at 967,168 entries — a
 * twenty-bit number.  The four bits above it can never name a slot.
 */
#define	MACH_PORT_GEN_BITS	8

/*
 * ...of which the low two never move.
 *
 * The generation advances by four, not by one, so the field takes 64 of its
 * 256 values and the low two bits of every name the kernel allocates are
 * zero.  They were reserved for a tag distinguishing kinds of port name on a
 * fast path — the interface generator still emits the test for it wrapped in
 * an `if (0)`, with a comment saying what it should have been: a check that
 * the low two bits of the name are clear.
 *
 * The tag was never implemented, so the two bits buy nothing and cost three
 * quarters of the recycle window.  They stay zero here anyway: this target's
 * job is to keep working, and "nothing appears to use it" is not the same
 * measurement as "nothing uses it".
 */
#define	MACH_PORT_GEN_SKIP	2

#endif	/* _MACH_I386_PORT_NAME_H_ */
