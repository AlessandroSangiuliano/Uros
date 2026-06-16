/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */
/*
 * MkLinux
 */
/* CMU_HIST */
/*
 * Revision 2.7  91/10/09  16:08:15  af
 * 	 Revision 2.6.2.1  91/09/16  10:15:30  rpd
 * 	 	Added <ipc/ipc_hash.h>.
 * 	 	[91/09/02            rpd]
 * 
 * Revision 2.6.2.1  91/09/16  10:15:30  rpd
 * 	Added <ipc/ipc_hash.h>.
 * 	[91/09/02            rpd]
 * 
 * Revision 2.6  91/05/14  16:31:38  mrt
 * 	Correcting copyright
 * 
 * Revision 2.5  91/03/16  14:47:45  rpd
 * 	Fixed ipc_entry_grow_table to use it_entries_realloc.
 * 	[91/03/05            rpd]
 * 
 * Revision 2.4  91/02/05  17:21:17  mrt
 * 	Changed to new Mach copyright
 * 	[91/02/01  15:44:19  mrt]
 * 
 * Revision 2.3  91/01/08  15:12:58  rpd
 * 	Removed MACH_IPC_GENNOS.
 * 	[90/11/08            rpd]
 * 
 * Revision 2.2  90/06/02  14:49:36  rpd
 * 	Created for new IPC.
 * 	[90/03/26  20:54:27  rpd]
 * 
 */
/* CMU_ENDHIST */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */
/*
 *	File:	ipc/ipc_entry.c
 *	Author:	Rich Draves
 *	Date:	1989
 *
 *	Primitive functions to manipulate translation entries.
 */

#include <mach_kdb.h>
#include <mach_debug.h>

#include <mach/kern_return.h>
#include <mach/port.h>
#include <kern/assert.h>
#include <kern/sched_prim.h>
#include <kern/zalloc.h>
#include <kern/misc_protos.h>
#include <ipc/port.h>
#include <ipc/ipc_entry.h>
#include <ipc/ipc_space.h>
#include <ipc/ipc_radix.h>
#include <ipc/ipc_object.h>
#include <ipc/ipc_hash.h>
#include <ipc/ipc_table.h>
#include <ipc/ipc_port.h>
#include <string.h>

zone_t ipc_tree_entry_zone;

/*
 *	#331: collisions (two live names sharing one index) are now rejected
 *	at allocation with KERN_NAME_EXISTS, so the overflow store holds only
 *	sparse entries (index >= is_table_size) and never needs a "does name
 *	collide with a tree entry?" test -- ipc_entry_tree_collision is gone.
 */

/*
 *	Routine:	ipc_entry_lookup
 *	Purpose:
 *		Searches for an entry, given its name.
 *	Conditions:
 *		The space must be read or write locked throughout.
 *		The space must be active.
 */

ipc_entry_t
ipc_entry_lookup(
	ipc_space_t	space,
	mach_port_t	name)
{
	mach_port_index_t index;
	ipc_entry_t entry;

	assert(space->is_active);

			
	index = MACH_PORT_INDEX(name);
	/*
	 * If space is fast, we assume no splay tree and name within table
	 * bounds, but still check generation numbers (if enabled) and
	 * look for null entries.
	 */
	if (is_fast_space(space)) {
		entry = &space->is_table[index];
		if (IE_BITS_GEN(entry->ie_bits) != MACH_PORT_GEN(name) ||
			IE_BITS_TYPE(entry->ie_bits) == MACH_PORT_TYPE_NONE)
			entry = IE_NULL;
	}
	else {
		if (index < space->is_table_size) {
			entry = &space->is_table[index];
			if (IE_BITS_GEN(entry->ie_bits) != MACH_PORT_GEN(name) ||
			    IE_BITS_TYPE(entry->ie_bits) == MACH_PORT_TYPE_NONE)
				entry = IE_NULL;
		} else if (space->is_tree_total == 0)
			entry = IE_NULL;
		else {
			/*
			 * #331: a sparse name lives in the radix overflow.  The
			 * radix is keyed by index alone (one live entry per
			 * index), so confirm the full name -- a stale
			 * generation is not this entry.
			 */
			ipc_tree_entry_t tentry =
				ipc_radix_lookup(&space->is_tree, index);

			if (tentry != ITE_NULL && tentry->ite_name == name)
				entry = (ipc_entry_t) tentry;
			else
				entry = IE_NULL;
		}
	}

	assert((entry == IE_NULL) || IE_BITS_TYPE(entry->ie_bits));
	return entry;
}

