/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#ifndef _VFS_FLIPC_H_
#define _VFS_FLIPC_H_

/*
 * vfs_flipc.h — FLIPC v2 fast-path protocol between libvfs (client) and
 * an fs_server (#232).  Bulk read/write whose size crosses a threshold
 * route their data through a shared-memory FLIPC v2 channel pair instead
 * of a Mach RPC; control operations (open/close/stat/...) stay on Mach.
 *
 * Connection model: the fs_server registers a single named endpoint.
 * libvfs connects lazily on the first large request and caches the
 * channel pair.  Each request descriptor carries the mount identity
 * (the fs_port value the server handed out via fs_open) plus the open
 * handle, so one endpoint serves every mount the server hosts.
 *
 * Wire layout uses the flipc2 descriptor (struct flipc2_desc):
 *
 *   Request  (client -> server, rev channel):
 *     opcode      = VFS_FLIPC_OP_READ
 *     cookie      = caller-chosen correlation id
 *     param[0]    = fs_port (mount identity)
 *     param[1]    = handle  (server open-file cookie)
 *     param[2]    = offset
 *     data_length = bytes requested
 *
 *   Reply    (server -> client, fwd channel):
 *     opcode      = VFS_FLIPC_OP_READ
 *     cookie      = echoed from the request
 *     status      = VFS_FLIPC_OK or VFS_FLIPC_ERR_*
 *     data_offset = byte offset of the payload in the fwd data region
 *     data_length = bytes actually read (0 = EOF)
 */

#define VFS_FLIPC_ENDPOINT_PREFIX  "flipc."   /* + service name */

/* Channel sizing for an fs fast-path connection. */
#define VFS_FLIPC_CHANNEL_SIZE     (256 * 1024)
#define VFS_FLIPC_RING_ENTRIES     64

/* Largest single FLIPC read payload (kept under the channel data region;
 * libvfs/callers loop for bigger transfers, short reads are allowed). */
#define VFS_FLIPC_MAX_READ         (192 * 1024)

/* Default size threshold (bytes): requests >= this take the FLIPC path. */
#define VFS_FLIPC_THRESHOLD        4096

/* Opcodes. */
#define VFS_FLIPC_OP_READ          1

/* Reply status codes. */
#define VFS_FLIPC_OK               0
#define VFS_FLIPC_ERR_BADHANDLE    1
#define VFS_FLIPC_ERR_IO           2
#define VFS_FLIPC_ERR_INVAL        3

#endif /* _VFS_FLIPC_H_ */
