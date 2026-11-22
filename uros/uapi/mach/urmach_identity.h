/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * mach/urmach_identity.h -- a task's identity (#747).
 */

#ifndef _MACH_URMACH_IDENTITY_H_
#define _MACH_URMACH_IDENTITY_H_

#include <mach/message.h>	/* security_token_t */
#include <stdint.h>

/*
 * 🔑 A TASK'S IDENTITY: 64 bits the kernel gives a task when it creates it,
 * from a counter that is never reused, and stamps in the trailer of every
 * message the task sends (msgh_sender, the security token).  A forked child and
 * an exec'd program are tasks of their own and get identities of their own;
 * nothing inherits one, and nobody sets one -- task_set_security_token()
 * refuses.  So a receiver that asks for the sender's trailer element learns
 * which task sent the message, and a capability issued to one task can be
 * refused to every other (#747).
 *
 * 🔴 It used to be OSF's security token and nothing else: every task inherited
 * its parent's or a default every user task shared, and the kernel stamped its
 * own token, KERNEL_SECURITY_TOKEN, in the trailer of every message a task
 * sent -- so a trailer said that everything came from the kernel.
 *
 * The top bit is set in every task's identity, so that none is the kernel's
 * own, KERNEL_SECURITY_TOKEN ({0, 1}), nor the zero a capability with no
 * owner carries.
 */
#define	URMACH_IDENTITY_TASK_BIT	(1ULL << 63)

/* An identity as one number, as MIG carries it (mach_host.defs). */
typedef uint64_t	urmach_identity_t;

/* The identity a security token carries, as one number. */
static __inline__ uint64_t
urmach_identity_of(security_token_t t)
{
	return (uint64_t)t.val[0] | ((uint64_t)t.val[1] << 32);
}

/*
 * The receive option that asks for the sender's identity: OR it into a
 * server's receive -- mach_msg_server()'s options -- and every request comes
 * with it in its trailer.
 */
#define	URMACH_RCV_SENDER						\
	(MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |		\
	 MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_SENDER))

/*
 * Who sent a message received with URMACH_RCV_SENDER: the identity in its
 * trailer, which follows the message at its rounded size.  0 for a message
 * received without asking, which is no task's identity, so that a check
 * against it refuses rather than passes.
 */
static __inline__ uint64_t
urmach_msg_sender(const mach_msg_header_t *m)
{
	const mach_msg_format_0_trailer_t *t =
	    (const mach_msg_format_0_trailer_t *)
	    ((const char *)m + round_msg(m->msgh_size));

	if (t->msgh_trailer_type != MACH_MSG_TRAILER_FORMAT_0 ||
	    t->msgh_trailer_size < sizeof(mach_msg_security_trailer_t))
		return 0;
	return urmach_identity_of(t->msgh_sender);
}

#endif	/* _MACH_URMACH_IDENTITY_H_ */
