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

#ifndef _EXEC_TYPES_H_
#define _EXEC_TYPES_H_

/*
 * exec_types.h — wire-stable types for the exec MIG subsystem (#228).
 * Imported by exec.defs and shared with libposix-uros / proc_server
 * once those land.
 */

#include <stdint.h>
#include <mach/vm_types.h>

/* ------------------------------------------------------------------ */
/*  Server version (BSD-style major.minor.patch)                       */
/* ------------------------------------------------------------------ */

#define EXEC_SERVER_VERSION_MAJOR    0
#define EXEC_SERVER_VERSION_MINOR    1
#define EXEC_SERVER_VERSION_PATCH    0
#define EXEC_SERVER_VERSION_STRING   "0.1.0"

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define EXEC_PATH_MAX            1024
#define EXEC_BLOB_MAX            4096    /* argv/envp serialized blob */

/*
 * Initial stack layout in the new task: 16 KiB at a fixed VA below
 * the load_bias range used by ET_DYN binaries.  Everything in v0.1.0
 * is static ET_EXEC so this is just below the typical text base.
 */
#define EXEC_STACK_VA            0xBF800000U
#define EXEC_STACK_SIZE          (16 * 1024)
#define EXEC_STACK_TOP           (EXEC_STACK_VA + EXEC_STACK_SIZE)

/* ------------------------------------------------------------------ */
/*  Wire types                                                         */
/* ------------------------------------------------------------------ */

typedef char            exec_path_t[EXEC_PATH_MAX];

/*
 * exec_blob_t — serialized argv or envp blob, sent out-of-line.
 * Format: NUL-separated C strings followed by a terminating empty
 * string (i.e. two consecutive NUL bytes).  The server decodes by
 * scanning for the double-NUL and counts entries.  No length prefix
 * to keep the encoding trivial; the buffer length is on the wire.
 *
 * Empty argv is encoded as a single NUL byte (one empty string +
 * terminator), or a length-1 buffer holding "\0".  Caller must always
 * pass at least the terminator.
 */
typedef pointer_t       exec_blob_t;

/* ------------------------------------------------------------------ */
/*  Result codes (returned in result_code, distinct from kern_return)  */
/* ------------------------------------------------------------------ */

#define EXEC_OK                  0
#define EXEC_ERR_NOT_FOUND      -1   /* binary path not resolvable */
#define EXEC_ERR_READ           -2   /* vfs_read failed */
#define EXEC_ERR_PARSE          -3   /* libelf rejected the image */
#define EXEC_ERR_NOT_STATIC     -4   /* PT_INTERP present (v0.2.0 needed) */
#define EXEC_ERR_TASK_CREATE    -5   /* kernel task_create failed */
#define EXEC_ERR_VM_SETUP       -6   /* vm_allocate / vm_write / vm_protect */
#define EXEC_ERR_THREAD_CREATE  -7   /* thread_create or set_state failed */
#define EXEC_ERR_BAD_BLOB       -8   /* argv/envp blob malformed */
#define EXEC_ERR_TOO_BIG        -9   /* image or argv exceeds limits */
#define EXEC_ERR_PERMISSION    -10   /* future: manifest denial */

#endif /* _EXEC_TYPES_H_ */
