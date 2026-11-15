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
 * vm/vm_memory_entry.c -- a range of memory named by a port (#911).
 *
 * A task that wants another to share some of its pages used to need that
 * other task's port, because vm_remap() maps into a task named by its port,
 * and a task port is everything: the memory, the threads, the port rights.
 * FLIPC's endpoint was handed the port of every client that connected, for
 * that one vm_remap().
 *
 * An entry turns it round.  The task with the pages makes an entry and sends
 * its port; the other maps it with vm_map() into itself.  Neither holds the
 * other.  The maker also chooses, segment by segment, the most a mapping may
 * have, which vm_protect() on the other's mapping never could: that set the
 * current protection only, and the owner of the mapping could set it back.
 *
 * The entry is the object it names, a range of it, and the segments.  It
 * holds a reference on the object, so the pages outlive the maker's mapping
 * for as long as anyone holds the port, and it goes, reference and all, with
 * the port's last send right (vm_memory_entry_notify()).
 */

#include <mach/boolean.h>
#include <mach/kern_return.h>
#include <mach/mach_types.h>
#include <mach/memory_entry.h>
#include <mach/memory_object.h>
#include <mach/notify.h>
#include <mach/vm_param.h>
#include <mach/mach_host_server.h>	/* urmach_make_memory_entry() */
#include <kern/ipc_kobject.h>
#include <kern/misc_protos.h>
#include <kern/zalloc.h>
#include <ipc/ipc_port.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_memory_entry.h>

/*
 * A zone of their own, so that a test can read how many there are, and
 * exhaustible, so that a task making entries until it is full is told
 * KERN_RESOURCE_SHORTAGE and the kernel does not panic.
 */
#define	VM_MEMORY_ENTRY_MAX	16384

static zone_t	vm_memory_entry_zone;

void
vm_memory_entry_init(void)
{
	vm_memory_entry_zone = zinit((vm_size_t) sizeof(struct vm_memory_entry),
				     VM_MEMORY_ENTRY_MAX *
				     sizeof(struct vm_memory_entry),
				     PAGE_SIZE, "memory entries");
	zone_change(vm_memory_entry_zone, Z_EXHAUST, TRUE);
}

/*
 * The segments a maker asked for: page multiples, strictly increasing, the
 * last one the size, each protection a set of VM_PROT_ bits the maker's own
 * mapping allows.  Anything else is refused whole.
 */
static kern_return_t
vm_memory_entry_check_segments(
	vm_size_t		size,
	const vm_size_t		*ends,
	const vm_prot_t		*prots,
	natural_t		segments)
{
	vm_size_t	previous = 0;
	unsigned int	i;

	if (segments == 0 || segments > URMACH_MEMORY_ENTRY_SEGMENTS_MAX)
		return KERN_INVALID_ARGUMENT;
	for (i = 0; i < segments; i++) {
		if (!page_aligned(ends[i]) || ends[i] <= previous ||
		    ends[i] > size || (prots[i] & ~VM_PROT_ALL) != 0)
			return KERN_INVALID_ARGUMENT;
		previous = ends[i];
	}
	if (previous != size)
		return KERN_INVALID_ARGUMENT;
	return KERN_SUCCESS;
}

/*
 * The object behind [address, address + size) of `map', made fit to share as
 * vm_remap() makes it when it does not copy (vm_remap_extract()): an object
 * where there was none yet, a shadow where the entry still owed a copy, and
 * the symmetric copy strategy turned to a delayed one -- a symmetric copy
 * assumes the object is its mapping's alone, which a shared one is not.  The
 * range lies in one map entry, and no segment may allow more than that
 * entry's maximum: an entry is a way to share, not to gain.  The object comes
 * back with a reference for the caller.
 */
static kern_return_t
vm_memory_entry_object(
	vm_map_t		map,
	vm_offset_t		address,
	vm_size_t		size,
	const vm_prot_t		*prots,
	natural_t		segments,
	vm_object_t		*objectp,
	vm_offset_t		*offsetp)
{
	vm_map_entry_t	map_entry;
	vm_object_t	object;
	vm_size_t	entry_size;
	unsigned int	i;

	vm_map_lock(map);
	if (!vm_map_lookup_entry(map, address, &map_entry) ||
	    map_entry->is_sub_map ||
	    address + size > map_entry->vme_end) {
		vm_map_unlock(map);
		return KERN_INVALID_ADDRESS;
	}
	for (i = 0; i < segments; i++) {
		if ((prots[i] & ~map_entry->max_protection) != 0) {
			vm_map_unlock(map);
			return KERN_PROTECTION_FAILURE;
		}
	}

	object = map_entry->object.vm_object;
	entry_size = (vm_size_t) (map_entry->vme_end - map_entry->vme_start);
	if (object == VM_OBJECT_NULL) {
		object = vm_object_allocate(entry_size);
		map_entry->offset = 0;
		map_entry->object.vm_object = object;
	} else if (object->copy_strategy != MEMORY_OBJECT_COPY_SYMMETRIC) {
		/* Already copied asymmetrically: the object is the right one. */
		assert(!map_entry->needs_copy);
	} else if (map_entry->needs_copy || object->shadowed ||
		   (object->internal && !map_entry->is_shared &&
		    object->size > entry_size)) {
		vm_object_shadow(&map_entry->object.vm_object,
				 &map_entry->offset, entry_size);
		if (!map_entry->needs_copy &&
		    (map_entry->protection & VM_PROT_WRITE))
			pmap_protect(vm_map_pmap(map), map_entry->vme_start,
				     map_entry->vme_end,
				     map_entry->protection & ~VM_PROT_WRITE);
		object = map_entry->object.vm_object;
		map_entry->needs_copy = FALSE;
	}

	vm_object_lock(object);
	object->ref_count++;		/* the caller's */
	VM_OBJ_RES_INCR(object);
	if (object->copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC)
		object->copy_strategy = MEMORY_OBJECT_COPY_DELAY;
	vm_object_unlock(object);
	map_entry->is_shared = TRUE;

	*objectp = object;
	*offsetp = map_entry->offset + (address - map_entry->vme_start);
	vm_map_unlock(map);
	return KERN_SUCCESS;
}

/*
 *	Routine:	urmach_make_memory_entry [kernel call]
 *	Purpose:
 *		Name [address, address + size) of a task's memory by a port,
 *		cut into segments that end at ends[] and may be mapped with at
 *		most prots[].  Answers a send right to the port.
 *	Returns:
 *		KERN_INVALID_ARGUMENT	no task, a range not page-aligned or
 *					empty or wrapping, bad segments
 *		KERN_INVALID_ADDRESS	the range is not in one mapping
 *		KERN_PROTECTION_FAILURE	a segment allows more than the task's
 *					mapping does
 *		KERN_RESOURCE_SHORTAGE	no entry or no port to be had
 */
kern_return_t
urmach_make_memory_entry(
	vm_map_t			map,
	vm_address_t			address,
	vm_size_t			size,
	urmach_memory_entry_ends_t	ends,
	urmach_memory_entry_prots_t	prots,
	natural_t			segments,
	ipc_port_t			*entryp)
{
	vm_memory_entry_t	me;
	vm_object_t		object;
	vm_offset_t		offset;
	ipc_port_t		port;
	ipc_port_t		notify;
	ipc_port_t		previous;
	kern_return_t		kr;
	unsigned int		i;

	if (map == VM_MAP_NULL)
		return KERN_INVALID_ARGUMENT;
	if (size == 0 || !page_aligned(address) || !page_aligned(size) ||
	    address + size < address)
		return KERN_INVALID_ARGUMENT;
	kr = vm_memory_entry_check_segments(size, ends, prots, segments);
	if (kr != KERN_SUCCESS)
		return kr;

	me = (vm_memory_entry_t) zalloc(vm_memory_entry_zone);
	if (me == VM_MEMORY_ENTRY_NULL)
		return KERN_RESOURCE_SHORTAGE;
	port = ipc_port_alloc_kernel();
	if (port == IP_NULL) {
		zfree(vm_memory_entry_zone, (vm_offset_t) me);
		return KERN_RESOURCE_SHORTAGE;
	}

	kr = vm_memory_entry_object(map, address, size, prots, segments,
				    &object, &offset);
	if (kr != KERN_SUCCESS) {
		ipc_port_dealloc_kernel(port);
		zfree(vm_memory_entry_zone, (vm_offset_t) me);
		return kr;
	}

	me->object = object;
	me->offset = offset;
	me->size = size;
	me->segments = segments;
	for (i = 0; i < segments; i++) {
		me->ends[i] = ends[i];
		me->prots[i] = prots[i];
	}

	ipc_kobject_set(port, (ipc_kobject_t) me, IKOT_MEMORY_ENTRY);
	*entryp = ipc_port_make_send(port);

	/* The entry ends with its last send right. */
	notify = ipc_port_make_sonce(port);
	ip_lock(port);
	ipc_port_nsrequest(port, 1, notify, &previous);
	assert(previous == IP_NULL);

	return KERN_SUCCESS;
}

/*
 * vm_map() with an entry's port where a memory object would be.  The pages
 * are the entry's object's, shared and never copied: a copy of a task's
 * pages is what vm_read() or out-of-line memory gives, and nobody needs one
 * through an entry.  The range asked for lies in the entry, and every piece
 * of it gets the smaller of the protections asked for and its segment's.
 *
 * The port is not consumed here: the send right came in the request, and
 * the request's server stub lets it go.  While it is held the entry cannot
 * end, so it is read without a lock.
 */
kern_return_t
vm_memory_entry_map(
	vm_map_t	map,
	vm_offset_t	*address,
	vm_size_t	size,
	vm_offset_t	mask,
	boolean_t	anywhere,
	ipc_port_t	port,
	vm_offset_t	offset,
	boolean_t	copy,
	vm_prot_t	cur_protection,
	vm_prot_t	max_protection,
	vm_inherit_t	inheritance)
{
	vm_memory_entry_t	me;
	vm_offset_t		start;
	kern_return_t		kr;

	if (copy)
		return KERN_INVALID_ARGUMENT;

	ip_lock(port);
	if (!ip_active(port) || ip_kotype(port) != IKOT_MEMORY_ENTRY) {
		ip_unlock(port);
		return KERN_INVALID_OBJECT;
	}
	me = (vm_memory_entry_t) port->ip_kobject;
	ip_unlock(port);

	size = round_page(size);
	if (size == 0 || !page_aligned(offset) || offset > me->size ||
	    size > me->size - offset)
		return KERN_INVALID_ARGUMENT;

	start = trunc_page(*address);
	vm_object_reference(me->object);	/* consumed by the call */
	kr = vm_map_enter_segments(map, &start, size, mask, anywhere,
				   me->object, me->offset + offset, offset,
				   me->segments, me->ends, me->prots,
				   cur_protection, max_protection, inheritance);
	if (kr == KERN_SUCCESS)
		*address = start;
	return kr;
}

/*
 * The last send right went: the entry lets its object go and its port is
 * destroyed.  Only urmach_make_memory_entry() ever makes a send right, once,
 * so no right can have been made since the notification was sent; if one
 * had, the entry would be freed under its holder, and it is kept instead.
 */
boolean_t
vm_memory_entry_notify(
	ipc_port_t		port,
	mach_msg_header_t	*msg)
{
	vm_memory_entry_t	me;

	if (msg->msgh_id != MACH_NOTIFY_NO_SENDERS) {
		printf("vm_memory_entry_notify: strange notification %d\n",
		       msg->msgh_id);
		return FALSE;
	}

	ip_lock(port);
	if (!ip_active(port) || ip_kotype(port) != IKOT_MEMORY_ENTRY ||
	    port->ip_srights != 0) {
		ip_unlock(port);
		return TRUE;
	}
	me = (vm_memory_entry_t) port->ip_kobject;
	ip_unlock(port);

	ipc_kobject_set(port, IKO_NULL, IKOT_NONE);
	ipc_port_dealloc_kernel(port);
	vm_object_deallocate(me->object);
	zfree(vm_memory_entry_zone, (vm_offset_t) me);
	return TRUE;
}