/*
 *	Routine:	ipc_entry_get
 *	Purpose:
 *		Tries to allocate an entry out of the space.
 *	Conditions:
 *		The space is write-locked and active throughout.
 *		An object may be locked.  Will not allocate memory.
 *	Returns:
 *		KERN_SUCCESS		A free entry was found.
 *		KERN_NO_SPACE		No entry allocated.
 */

kern_return_t
ipc_entry_get(
	ipc_space_t	space,
	boolean_t	is_send_once,
	mach_port_t	*namep,
	ipc_entry_t	*entryp)
{
	ipc_entry_t table;
	mach_port_index_t first_free;
	ipc_entry_t free_entry;

	assert(space->is_active);

	{
		table = space->is_table;
		first_free = table->ie_next;

		if (first_free == 0)
			return KERN_NO_SPACE;

		free_entry = &table[first_free];
		table->ie_next = free_entry->ie_next;
	}

	/*
	 *	Initialize the new entry.  We need only
	 *	increment the generation number and clear ie_request.
	 */

    {
	mach_port_t new_name;
	mach_port_gen_t gen;

	assert((free_entry->ie_bits &~ IE_BITS_GEN_MASK) == 0);
	gen = IE_BITS_NEW_GEN(free_entry->ie_bits);
	free_entry->ie_bits = gen;
	free_entry->ie_request = 0;
	/*
	 *	The new name can't be MACH_PORT_NULL because index
	 *	is non-zero.  It can't be MACH_PORT_DEAD because
	 *	the table isn't allowed to grow big enough.
	 *	(See comment in ipc/ipc_table.h.)
	 */
	new_name = MACH_PORT_MAKE(first_free, gen);
	assert(MACH_PORT_VALID(new_name));
	*namep = new_name;
    }

	assert(free_entry->ie_object == IO_NULL);

	*entryp = free_entry;
	return KERN_SUCCESS;
}

/*
 *	Routine:	ipc_entry_alloc
 *	Purpose:
 *		Allocate an entry out of the space.
 *	Conditions:
 *		The space is not locked before, but it is write-locked after
 *		if the call is successful.  May allocate memory.
 *	Returns:
 *		KERN_SUCCESS		An entry was allocated.
 *		KERN_INVALID_TASK	The space is dead.
 *		KERN_NO_SPACE		No room for an entry in the space.
 *		KERN_RESOURCE_SHORTAGE	Couldn't allocate memory for an entry.
 */

kern_return_t
ipc_entry_alloc(
	ipc_space_t	space,
	boolean_t	is_send_once,
	mach_port_t	*namep,
	ipc_entry_t	*entryp)
{
	kern_return_t kr;

	is_write_lock(space);

	for (;;) {
		if (!space->is_active) {
			is_write_unlock(space);
			return KERN_INVALID_TASK;
		}

		kr = ipc_entry_get(space, is_send_once, namep, entryp);
		if (kr == KERN_SUCCESS)
			return kr;

		kr = ipc_entry_grow_table(space, ITS_SIZE_NONE);
		if (kr != KERN_SUCCESS)
			return kr; /* space is unlocked */
	}
}

/*
 *	Routine:	ipc_entry_name_prealloc_free
 *	Purpose:
 *		Free a tree entry and/or radix node supply that
 *		ipc_entry_alloc_name() pre-allocated outside the space lock
 *		but did not end up consuming.
 */

static void
ipc_entry_name_prealloc_free(
	ipc_tree_entry_t	tree_entry,
	struct ipc_radix_node	*supply)
{
	if (tree_entry != ITE_NULL)
		ite_free(tree_entry);

