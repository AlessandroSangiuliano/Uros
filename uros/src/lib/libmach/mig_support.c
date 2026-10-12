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
 * MkLinux
 */
/*
 *  Abstract:
 *	Routines to set and deallocate the mig reply port.
 *	They are called from mig generated interfaces.
 *
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include "externs.h"

/*
 * The reply port, and errno beside it, are kept here and nowhere else (#645).
 *
 * They are the task's until a thread library hands over per-thread storage:
 * mig_init() is given the function that finds the calling thread's
 * mach_thread_state, and from then on every thread has its own.  libpthreads
 * does it in pthread_create(), before a second thread can run, and not in
 * pthread_init(), where pthread_self() is not yet the main thread, which still
 * runs on crt0's stack.  mig_init(0) gives the state back to the task:
 * mach_init() at start, and mach_task_self_init() in a child, whose threads are
 * not its parent's; the child's first pthread_create() hands over again.
 *
 * Each thread needs its own reply port because two threads receiving on one
 * port take each other's replies, and the caller whose reply was taken waits
 * for good (#299: gpu_server's render worker).  These functions used to exist
 * twice: here, weak, with one port for the task, and in libpthreads, strong,
 * with a port per thread.  The linker chose which copy a program got, and the
 * reset of a forked child cleared this copy only, so a child of a program with
 * the other kept its parent's reply-port name (#645).
 */
static struct mach_thread_state	task_state = { MACH_PORT_NULL, 0 };
static struct mach_thread_state	*(*thread_state)(void);

struct mach_thread_state *
mach_thread_state(void)
{
	return (thread_state != 0) ? (*thread_state)() : &task_state;
}

void
mig_init(
	struct mach_thread_state	*(*per_thread)(void))
{
	if (per_thread == 0) {
		task_state.reply_port = MACH_PORT_NULL;
		thread_state = 0;
		return;
	}
	if (thread_state == per_thread)
		return;
	/* The calling thread keeps the port and the errno it was using. */
	*(*per_thread)() = task_state;
	task_state.reply_port = MACH_PORT_NULL;
	thread_state = per_thread;
}

/********************************************************
 *  Called by mig interfaces whenever they  need a reply port.
 *  Used to provide the same interface as multi-threaded tasks need.
 ********************************************************/

mach_port_t
mig_get_reply_port(void)
{
	struct mach_thread_state *state = mach_thread_state();

	if (state->reply_port == MACH_PORT_NULL)
		state->reply_port = mach_reply_port();

	return state->reply_port;
}

/*************************************************************
 *  Called by mig interfaces after a timeout on the port.
 *  Could be called by user.
 ***********************************************************/

void
mig_dealloc_reply_port(
	mach_port_t	reply_port)
{
	struct mach_thread_state *state = mach_thread_state();
	mach_port_t port;

	port = state->reply_port;
	state->reply_port = MACH_PORT_NULL;

	if (port != MACH_PORT_NULL)
		(void) mach_port_mod_refs(mach_task_self(), port,
					  MACH_PORT_RIGHT_RECEIVE, -1);
}

/*************************************************************
 *  Called by mig interfaces after each RPC.
 *  Could be called by user.
 ***********************************************************/

void
mig_put_reply_port(
	mach_port_t	reply_port)
{
}

/*
 * Lightweight reinit for a child task created with task_create(inherit_memory):
 * the child of a fork(), and a raw thread started in a task that inherited its
 * parent's memory.  The cached task port still names the parent's, and the
 * reply port and the per-thread hand-over are the parent's; both are reset.
 * Port registration, the page size and the RPC glue are inherited and left
 * alone.
 *
 * It lives here and not in mach_init.c, where it was.  A caller outside libmach
 * that names a function in mach_init.c pulls that file into libc.so beside
 * mach_init_sa.c, which defines the same globals, and the link fails.
 * mig_reset_after_fork() was put here for that reason, reset only libmach's
 * copy of the reply port, and is gone (#645).
 *
 * mach_task_self() in <mach_init.h> is a macro that reads the cached global, so
 * it is undefined here and the trap stub runs, returning a name that is valid
 * in the child's space (#269).
 */
#undef mach_task_self
extern mach_port_t mach_task_self(void);

void
mach_task_self_init(void)
{
	mach_task_self_ = mach_task_self();
	mig_init(0);
}
