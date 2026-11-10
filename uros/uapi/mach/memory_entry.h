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
 * mach/memory_entry.h -- a range of a task's memory named by a port (#911).
 *
 * urmach_make_memory_entry() (mach_host.defs) takes a range of a task's
 * address space and answers a send right to a port that names it.  Whoever
 * holds the right maps the same pages with vm_map(), the port in place of a
 * memory object, and never holds the task the pages came from.  That is the
 * point: before this, the only way to put a page of one task into another
 * was vm_remap(), which needs the other task's port -- the right to read and
 * write all of its memory, to make and stop its threads, and to take its
 * port rights.
 *
 * The range is cut into segments, each with the most a mapping of it may
 * have: a segment of VM_PROT_READ can be mapped, and its protection changed,
 * but never written.  The maker chooses them, so a server that shares a ring
 * with a client decides which pages the client may write.  A mapping asks for
 * a current and a maximum protection as vm_map() always did, and each
 * segment gets the smaller of what was asked and what the segment allows.
 */

#ifndef	_MACH_MEMORY_ENTRY_H_
#define	_MACH_MEMORY_ENTRY_H_

#include <mach/machine/vm_types.h>
#include <mach/vm_prot.h>

/*
 * At most this many segments in one entry.  A FLIPC channel needs four
 * (constants, producer, consumer, ring and data); the bound keeps the request
 * small and the entry a fixed size.
 */
#define	URMACH_MEMORY_ENTRY_SEGMENTS_MAX	8

/*
 * Segment i covers [ends[i-1], ends[i]) of the range, ends[-1] being 0, for
 * the first `segments' of them.  The ends are page multiples, strictly
 * increasing, and the last one is the range's size.  prots[i] is segment i's
 * maximum protection.  Both arrays travel whole; what is past `segments' is
 * not looked at.
 */
typedef vm_size_t	urmach_memory_entry_ends_t[URMACH_MEMORY_ENTRY_SEGMENTS_MAX];
typedef vm_prot_t	urmach_memory_entry_prots_t[URMACH_MEMORY_ENTRY_SEGMENTS_MAX];

#endif	/* _MACH_MEMORY_ENTRY_H_ */
