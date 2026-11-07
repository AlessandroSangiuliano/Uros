/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A port-destroyed notification, as the kernel sends one, through the stub MIG
 * generates for it (#574).
 *
 * bootstrap asks for one on every port it lends a server (service.c), so that
 * a server that dies gives its slot's receive right back instead of taking it
 * down with it.  When the name server died holding NAME_SERVER_SLOT's, the
 * notification that brought the right back was refused by bootstrap's stub,
 * and the right was destroyed with the message: on x86-64 the kernel said the
 * message was 52 bytes long, counting the padding after its trailer, and the
 * stub reads 48.  No other task asks for these notifications.  This is the
 * same message, from the same kernel code, to a stub generated from the same
 * notify.defs -- without a server having to die for it.
 */

#include <mach.h>
#include <mach/message.h>
#include <mach/notify.h>
#include <mach/port.h>

#include <string.h>

#include "notify_server.h"
#include "mount_test.h"

static mach_port_t	probe_rights;
static int		probe_calls;

kern_return_t
do_mach_notify_port_destroyed(mach_port_t notify, mach_port_t rights)
{
	(void)notify;
	probe_rights = rights;
	probe_calls++;
	return KERN_SUCCESS;
}

/* The four this probe never asks for: a call to one is a wrong message. */
kern_return_t
do_mach_notify_port_deleted(mach_port_t notify, mach_port_t name)
{
	(void)notify; (void)name;
	return KERN_FAILURE;
}

kern_return_t
do_mach_notify_no_senders(mach_port_t notify, mach_port_mscount_t mscount)
{
	(void)notify; (void)mscount;
	return KERN_FAILURE;
}

kern_return_t
do_mach_notify_send_once(mach_port_t notify)
{
	(void)notify;
	return KERN_FAILURE;
}

kern_return_t
do_mach_notify_dead_name(mach_port_t notify, mach_port_t name)
{
	(void)notify; (void)name;
	return KERN_FAILURE;
}

static void
mt_drop_receive(mach_port_t port)
{
	if (MACH_PORT_VALID(port))
		(void)mach_port_mod_refs(mach_task_self(), port,
					 MACH_PORT_RIGHT_RECEIVE, -1);
}

const char *
mt_port_destroyed(mach_msg_id_t *id, mach_msg_size_t *size)
{
	mach_port_t		victim = MACH_PORT_NULL, notify = MACH_PORT_NULL;
	mach_port_t		previous = MACH_PORT_NULL;
	mach_port_type_t	type = 0;
	mach_msg_return_t	mr;
	const char		*why = NULL;
	union {
		mach_msg_header_t	head;
		char			room[256];
	} in, out;

	*id = 0;
	*size = 0;
	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			       &victim) != KERN_SUCCESS ||
	    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			       &notify) != KERN_SUCCESS) {
		mt_drop_receive(victim);
		return "no ports";
	}
	if (mach_port_request_notification(mach_task_self(), victim,
					   MACH_NOTIFY_PORT_DESTROYED, 0,
					   notify, MACH_MSG_TYPE_MAKE_SEND_ONCE,
					   &previous) != KERN_SUCCESS) {
		mt_drop_receive(victim);
		mt_drop_receive(notify);
		return "the kernel refused the request for the notification";
	}

	/* The receive right dies here, and the kernel sends it to `notify'. */
	(void)mach_port_mod_refs(mach_task_self(), victim,
				 MACH_PORT_RIGHT_RECEIVE, -1);

	memset(&in, 0, sizeof in);
	mr = mach_msg(&in.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof in,
		      notify, 2000, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS) {
		mt_drop_receive(notify);
		return "no notification arrived";
	}
	*id = in.head.msgh_id;
	*size = in.head.msgh_size;

	probe_calls = 0;
	probe_rights = MACH_PORT_NULL;
	memset(&out, 0, sizeof out);
	(void)notify_server(&in.head, &out.head);

	if (probe_calls != 1) {
		why = "the stub refused it";
		mach_msg_destroy(&in.head);	/* the right it carried, unclaimed */
	} else if (mach_port_type(mach_task_self(), probe_rights, &type)
		   != KERN_SUCCESS || !(type & MACH_PORT_TYPE_RECEIVE)) {
		why = "what came back is not a receive right";
	}
	mt_drop_receive(probe_rights);
	mt_drop_receive(notify);
	return why;
}
