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
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * dl_loader.h — Dynamic module loading support for block_device_server.
 *
 * Provides the libdl I/O callback, host symbol table, and module
 * loading interface.  Phase 1: modules are embedded as binary blobs
 * in the server executable via objcopy.  Future: read from filesystem.
 */

#ifndef _DL_LOADER_H_
#define _DL_LOADER_H_

#include "block_server.h"

/*
 * Initialise libdl: set I/O callbacks and host symbol table.
 * Must be called before any dlopen() call.
 */
void	blk_dl_init(void);

/*
 * Load a driver module by path and return its block_driver_ops.
 * Returns NULL on failure (prints error message).
 */
const struct block_driver_ops *blk_dl_load_module(const char *path);

/*
 * Unload a previously loaded module.
 */
void	blk_dl_unload_module(const char *path);

#endif /* _DL_LOADER_H_ */
