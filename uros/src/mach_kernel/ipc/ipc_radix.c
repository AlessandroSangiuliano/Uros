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
 *	File:	ipc/ipc_radix.c
 *
 *	Sparse radix tree (4 x 64-way over a 24-bit index) used as an
 *	ipc_space's overflow store, replacing the splay tree (#331).
 *	See ipc/ipc_radix.h for the rationale.
 */

#include <mach/port.h>
#include <kern/assert.h>
#include <kern/kalloc.h>
#include <ipc/ipc_entry.h>
#include <ipc/ipc_radix.h>

/* slot index for `index' at level `lvl' (lvl 0 = leaf). */
#define	IRT_SLOT(index, lvl)	\
	(((index) >> ((lvl) * IPC_RADIX_BITS)) & IPC_RADIX_MASK)

struct ipc_radix_node *
ipc_radix_node_alloc(void)
{
	struct ipc_radix_node *n;
	int i;

	n = (struct ipc_radix_node *) kalloc(sizeof(struct ipc_radix_node));
	if (n == (struct ipc_radix_node *) 0)
		return n;

	for (i = 0; i < IPC_RADIX_FANOUT; i++)
		n->slots[i] = (void *) 0;
	n->count = 0;
	return n;
}

void
ipc_radix_node_free(struct ipc_radix_node *node)
{
	kfree((vm_offset_t) node, sizeof(struct ipc_radix_node));
}

/* Pop a pre-allocated node off the caller's supply stack (linked via slots[0]). */
static struct ipc_radix_node *
irt_pop(struct ipc_radix_node **supply)
{
	struct ipc_radix_node *n = *supply;

	assert(n != (struct ipc_radix_node *) 0);
	*supply = (struct ipc_radix_node *) n->slots[0];
	n->slots[0] = (void *) 0;
	return n;
}

void
ipc_radix_init(ipc_radix_tree_t tree)
{
	tree->irt_root = (struct ipc_radix_node *) 0;
	tree->irt_count = 0;
}

ipc_tree_entry_t
ipc_radix_lookup(
	ipc_radix_tree_t	tree,
	mach_port_index_t	index)
{
	struct ipc_radix_node *node = tree->irt_root;
	int lvl;

	for (lvl = IPC_RADIX_LEVELS - 1; lvl >= 1; lvl--) {
		if (node == (struct ipc_radix_node *) 0)
			return ITE_NULL;
		node = (struct ipc_radix_node *) node->slots[IRT_SLOT(index, lvl)];
	}
	if (node == (struct ipc_radix_node *) 0)
		return ITE_NULL;

	return (ipc_tree_entry_t) node->slots[IRT_SLOT(index, 0)];
}

void
ipc_radix_insert(
	ipc_radix_tree_t	  tree,
	mach_port_index_t	  index,
	ipc_tree_entry_t	  entry,
	struct ipc_radix_node	**supply)
{
	struct ipc_radix_node *node;
	int lvl, slot;

	assert(entry != ITE_NULL);

	if (tree->irt_root == (struct ipc_radix_node *) 0)
		tree->irt_root = irt_pop(supply);
	node = tree->irt_root;

	/* Descend the internal levels, growing the path as needed. */
	for (lvl = IPC_RADIX_LEVELS - 1; lvl >= 1; lvl--) {
		slot = IRT_SLOT(index, lvl);
		if (node->slots[slot] == (void *) 0) {
			node->slots[slot] = (void *) irt_pop(supply);
			node->count++;
		}
		node = (struct ipc_radix_node *) node->slots[slot];
	}

	/* Leaf: the caller guarantees the name isn't already present. */
	slot = IRT_SLOT(index, 0);
	assert(node->slots[slot] == (void *) 0);
	node->slots[slot] = (void *) entry;
	node->count++;
	tree->irt_count++;
}

