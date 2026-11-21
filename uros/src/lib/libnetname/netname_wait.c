/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * netname_wait() -- wait for a name to be checked in, on the check-in itself
 * (#696).
 */

#include <mach.h>
#include <mach/message.h>
#include <mach/mig_errors.h>
#include <servers/netname.h>
#include <servers/netname_wait.h>

/*
 * 🔴 ON A FACT, NOT ON A COUNT (#696).  The HAL's name was waited for six
 * ways, and four of them were a count of yields, a deadline or a single look:
 * a count lasts as long as a yield costs, which the accelerator, the host and
 * the kernel decide, and under KVM at four processors irq_claim_test's two
 * hundred yields ran out before the HAL checked in in 19 of 60 boots on
 * 19/11, so its arms past [6] did not run.  The name server sends a
 * notification when a name is checked in, at once when it already is, so a
 * wait on that notification ends on the check-in and on nothing else.
 *
 * ⚠️ On nothing else: a name that is never checked in is waited for as long as
 * the caller lives.  Whether one is coming is known to whoever starts what the
 * bundle holds, not to the name server, and #696 is to say it.
 */
kern_return_t
netname_wait(mach_port_t server, const char *name, mach_port_t *port)
{
	netname_notify_msg_t	m;
	mach_port_t		recv = MACH_PORT_NULL;
	kern_return_t		kr;

	*port = MACH_PORT_NULL;
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&recv);
	if (kr != KERN_SUCCESS)
		return kr;
	kr = netname_notify(server, name, recv);
	if (kr == KERN_SUCCESS)
		kr = mach_msg(&m.head, MACH_RCV_MSG, 0, sizeof(m), recv,
			      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (kr == KERN_SUCCESS) {
		/* Only the name server holds a right to `recv', and once. */
		if (m.head.msgh_id == NETNAME_NOTIFY_MSID &&
		    (m.head.msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0 &&
		    m.body.msgh_descriptor_count == 1 &&
		    m.service.type == MACH_MSG_PORT_DESCRIPTOR)
			*port = m.service.name;
		else {
			mach_msg_destroy(&m.head);
			kr = MIG_BAD_ID;
		}
	}
	(void)mach_port_mod_refs(mach_task_self(), recv,
				 MACH_PORT_RIGHT_RECEIVE, -1);
	return kr;
}
