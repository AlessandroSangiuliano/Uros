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
 * vm/vm_memory_entry.h -- a range of memory named by a port (#911).
 * The interface is in <mach/memory_entry.h>; this is the kernel's side.
 */

#ifndef	_VM_VM_MEMORY_ENTRY_H_
#define	_VM_VM_MEMORY_ENTRY_H_

#include <mach/boolean.h>
#include <mach/kern_return.h>
#include <mach/memory_entry.h>
#include <mach/message.h>
#include <mach/vm_inherit.h>
#include <mach/vm_prot.h>
#include <ipc/ipc_port.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>

/*
 * What a memory entry's port names.  Nothing in it changes after
 * urmach_make_memory_entry() fills it, so whoever holds a send right reads it
 * without a lock: the right keeps the port, and with it the entry, alive.
 */
struct vm_memory_entry {
	vm_object_t	object;		/* a reference of the entry's own */
	vm_offset_t	offset;		/* where the range starts in it */
	vm_size_t	size;		/* a page multiple */
	unsigned int	segments;	/* 1 .. URMACH_MEMORY_ENTRY_SEGMENTS_MAX */
	vm_size_t	ends[URMACH_MEMORY_ENTRY_SEGMENTS_MAX];
	vm_prot_t	prots[URMACH_MEMORY_ENTRY_SEGMENTS_MAX];
};

typedef struct vm_memory_entry	*vm_memory_entry_t;

#define	VM_MEMORY_ENTRY_NULL	((vm_memory_entry_t) 0)

extern void		vm_memory_entry_init(void);

/* vm_map() with a memory entry's port where a memory object would be. */
extern kern_return_t	vm_memory_entry_map(
				vm_map_t	map,
				vm_offset_t	*address,	/* IN/OUT */
				vm_size_t	size,
				vm_offset_t	mask,
				boolean_t	anywhere,
				ipc_port_t	port,
				vm_offset_t	offset,
				boolean_t	copy,
				vm_prot_t	cur_protection,
				vm_prot_t	max_protection,
				vm_inherit_t	inheritance);

/* The no-senders notification that ends an entry (ipc_kobject_notify). */
extern boolean_t	vm_memory_entry_notify(
				ipc_port_t		port,
				mach_msg_header_t	*msg);

#endif	/* _VM_VM_MEMORY_ENTRY_H_ */