ipc_tree_entry_t
ipc_radix_delete(
	ipc_radix_tree_t	tree,
	mach_port_index_t	index)
{
	struct ipc_radix_node *path[IPC_RADIX_LEVELS];	/* path[lvl] = node at lvl */
	int slot[IPC_RADIX_LEVELS];			/* slot[lvl] in path[lvl]  */
	struct ipc_radix_node *node = tree->irt_root;
	ipc_tree_entry_t entry;
	int lvl, leaf_slot;

	if (node == (struct ipc_radix_node *) 0)
		return ITE_NULL;

	for (lvl = IPC_RADIX_LEVELS - 1; lvl >= 1; lvl--) {
		path[lvl] = node;
		slot[lvl] = IRT_SLOT(index, lvl);
		node = (struct ipc_radix_node *) node->slots[slot[lvl]];
		if (node == (struct ipc_radix_node *) 0)
			return ITE_NULL;
	}
	path[0] = node;					/* leaf */

	leaf_slot = IRT_SLOT(index, 0);
	entry = (ipc_tree_entry_t) node->slots[leaf_slot];
	if (entry == ITE_NULL)
		return ITE_NULL;

	node->slots[leaf_slot] = (void *) 0;
	node->count--;
	tree->irt_count--;

	/* Prune nodes that just emptied, bottom-up. */
	for (lvl = 0; lvl < IPC_RADIX_LEVELS - 1; lvl++) {
		if (path[lvl]->count != 0)
			break;
		ipc_radix_node_free(path[lvl]);
		path[lvl + 1]->slots[slot[lvl + 1]] = (void *) 0;
		path[lvl + 1]->count--;
	}

	if (tree->irt_root->count == 0) {
		ipc_radix_node_free(tree->irt_root);
		tree->irt_root = (struct ipc_radix_node *) 0;
	}

	return entry;
}

static void
irt_iterate_node(
	struct ipc_radix_node	*node,
	int			 lvl,
	mach_port_index_t	 prefix,
	void			(*fn)(mach_port_index_t, ipc_tree_entry_t, void *),
	void			*arg)
{
	int s;

	if (lvl == 0) {
		for (s = 0; s < IPC_RADIX_FANOUT; s++)
			if (node->slots[s] != (void *) 0)
				(*fn)(prefix | s,
				      (ipc_tree_entry_t) node->slots[s], arg);
		return;
	}

	for (s = 0; s < IPC_RADIX_FANOUT; s++)
		if (node->slots[s] != (void *) 0)
			irt_iterate_node((struct ipc_radix_node *) node->slots[s],
					 lvl - 1,
					 prefix | (((mach_port_index_t) s)
						   << (lvl * IPC_RADIX_BITS)),
					 fn, arg);
}

void
ipc_radix_iterate(
	ipc_radix_tree_t	tree,
	void			(*fn)(mach_port_index_t, ipc_tree_entry_t, void *),
	void			*arg)
{
	if (tree->irt_root != (struct ipc_radix_node *) 0)
		irt_iterate_node(tree->irt_root, IPC_RADIX_LEVELS - 1, 0, fn, arg);
}

static void
irt_destroy_node(
	struct ipc_radix_node	*node,
	int			 lvl,
	mach_port_index_t	 prefix,
	void			(*fn)(mach_port_index_t, ipc_tree_entry_t, void *),
	void			*arg)
{
	int s;

	if (lvl == 0) {
		if (fn != (void (*)(mach_port_index_t, ipc_tree_entry_t,
				    void *)) 0)
			for (s = 0; s < IPC_RADIX_FANOUT; s++)
				if (node->slots[s] != (void *) 0)
					(*fn)(prefix | s,
					      (ipc_tree_entry_t) node->slots[s],
					      arg);
	} else {
		for (s = 0; s < IPC_RADIX_FANOUT; s++)
			if (node->slots[s] != (void *) 0)
				irt_destroy_node(
					(struct ipc_radix_node *) node->slots[s],
					lvl - 1,
					prefix | (((mach_port_index_t) s)
						  << (lvl * IPC_RADIX_BITS)),
					fn, arg);
	}
	ipc_radix_node_free(node);
}

void
ipc_radix_destroy(
	ipc_radix_tree_t	tree,
	void			(*fn)(mach_port_index_t, ipc_tree_entry_t, void *),
	void			*arg)
{
	if (tree->irt_root != (struct ipc_radix_node *) 0)
		irt_destroy_node(tree->irt_root, IPC_RADIX_LEVELS - 1, 0,
				 fn, arg);
	tree->irt_root = (struct ipc_radix_node *) 0;
	tree->irt_count = 0;
}
