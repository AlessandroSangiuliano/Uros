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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY.
 */
/*
 *	File:	ipc/ipc_radix.h
 *
 *	A sparse radix tree mapping a 24-bit port-name index to an
 *	ipc_tree_entry, replacing the self-adjusting splay tree (#331).
 *	It is the *overflow* store for an ipc_space: entries whose index
 *	does not fit in the dense is_table live here.
 *
 *	Unlike the splay tree it does not restructure on lookup, so a read
 *	never mutates it -- the prerequisite for letting lookups eventually
 *	run lockless (a later step of #331).  For now every operation runs
 *	under the ipc_space lock (read for lookup/iterate, write for
 *	insert/delete), exactly like the splay tree it replaces.
 *
 *	Layout: 4 levels of 64-way (6-bit) nodes cover the full 24-bit index
 *	space.  Only populated branches are allocated, so a handful of sparse
 *	entries cost a handful of nodes, not a 16M-slot array.
 */

#ifndef	_IPC_IPC_RADIX_H_
#define	_IPC_IPC_RADIX_H_

#include <mach/port.h>
#include <mach/boolean.h>
#include <ipc/ipc_entry.h>

#define	IPC_RADIX_BITS		6				/* bits per level   */
#define	IPC_RADIX_FANOUT	(1 << IPC_RADIX_BITS)		/* 64 slots/node    */
#define	IPC_RADIX_MASK		(IPC_RADIX_FANOUT - 1)
#define	IPC_RADIX_LEVELS	4				/* 4 * 6 = 24 bits  */
#define	IPC_RADIX_MAX_INDEX	(1U << (IPC_RADIX_BITS * IPC_RADIX_LEVELS))

/*
 *	An internal node holds child pointers; a leaf node holds
 *	ipc_tree_entry_t values.  Both share this struct -- the level tells
 *	the code which a given node is.  `count' is the number of non-NULL
 *	slots, used to prune a node once it empties.
 *
 *	slots[0] doubles as the link field while a node sits on a caller's
 *	pre-allocation freelist (see ipc_radix_insert).
 */
struct ipc_radix_node {
	void		*slots[IPC_RADIX_FANOUT];
	unsigned int	count;
};

typedef struct ipc_radix_tree {
	struct ipc_radix_node	*irt_root;
	unsigned int		 irt_count;	/* total entries in the tree */
} *ipc_radix_tree_t;

/* Initialize an empty tree (embedded by value in struct ipc_space). */
extern void		ipc_radix_init(
				ipc_radix_tree_t	tree);

/* Find the entry for `index', or ITE_NULL.  Read-only (no restructuring). */
extern ipc_tree_entry_t	ipc_radix_lookup(
				ipc_radix_tree_t	tree,
				mach_port_index_t	index);

/*
 *	Insert `entry' at `index'.  The path may need up to IPC_RADIX_LEVELS
 *	fresh nodes; the caller supplies them on `*supply' (a stack linked
 *	through slots[0]) allocated *outside* the space lock, mirroring the
 *	splay tree's ite_alloc discipline.  Consumed nodes are popped off
 *	`*supply'; the caller frees whatever is left.  Never allocates.
 */
extern void		ipc_radix_insert(
				ipc_radix_tree_t	 tree,
				mach_port_index_t	 index,
				ipc_tree_entry_t	 entry,
				struct ipc_radix_node	**supply);

/*
 *	Remove and return the entry at `index' (or ITE_NULL if absent).
 *	Frees any nodes it empties (zfree/kfree never blocks, so this is
 *	safe under the space write lock).
 */
extern ipc_tree_entry_t	ipc_radix_delete(
				ipc_radix_tree_t	tree,
				mach_port_index_t	index);

/*
 *	Walk every entry in ascending index order, calling fn(index, entry,
 *	arg).  Read-only; used by mach_port_names / mach_port_space_info /
 *	ipc_space teardown.
 */
extern void		ipc_radix_iterate(
				ipc_radix_tree_t	tree,
				void			(*fn)(mach_port_index_t,
						              ipc_tree_entry_t,
						              void *),
				void			*arg);

/*
 *	Tear the whole tree down: call fn(index, entry, arg) for every entry
 *	(may be NULL) and free every node, leaving the tree empty.  Used by
 *	ipc_space teardown to clean each right before discarding its node.
 */
extern void		ipc_radix_destroy(
				ipc_radix_tree_t	tree,
				void			(*fn)(mach_port_index_t,
						              ipc_tree_entry_t,
						              void *),
				void			*arg);

/* Node supply helpers (call the alloc one *outside* the space lock). */
extern struct ipc_radix_node *ipc_radix_node_alloc(void);
extern void		ipc_radix_node_free(struct ipc_radix_node *node);

#endif	/* _IPC_IPC_RADIX_H_ */
