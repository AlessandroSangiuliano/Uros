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
 * The port a server learns this task's death by (#911).
 *
 * A server that keeps something for a client -- an open file, a lock --
 * needs to know when the client dies, to give it back.  ext_server learnt it
 * from the client's task port, which libvfs sent with every fs_open(): a send
 * right to a task port turns into a dead name when the task dies.  But a send
 * right to a task port is also the task: its memory, its threads, its port
 * rights.  Every filesystem server held all of every client that had a file
 * open through it.
 *
 * 🔑 This port gives the death and nothing else.  The task holds the receive
 * right and sends make-send rights from it; when the task dies the receive
 * right dies with it, and every send right a server holds turns into a dead
 * name, as the task port's did.  A send right to it lets its holder send it
 * messages nobody reads, and that is all.
 *
 * ⚠️ NOT urmach_netname_signature().  That port is an authority: the name
 * server lets whoever presents it check out the names it signed (#744).  A
 * filesystem server handed it could take its clients' names.  Two ports, then,
 * one per meaning.
 *
 * One per task, made the first time it is asked for.  Two threads asking at once
 * both make one, and the one that loses the publication destroys its own.  A
 * child made with task_create(inherit_memory) inherits the name and not the
 * right, so mach_task_self_init(), which the child calls before its first RPC,
 * forgets it -- as it forgets the signature.
 */

#include <mach.h>
#include "externs.h"

static mach_port_t	lifetime_port = MACH_PORT_NULL;

kern_return_t
urmach_lifetime_port(mach_port_t *port)
{
	mach_port_t	mine, published;
	kern_return_t	kr;

	published = __atomic_load_n(&lifetime_port, __ATOMIC_ACQUIRE);
	if (published != MACH_PORT_NULL) {
		*port = published;
		return KERN_SUCCESS;
	}

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&mine);
	if (kr != KERN_SUCCESS)
		return kr;

	published = MACH_PORT_NULL;
	if (!__atomic_compare_exchange_n(&lifetime_port, &published, mine,
					 0, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE)) {
		(void) mach_port_destroy(mach_task_self(), mine);
		mine = published;
	}
	*port = mine;
	return KERN_SUCCESS;
}

void
urmach_lifetime_port_forget(void)
{
	__atomic_store_n(&lifetime_port, MACH_PORT_NULL, __ATOMIC_RELEASE);
}