	while (supply != (struct ipc_radix_node *) 0) {
		struct ipc_radix_node *n = supply;

		supply = (struct ipc_radix_node *) n->slots[0];
		ipc_radix_node_free(n);
	}
}

/*
 *	Routine:	ipc_entry_alloc_name
 *	Purpose:
 *		Allocates/finds an entry with a specific name.
 *		If an existing entry is returned, its type will be nonzero.
 *	Conditions:
 *		The space is not locked before, but it is write-locked after
 *		if the call is successful.  May allocate memory.
 *	Returns:
 *		KERN_SUCCESS		Found existing entry with same name.
 *		KERN_SUCCESS		Allocated a new entry.
 *		KERN_NAME_EXISTS	#331: the index is already in use by a
 *					different generation (no overflow displace).
 *		KERN_INVALID_TASK	The space is dead.
 *		KERN_RESOURCE_SHORTAGE	Couldn't allocate memory.
 */

kern_return_t
ipc_entry_alloc_name(
	ipc_space_t	space,
	mach_port_t	name,
	ipc_entry_t	*entryp)
{
	mach_port_index_t index = MACH_PORT_INDEX(name);
	mach_port_gen_t gen = MACH_PORT_GEN(name);
	ipc_tree_entry_t tree_entry = ITE_NULL;
	struct ipc_radix_node *supply = (struct ipc_radix_node *) 0;

	assert(MACH_PORT_VALID(name));


	is_write_lock(space);

	for (;;) {
		ipc_entry_t entry;
		ipc_tree_entry_t tentry;

		if (!space->is_active) {
			is_write_unlock(space);
			ipc_entry_name_prealloc_free(tree_entry, supply);
			return KERN_INVALID_TASK;
		}

		/*
		 *	Under the table cutoff there are three cases:
		 *		1) inuse, same name        -> return it
		 *		2) inuse, different gen    -> KERN_NAME_EXISTS (#331)
		 *		3) free                    -> take the slot
		 *	A "fast" space disallows case 3 (ports can't be renamed).
		 */

		if ((0 < index) && (index < space->is_table_size)) {
			ipc_entry_t table = space->is_table;

			entry = &table[index];

			if (IE_BITS_TYPE(entry->ie_bits)) {
				if (IE_BITS_GEN(entry->ie_bits) == gen) {
					/* case 1: same name already present */
					*entryp = entry;
					ipc_entry_name_prealloc_free(tree_entry,
								     supply);
					return KERN_SUCCESS;
				}

				/*
				 * case 2 (#331): the index is live under a
				 * different generation.  We no longer displace
				 * it into an overflow tree -- the name is taken.
				 */
				is_write_unlock(space);
				ipc_entry_name_prealloc_free(tree_entry, supply);
				return KERN_NAME_EXISTS;
			} else {
				mach_port_index_t free_index, next_index;

				/*
				 *	case 3: rip the entry out of the free list.
				 */

				for (free_index = 0;
				     (next_index = table[free_index].ie_next)
							!= index;
				     free_index = next_index)
					continue;

				table[free_index].ie_next =
					table[next_index].ie_next;

				entry->ie_bits = gen;
				assert(entry->ie_object == IO_NULL);
				entry->ie_request = 0;

				*entryp = entry;
				ipc_entry_name_prealloc_free(tree_entry, supply);
				return KERN_SUCCESS;
			}
		}

		/*
		 * In a fast space, ipc_entry_alloc_name may be
		 * used only to add a right to a port name already
		 * known in this space.
		 */
		if (is_fast_space(space)) {
			is_write_unlock(space);
			ipc_entry_name_prealloc_free(tree_entry, supply);
			return KERN_FAILURE;
		}

		/*
		 *	#331: index >= is_table_size -- a sparse name living in
		 *	the radix overflow.  If something already occupies the
		 *	index it is either the same name (return it) or a
		 *	different generation (rejected, same as the table case).
		 */

		if (space->is_tree_total > 0) {
			tentry = ipc_radix_lookup(&space->is_tree, index);
			if (tentry != ITE_NULL) {
				if (tentry->ite_name == name) {
					assert(tentry->ite_space == space);
					assert(IE_BITS_TYPE(tentry->ite_bits));
					*entryp = &tentry->ite_entry;
					ipc_entry_name_prealloc_free(tree_entry,
								     supply);
					return KERN_SUCCESS;
				}

				is_write_unlock(space);
				ipc_entry_name_prealloc_free(tree_entry, supply);
				return KERN_NAME_EXISTS;
			}
		}

		/*
		 *	Not present.  If we have pre-allocated the tree entry and
		 *	a worst-case radix node supply, insert now; otherwise drop
		 *	the lock, pre-allocate, and retry (mirrors the old splay
		 *	ite_alloc-outside-the-lock dance).
		 */

		if (tree_entry != ITE_NULL) {
			tree_entry->ite_bits = 0;
			tree_entry->ite_object = IO_NULL;
			tree_entry->ite_request = 0;
			tree_entry->ite_name = name;
			tree_entry->ite_space = space;

			ipc_radix_insert(&space->is_tree, index, tree_entry,
					 &supply);
			space->is_tree_total++;

			*entryp = &tree_entry->ite_entry;

			/* the radix kept what it needed; free any spare nodes. */
			ipc_entry_name_prealloc_free(ITE_NULL, supply);
			return KERN_SUCCESS;
		}

		/*
		 *	Allocate the tree entry plus a worst-case radix node
		 *	supply (one fresh node per level) outside the lock, then
		 *	retry.
		 */

		is_write_unlock(space);
		tree_entry = ite_alloc();
		if (tree_entry == ITE_NULL)
			return KERN_RESOURCE_SHORTAGE;
		{
			int i;

			for (i = 0; i < IPC_RADIX_LEVELS; i++) {
				struct ipc_radix_node *n = ipc_radix_node_alloc();

				if (n == (struct ipc_radix_node *) 0) {
					ipc_entry_name_prealloc_free(tree_entry,
								     supply);
					return KERN_RESOURCE_SHORTAGE;
				}
				n->slots[0] = (void *) supply;
				supply = n;
			}
		}
		is_write_lock(space);
	}
}

