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

static mach_port_t	mig_reply_port = MACH_PORT_NULL;

/*
 * #299: these four entry points are also defined, with a per-thread
 * (pthread-TSD) implementation, in libpthreads/mig_support.c.  A program
 * that links BOTH libmach and libpthreads (i.e. any multithreaded server)
 * must use the per-thread version: otherwise every thread shares this one
 * static mig_reply_port, two threads receive on the same reply port, and
 * ipc_mqueue_deliver hands an RPC reply to the wrong thread — the caller
 * hangs forever (only visible under real concurrency, i.e. SMP 4+ CPUs;
 * gpu_server's render worker was the first victim).  Mark libmach's copies
 * WEAK so the strong libpthreads definitions win whenever both are linked;
 * pure single-threaded programs (libmach only) still get these as the sole,
 * weak-but-present definitions.
 */

__attribute__((weak))
void
mig_init(
	void		*first)
{
	if (first == (void *) 0)
		mig_reply_port = MACH_PORT_NULL;
}

/********************************************************
 *  Called by mig interfaces whenever they  need a reply port.
 *  Used to provide the same interface as multi-threaded tasks need.
 ********************************************************/

__attribute__((weak))
mach_port_t
mig_get_reply_port()
{
	if (mig_reply_port == MACH_PORT_NULL)
		mig_reply_port = mach_reply_port();

	return mig_reply_port;
}

/*************************************************************
 *  Called by mig interfaces after a timeout on the port.
 *  Could be called by user.
 ***********************************************************/

__attribute__((weak))
void
mig_dealloc_reply_port(
	mach_port_t	reply_port)
{
	mach_port_t port;

	port = mig_reply_port;
	mig_reply_port = MACH_PORT_NULL;

	(void) mach_port_mod_refs(mach_task_self(), port,
				  MACH_PORT_RIGHT_RECEIVE, -1);
}

/*************************************************************
 *  Called by mig interfaces after each RPC.
 *  Could be called by user.
 ***********************************************************/

__attribute__((weak))
void
mig_put_reply_port(
	mach_port_t	reply_port)
{
}

/*
 * Phase 5b (#255): drop libmach's cached port-name globals after a
 * fork().  The child's IPC space is empty when task_create returns,
 * so the parent's cached values are noise that next MIG calls will
 * accidentally reuse — MACH_SEND_INVALID_DEST on the first RPC.
 * Forces a re-cache via the relevant Mach traps on next use.
 */
extern mach_port_t mach_task_self_;
void
mig_reset_after_fork(void)
{
	mig_reply_port = MACH_PORT_NULL;
	/*
	 * mach_task_self() in <mach_init.h> is a macro that expands to the
	 * cached global, so `mach_task_self_ = mach_task_self()` would be a
	 * no-op.  Undef before invoking so the real trap stub runs and
	 * returns a port name valid in the child's IPC space — without
	 * this the child kept the parent's stale name, which exec_load
	 * passed to task_create and the kernel rejected as IKOT_ACT (#269).
	 */
#undef mach_task_self
	extern mach_port_t mach_task_self(void);
	mach_task_self_ = mach_task_self();
}
