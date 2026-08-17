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

/*
 *	File:		kern/ipc_subsystem.c
 *	Purpose:	Routines to support ipc semantics of new kernel
 *			RPC subsystem descriptions
 */

#include <mach/message.h>
#include <kern/ipc_kobject.h>
#include <kern/task.h>
#include <kern/ipc_subsystem.h>
#include <kern/subsystem.h>
#include <kern/lock.h>
#include <kern/spl.h>
#include <kern/misc_protos.h>
#include <ipc/ipc_port.h>
#include <ipc/ipc_space.h>

/*
 *	Routine:	ipc_subsystem_init
 *	Purpose:
 *		Initialize ipc control of a subsystem.
 */
void
ipc_subsystem_init(
	subsystem_t		subsystem)
{
	ipc_port_t	port;

	port = ipc_port_alloc_kernel();
	if (port == IP_NULL)
		panic("ipc_subsystem_init");
	subsystem->ipc_self = port;
}

/*
 *	Routine:	ipc_subsystem_enable
 *	Purpose:
 *		Enable ipc access to a subsystem.
 */
void
ipc_subsystem_enable(
	subsystem_t		subsystem)
{
	ipc_kobject_set(subsystem->ipc_self,
			(ipc_kobject_t) subsystem, IKOT_SUBSYSTEM);
}


/*
 *      Routine:        ipc_subsystem_disable
 *      Purpose:
 *              Disable IPC access to a subsystem.
 *      Conditions:
 *              Nothing locked.
 */

void
ipc_subsystem_disable(
        subsystem_t        subsystem)
{
        ipc_port_t kport;

        kport = subsystem->ipc_self;
        if (kport != IP_NULL)
                ipc_kobject_set(kport, IKO_NULL, IKOT_NONE);
}


/*
 *	Routine:	convert_port_to_subsystem
 *	Purpose:
 *		Convert from a port to a subsystem.
 *		Doesn't consume the port ref; produces a subsystem ref,
 *		which may be null.
 *	Conditions:
 *		Nothing locked.
 *
 *	🔥 The three lines above are older than the body that now keeps them
 *	(#478).  Until this issue the routine took no reference at all, and
 *	every caller was left to guess which of the two contracts was real --
 *	the comment's or the code's.  kern/subsystem.h states the answer once;
 *	this is the routine that has to make it true.
 */
subsystem_t
convert_port_to_subsystem(
	ipc_port_t	port)
{
	boolean_t		r = FALSE;
	subsystem_t		subsystem = SUBSYSTEM_NULL;

	while (!r && IP_VALID(port)) {
		ip_lock(port);
		r = ref_subsystem_port_locked(port, &subsystem);
		/* port unlocked */
	}
	return (subsystem);
}


/*
 *	Routine:	ref_subsystem_port_locked
 *	Purpose:
 *		Take a reference on the subsystem a port names, for a caller
 *		that already holds the port lock and wants to keep the
 *		lookup and the reference in the same critical section.
 *	Conditions:
 *		The port is locked.  On return it is unlocked either way.
 *	Returns:
 *		TRUE with *psubsystem set -- possibly to SUBSYSTEM_NULL, if
 *		the port is dead or names something else.
 *		FALSE if the lock order made it retry; the caller relocks the
 *		port and asks again.
 */
boolean_t
ref_subsystem_port_locked(
	ipc_port_t	port,
	subsystem_t	*psubsystem)
{
	subsystem_t	subsystem = SUBSYSTEM_NULL;
	spl_t		s;

	if (ip_active(port) &&
	    (ip_kotype(port) == IKOT_SUBSYSTEM)) {
		subsystem = (subsystem_t) port->ip_kobject;
		assert(subsystem != SUBSYSTEM_NULL);

		/*
		 * Backwards, and knowingly: kern/subsystem.h puts
		 * subsystem_lock() before ip_lock(), because
		 * subsystem_deallocate() holds the subsystem across
		 * ipc_subsystem_disable() and that locks the port.  Taking
		 * the two in this order can only be a try, so
		 * subsystem_reference() is inlined here to accommodate it --
		 * the same accommodation, for the same reason, that
		 * ref_task_port_locked() makes for task_reference().
		 */
		s = splsched();
		if (!subsystem_lock_try(subsystem)) {
			splx(s);
			ip_unlock(port);
			mutex_pause();
			return (FALSE);
		}
		subsystem->ref_count++;
		subsystem_unlock(subsystem);
		splx(s);
	}
	*psubsystem = subsystem;
	ip_unlock(port);
	return (TRUE);
}


/*
 *	Routine:	convert_subsystem_to_port
 *	Purpose:
 *		Convert from a subsystem to a port.
 *		Produces a naked send right which may be invalid.
 *	Conditions:
 *		Nothing locked.
 */
ipc_port_t
convert_subsystem_to_port(
	subsystem_t		subsystem)
{
	ipc_port_t	port;

	port = ipc_port_make_send(subsystem->ipc_self);
	return (port);
}

