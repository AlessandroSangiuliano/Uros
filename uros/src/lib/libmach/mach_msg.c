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

#include <mach/port.h>
#include <mach/message.h>

#define LIBMACH_OPTIONS	(MACH_SEND_INTERRUPT|MACH_RCV_INTERRUPT)

/*
 * Which trap mach_msg() reaches the kernel through (#469).
 *
 * The plain six-argument form, or the nine-argument overwrite form with three
 * constants that say "not this time".  The two are the A and the B of a
 * measurement, and the switch is compile-time on purpose: a run-time choice
 * would put a branch on the path being measured.
 *
 * Define UROS_MACH_MSG_WIDE to build the old shape.
 */
#ifdef	UROS_MACH_MSG_WIDE

/*
 * ⚠️ The original body, VERBATIM, and not a delegation to
 * mach_msg_overwrite().  Delegating would have been shorter and would have
 * made the comparison worthless: A has to be what the tree had, or the
 * measurement is of two changes at once.
 */
mach_msg_return_t
mach_msg(mach_msg_header_t *msg, mach_msg_option_t option,
	 mach_msg_size_t send_size, mach_msg_size_t rcv_size,
	 mach_port_t rcv_name, mach_msg_timeout_t timeout,
	 mach_port_t notify)
{
	mach_msg_return_t mr;

	mr = mach_msg_overwrite_trap(msg, option &~ LIBMACH_OPTIONS,
			   send_size, rcv_size, rcv_name,
			   timeout, notify, MACH_MSG_NULL, 0);
	if (mr == MACH_MSG_SUCCESS)
		return MACH_MSG_SUCCESS;

	if ((option & MACH_SEND_INTERRUPT) == 0)
		while (mr == MACH_SEND_INTERRUPTED)
			mr = mach_msg_overwrite_trap(msg,
				option &~ LIBMACH_OPTIONS,
				send_size, rcv_size, rcv_name,
				timeout, notify, MACH_MSG_NULL, 0);

	if ((option & MACH_RCV_INTERRUPT) == 0)
		while (mr == MACH_RCV_INTERRUPTED)
			mr = mach_msg_overwrite_trap(msg,
				option &~ (LIBMACH_OPTIONS|MACH_SEND_MSG),
				0, rcv_size, rcv_name,
				timeout, notify, MACH_MSG_NULL, 0);

	return mr;
}

#else	/* the narrow trap */

/*
 * ⚠️ The test on `notify' is ONCE, at the top, and that is not a style
 * preference — it cost a measurement.
 *
 * The first version chose the trap at each of the three call sites, with a
 * conditional expression inside a macro.  It compiles: gcc emits both calls
 * three times over, and the function goes from 69 instructions to 122.  A
 * benchmark run against that would have measured a function twice the size
 * and reported it as the narrow trap's number.
 *
 * A caller that wants a notify port is rare and already has a home: the
 * nine-argument path is exactly mach_msg_overwrite() with no scatter list,
 * retry loops and all.  So the branch is taken once, almost never, and what
 * remains below has one call shape in it.
 */
mach_msg_return_t
mach_msg(mach_msg_header_t *msg, mach_msg_option_t option,
	 mach_msg_size_t send_size, mach_msg_size_t rcv_size,
	 mach_port_t rcv_name, mach_msg_timeout_t timeout,
	 mach_port_t notify)
{
	mach_msg_return_t mr;

	if (notify != MACH_PORT_NULL)
		return mach_msg_overwrite(msg, option, send_size, rcv_size,
					  rcv_name, timeout, notify,
					  MACH_MSG_NULL, 0);

	/*
	 * The option bits libmach implements itself are kept from the kernel,
	 * so that their presence does not inhibit its fast path.
	 */
	mr = urmach_msg(msg, option &~ LIBMACH_OPTIONS,
			send_size, rcv_size, rcv_name, timeout);
	if (mr == MACH_MSG_SUCCESS)
		return MACH_MSG_SUCCESS;

	if ((option & MACH_SEND_INTERRUPT) == 0)
		while (mr == MACH_SEND_INTERRUPTED)
			mr = urmach_msg(msg, option &~ LIBMACH_OPTIONS,
					send_size, rcv_size, rcv_name, timeout);

	if ((option & MACH_RCV_INTERRUPT) == 0)
		while (mr == MACH_RCV_INTERRUPTED)
			mr = urmach_msg(msg,
					option &~ (LIBMACH_OPTIONS|MACH_SEND_MSG),
					0, rcv_size, rcv_name, timeout);

	return mr;
}

#endif	/* UROS_MACH_MSG_WIDE */

mach_msg_return_t
mach_msg_overwrite(mach_msg_header_t *msg, mach_msg_option_t option,
		   mach_msg_size_t send_size, mach_msg_size_t rcv_limit,
		   mach_port_t rcv_name, mach_msg_timeout_t timeout,
		   mach_port_t notify, mach_msg_header_t *rcv_msg,
		   mach_msg_size_t rcv_msg_size)
{
	mach_msg_return_t mr;

	/*
	 * Consider the following cases:
	 *	1) Errors in pseudo-receive (eg, MACH_SEND_INTERRUPTED
	 *	plus special bits).
	 *	2) Use of MACH_SEND_INTERRUPT/MACH_RCV_INTERRUPT options.
	 *	3) RPC calls with interruptions in one/both halves.
	 *
	 * We refrain from passing the option bits that we implement
	 * to the kernel.  This prevents their presence from inhibiting
	 * the kernel's fast paths (when it checks the option value).
	 */

	mr = mach_msg_overwrite_trap(msg, option &~ LIBMACH_OPTIONS,
			   send_size, rcv_limit, rcv_name,
			   timeout, notify, rcv_msg, rcv_msg_size);
	if (mr == MACH_MSG_SUCCESS)
		return MACH_MSG_SUCCESS;

	if ((option & MACH_SEND_INTERRUPT) == 0)
		while (mr == MACH_SEND_INTERRUPTED)
			mr = mach_msg_overwrite_trap(msg,
				option &~ LIBMACH_OPTIONS,
				send_size, rcv_limit, rcv_name,
				timeout, notify, rcv_msg, rcv_msg_size);

	if ((option & MACH_RCV_INTERRUPT) == 0)
		while (mr == MACH_RCV_INTERRUPTED)
			mr = mach_msg_overwrite_trap(msg,
				option &~ (LIBMACH_OPTIONS|MACH_SEND_MSG),
				0, rcv_limit, rcv_name,
				timeout, notify, rcv_msg, rcv_msg_size);

	return mr;
}