/*
 *	Routine:	ipc_entry_dealloc
 *	Purpose:
 *		Deallocates an entry from a space.
 *	Conditions:
 *		The space must be write-locked throughout.
 *		The space must be active.
 */

void
ipc_entry_dealloc(
	ipc_space_t	space,
	mach_port_t	name,
	ipc_entry_t	entry)
{
	ipc_entry_t table;
	ipc_entry_num_t size;
	mach_port_index_t index;

	assert(space->is_active);
	assert(entry->ie_object == IO_NULL);
	assert(entry->ie_request == 0);


	index = MACH_PORT_INDEX(name);
	table = space->is_table;
	size = space->is_table_size;


	if (is_fast_space(space)) {
		assert(index < size);
		assert(entry == &table[index]);
		assert(IE_BITS_GEN(entry->ie_bits) == MACH_PORT_GEN(name));
		assert(!(entry->ie_bits & IE_BITS_COLLISION));
		entry->ie_bits &= IE_BITS_GEN_MASK;
		entry->ie_next = table->ie_next;
		table->ie_next = index;
		return;
	}


	if ((index < size) && (entry == &table[index])) {
		assert(IE_BITS_GEN(entry->ie_bits) == MACH_PORT_GEN(name));

		/*
		 * #331: one live entry per index, so freeing a table slot no
		 * longer promotes a colliding entry up from an overflow tree.
		 */
		entry->ie_bits &= IE_BITS_GEN_MASK;
		entry->ie_next = table->ie_next;
		table->ie_next = index;
	} else {
		/* A sparse name: the entry lives in the radix overflow. */
		ipc_tree_entry_t tentry = (ipc_tree_entry_t) entry;
		ipc_tree_entry_t deleted;

		assert(tentry->ite_space == space);

		deleted = ipc_radix_delete(&space->is_tree, index);
		assert(deleted == tentry);

		assert(space->is_tree_total > 0);
		space->is_tree_total--;

		ite_free(tentry);
	}
}

