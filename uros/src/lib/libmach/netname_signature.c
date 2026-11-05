/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The port a task signs its names with at the name server (#744).
 *
 * A name is checked in with a signature, and the name server lets it be checked
 * in again or checked out only by whoever presents the same one.  So the
 * signature is the whole of what makes a name its server's: a port anyone else
 * can present is a name anyone else can take.
 *
 * ⚠️ NOT MACH_PORT_NULL, which four servers passed.  Every task can present it,
 * so cap_server's name was anyone's to rebind (#744) and ext_server's mounts
 * anyone's to check out (#716).  The name server refuses it now.
 *
 * ⚠️ NOT mach_task_self(), which six passed, and which looks like the obvious
 * private port.  The signature is SENT -- netname.defs takes it as a copied send
 * right -- so the name server kept a send right to the task port of every server
 * that checked a name in, and with it the power to read and write that server's
 * memory and to take its receive rights.  A signature has to prove who
 * registered a name without handing the name server the registrant.
 *
 * ⚠️ NOT the port being registered: everyone who looks the name up holds a send
 * right to it.  The name server refuses that one too.
 *
 * 🔑 So it is a port of its own, made for this and given to nobody else.  The
 * task holds the receive right, and the only send rights are its own and the
 * ones it hands the name server.  When the task dies the receive right dies with
 * it, and the name server, which asks for a dead-name notification on every
 * signature, removes the names it signed -- what the task port's death did when
 * that was the signature.
 *
 * One per task, made the first time it is asked for.  Two threads asking at once
 * both make one, and the one that loses the publication destroys its own, so
 * every caller in a task is handed the same port: a name checked in under one
 * signature and checked out under another would be refused.
 *
 * ⚠️ A child made with task_create(inherit_memory) has its parent's memory and
 * none of its rights, so the name it inherits here names nothing, or names
 * something else.  mach_task_self_init(), which such a child must call before
 * its first RPC, forgets it.
 */

#include <mach.h>
#include "externs.h"

static mach_port_t	netname_signature = MACH_PORT_NULL;

kern_return_t
urmach_netname_signature(mach_port_t *sig)
{
	mach_port_t	mine, published;
	kern_return_t	kr;

	published = __atomic_load_n(&netname_signature, __ATOMIC_ACQUIRE);
	if (published != MACH_PORT_NULL) {
		*sig = published;
		return KERN_SUCCESS;
	}

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&mine);
	if (kr != KERN_SUCCESS)
		return kr;
	/* netname.defs copies a send right, so the task needs one to copy. */
	kr = mach_port_insert_right(mach_task_self(), mine, mine,
				    MACH_MSG_TYPE_MAKE_SEND);
	if (kr != KERN_SUCCESS) {
		(void)mach_port_destroy(mach_task_self(), mine);
		return kr;
	}

	published = MACH_PORT_NULL;
	if (!__atomic_compare_exchange_n(&netname_signature, &published, mine,
					 0, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE)) {
		(void)mach_port_destroy(mach_task_self(), mine);
		mine = published;
	}
	*sig = mine;
	return KERN_SUCCESS;
}

void
urmach_netname_signature_forget(void)
{
	__atomic_store_n(&netname_signature, MACH_PORT_NULL, __ATOMIC_RELEASE);
}
