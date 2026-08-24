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
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS 
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
 * any improvements or extensions that they make and grant Carnegie the
 * rights to redistribute these changes.
 */
/*
 * MkLinux
 */

/*
 *	File:	netname_defs.h
 *	Author: Dan Julin, Carnegie Mellon University
 *	Date:	Dec. 1986
 *
 * 	Definitions for the mig interface to the network name service.
 */

#ifndef	_NETNAME_DEFS_
#define	_NETNAME_DEFS_

#define NETNAME_SUCCESS		(0)
#define	NETNAME_PENDING		(-1)
#define NETNAME_NOT_YOURS	(1000)
#define NAME_NOT_YOURS		(1000)
#define NETNAME_NOT_CHECKED_IN	(1001)
#define NAME_NOT_CHECKED_IN	(1001)
#define NETNAME_NO_SUCH_HOST	(1002)
#define NETNAME_HOST_NOT_FOUND	(1003)
#define	NETNAME_INVALID_PORT	(1004)
#define NETNAME_NO_MOUNT	(1005)	/* no mount prefix matched (#220) */
#define NETNAME_MOUNT_EXISTS	(1006)	/* prefix already registered (#220) */

/*
 * ⚠️ Both sizes come from <servers/netname_size.h>, which netname.defs also
 * includes.  They used to be literals here and different literals there; see
 * that header for what the disagreement cost.
 */
#include <servers/netname_size.h>

typedef char netname_name_t[NETNAME_NAME_MAX];
typedef char netname_path_t[NETNAME_PATH_MAX];

/*
 * The same two, as INPUTS.
 *
 * ⚠️ A parameter declared `char[N]' tells the compiler the caller supplies N
 * readable bytes, and for an `out' name that is exactly right.  For an `in'
 * name it is a promise nobody keeps: the value is a string, mig_strncpy stops
 * at its terminator, and every call site passes a literal far shorter than N.
 * gcc reported that at each one, correctly, about code that was fine -- and a
 * warning that is right and harmless at every call site is how a whole class
 * gets ignored.
 *
 * netname.defs gives these to its `in' parameters; see the note there.
 */
typedef const char *netname_name_in_t;
typedef const char *netname_path_in_t;

/*
 * Notification message sent by the name server to watchers when
 * a service becomes available via netname_check_in().
 * Contains the service port as a send right.
 */
#define NETNAME_NOTIFY_MSID	1099

typedef struct {
	mach_msg_header_t		head;
	mach_msg_body_t			body;
	mach_msg_port_descriptor_t	service;
	mach_msg_trailer_t		trailer;
} netname_notify_msg_t;

#endif /* NETNAME_DEFS_ */