/*
 *	Routine:	ipc_entry_grow_table
 *	Purpose:
 *		Grows the table in a space.
 *	Conditions:
 *		The space must be write-locked and active before.
 *		If successful, it is also returned locked.
 *		Allocates memory.
 *	Returns:
 *		KERN_SUCCESS		Grew the table.
 *		KERN_SUCCESS		Somebody else grew the table.
 *		KERN_SUCCESS		The space died.
 *		KERN_NO_SPACE		Table has maximum size already.
 *		KERN_RESOURCE_SHORTAGE	Couldn't allocate a new table.
 */

kern_return_t
ipc_entry_grow_table(
	ipc_space_t	space,
	int		target_size)
{
	ipc_entry_num_t osize, size, nsize, psize;

	do {
		ipc_entry_t otable, table;
		ipc_table_size_t oits = NULL, its, nits;
		mach_port_index_t i, free_index;

		assert(space->is_active);

		if (space->is_growing) {
			/*
			 *	Somebody else is growing the table.
			 *	We just wait for them to finish.
			 */

			assert_wait((event_t) space, FALSE);
			is_write_unlock(space);
			thread_block((void (*)(void)) 0);
			is_write_lock(space);
			return KERN_SUCCESS;
		}

		otable = space->is_table;
		
		its = space->is_table_next;
		size = its->its_size;
		
		/*
		 * Since is_table_next points to the next natural size
		 * we can identify the current size entry.
		 */
		oits = its - 1;
		osize = oits->its_size;
		
		/*
		 * If there is no target size, then the new size is simply
		 * specified by is_table_next.  If there is a target
		 * size, then search for the next entry.
		 */
		if (target_size != ITS_SIZE_NONE) {
			if (target_size <= osize) {
				is_write_unlock(space);
				return KERN_SUCCESS;
			}

			psize = osize;
			while ((psize != size) && (target_size > size)) {
				psize = size;
				its++;
				size = its->its_size;
			}
			if (psize == size) {
				is_write_unlock(space);
				return KERN_NO_SPACE;
			}
		}
		nits = its + 1;
		nsize = nits->its_size;

		if (osize == size) {
			is_write_unlock(space);
			return KERN_NO_SPACE;
		}

		assert((osize < size) && (size <= nsize));

		/*
		 *	OK, we'll attempt to grow the table.
		 *	The realloc requires that the old table
		 *	remain in existence.
		 */

		space->is_growing = TRUE;
		is_write_unlock(space);
		if (it_entries_reallocable(oits))
			table = it_entries_realloc(oits, otable, its);
		else
			table = it_entries_alloc(its);
		is_write_lock(space);
		space->is_growing = FALSE;

		/*
		 *	We need to do a wakeup on the space,
		 *	to rouse waiting threads.  We defer
		 *	this until the space is unlocked,
		 *	because we don't want them to spin.
		 */

		if (table == IE_NULL) {
			is_write_unlock(space);
			thread_wakeup((event_t) space);
			return KERN_RESOURCE_SHORTAGE;
		}

		if (!space->is_active) {
			/*
			 *	The space died while it was unlocked.
			 */

			is_write_unlock(space);
			thread_wakeup((event_t) space);
			it_entries_free(its, table);
			is_write_lock(space);
			return KERN_SUCCESS;
		}

		assert(space->is_table == otable);
		assert((space->is_table_next == its) ||
		       (target_size != ITS_SIZE_NONE));
		assert(space->is_table_size == osize);

		space->is_table = table;
		space->is_table_size = size;
		space->is_table_next = nits;

		/*
		 *	If we did a realloc, it remapped the data.
		 *	Otherwise we copy by hand first.  Then we have
		 *	to clear the index fields in the old part and
		 *	zero the new part.
		 */

		if (!it_entries_reallocable(oits))
			(void) memcpy((void *) table, (const void *) otable,
			      osize * sizeof(struct ipc_entry));

		for (i = 0; i < osize; i++)
			table[i].ie_index = 0;

		(void) memset((void *) (table + osize), 0,
		      (size - osize) * sizeof(struct ipc_entry));

		/*
		 *	Put old entries into the reverse hash table.
		 */

		for (i = 0; i < osize; i++) {
			ipc_entry_t entry = &table[i];

			if (IE_BITS_TYPE(entry->ie_bits) ==
						MACH_PORT_TYPE_SEND)
				ipc_hash_local_insert(space, entry->ie_object,
						      i, entry);
		}

		/*
		 *	#331: entries whose index now fits in the grown table,
		 *	i.e. osize <= index < size, move from the radix overflow
		 *	into the table; sparser entries stay put.  With one live
		 *	entry per index there are no collisions to juggle.
		 */

		assert(!is_fast_space(space) || space->is_tree_total == 0);
		if (space->is_tree_total > 0) {
			mach_port_index_t index;

			for (index = osize; index < size; index++) {
				ipc_tree_entry_t tentry;
				ipc_entry_t entry;
				ipc_entry_bits_t bits;
				mach_port_type_t type;
				ipc_object_t obj;

				tentry = ipc_radix_lookup(&space->is_tree, index);
				if (tentry == ITE_NULL)
					continue;

				assert(tentry->ite_space == space);
				assert(MACH_PORT_INDEX(tentry->ite_name) == index);

				entry = &table[index];
				assert(entry->ie_bits == 0);	/* a fresh slot */

				bits = tentry->ite_bits;
				type = IE_BITS_TYPE(bits);
				assert(type != MACH_PORT_TYPE_NONE);

				entry->ie_bits = bits | MACH_PORT_GEN(tentry->ite_name);
				entry->ie_object = obj = tentry->ite_object;
				entry->ie_request = tentry->ite_request;

				if (type == MACH_PORT_TYPE_SEND) {
					ipc_hash_global_delete(space, obj,
						       tentry->ite_name, tentry);
					ipc_hash_local_insert(space, obj,
						      index, entry);
				}

				(void) ipc_radix_delete(&space->is_tree, index);
				ite_free(tentry);
				space->is_tree_total--;
			}
		}

		/*
		 *	Add entries in the new part which still aren't used
		 *	to the free list.  Add them in reverse order,
		 *	and set the generation number to -1, so that
		 *	early allocations produce "natural" names.
		 */

		free_index = table[0].ie_next;
		for (i = size-1; i >= osize; --i) {
			ipc_entry_t entry = &table[i];

			if (entry->ie_bits == 0) {
				entry->ie_bits = IE_BITS_GEN_MASK;
				entry->ie_next = free_index;
				free_index = i;
			}
		}
		table[0].ie_next = free_index;

		/*
		 *	Now we need to free the old table.
		 *	If the space dies or grows while unlocked,
		 *	then we can quit here.
		 */

		is_write_unlock(space);
		thread_wakeup((event_t) space);
		it_entries_free(oits, otable);
		is_write_lock(space);
		if (!space->is_active || (space->is_table_next != nits))
			return KERN_SUCCESS;

		/*
		 *	#331: sparse names live in the radix without forcing the
		 *	table to grow, so there is no "grow again to absorb the
		 *	tree" pass -- a single growth is enough.
		 */
	} while (0);

	return KERN_SUCCESS;
}


#if	MACH_KDB
#include <ddb/db_output.h>
#define	printf	kdbprintf

ipc_entry_t	db_ipc_object_by_name(
			task_t		task,
			mach_port_t	name);


ipc_entry_t
db_ipc_object_by_name(
	task_t		task,
	mach_port_t	name)
{
        ipc_space_t space = task->itk_space;
        ipc_entry_t entry;
 
 
        entry = ipc_entry_lookup(space, name);
        if(entry != IE_NULL) {
                iprintf("(task 0x%x, name 0x%x) ==> object 0x%x\n",
			task, name, entry->ie_object);
                return (ipc_entry_t) entry->ie_object;
        }
        return entry;
}
#endif	/* MACH_KDB */
