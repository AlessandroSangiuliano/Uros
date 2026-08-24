/*
 * Copyright (c) 2026 Alessandro Sangiuliano <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * How long a netname is.  One definition, reachable from both halves.
 *
 * 🔥 There used to be three answers, and nothing compared them:
 *
 *	servers/netname_defs.h		typedef char netname_name_t[80];
 *	name_server/netname.defs	type netname_name_t = c_string[*:128];
 *	lib/libvfs/libvfs.c		char prefix[80];   -- and a comment
 *					saying it "matches netname_name_t"
 *
 * MIG believed the second, so the wire could carry 128 bytes into buffers the
 * first had made 80 long, and nprocs.c copied them with a bound of 80 into a
 * destination of exactly 80 -- which strncpy does not terminate.  A client
 * that checked in a long enough name left an unterminated key in the name
 * server, and the strcmp that ran off it ran on behalf of the NEXT client.
 * The third answer was a comment, which is the one kind of check that cannot
 * fail.
 *
 * ⚠️ This header holds preprocessor definitions and NOTHING else, on purpose.
 * `netname.defs' runs through cpp before migcom sees it, so it can include
 * this file and use the numbers; it could not include netname_defs.h, whose C
 * declarations migcom's parser has no idea what to do with.  Putting a
 * declaration here would break the .defs side silently -- the include would
 * still succeed and the parse would fail somewhere else.
 *
 * 🔑 The number is 128 rather than 80 because the wire already permitted 128:
 * narrowing would have rejected names that reach the server today, while
 * widening cannot break a client that works.  Every buffer now follows from
 * here, so the cost is 48 bytes on a local and the drift is impossible rather
 * than merely discouraged.
 */

#ifndef	_SERVERS_NETNAME_SIZE_H_
#define	_SERVERS_NETNAME_SIZE_H_

#define	NETNAME_NAME_MAX	128	/* a service name, with its terminator */
#define	NETNAME_PATH_MAX	1024	/* a full path, for mount look-ups */

#endif	/* _SERVERS_NETNAME_SIZE_H_ */
