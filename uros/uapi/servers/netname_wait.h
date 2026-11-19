/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * netname_wait() -- wait for a name to be checked in with the name server, on
 * the check-in itself and not on a count of tries or a deadline (#696).
 */

#ifndef _SERVERS_NETNAME_WAIT_H_
#define _SERVERS_NETNAME_WAIT_H_

#include <mach/kern_return.h>
#include <mach/port.h>

/*
 * Wait until `name' is checked in with the name server `server', and put a
 * send right to the port it names in *port.  It returns at once when the name
 * is already there.  On a failure *port is MACH_PORT_NULL, and the answer is
 * the call's that failed, or MIG_BAD_ID for a notification that was not one.
 *
 * ⚠️ A name never checked in is waited for as long as the caller lives.
 */
extern kern_return_t netname_wait(mach_port_t server, const char *name,
				  mach_port_t *port);

#endif	/* _SERVERS_NETNAME_WAIT_H_ */
