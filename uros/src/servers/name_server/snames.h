/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * name_server/snames.h -- what netname.c and nprocs.c share (#747).
 */

#ifndef _SNAMES_H_
#define _SNAMES_H_

#include <stdint.h>

/*
 * Who sent the request being served, by the identity the kernel stamped in
 * its trailer: the presenter of a mount's capability (nprocs.c).  Set by
 * snames_demux() around each request; one message loop, one request at a
 * time.
 */
extern uint64_t snames_request_sender;

#endif	/* _SNAMES_H_ */
