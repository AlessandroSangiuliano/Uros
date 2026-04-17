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
 * Modules are fetched from the bootstrap over MIG
 * (bootstrap_list_modules / bootstrap_get_module) as .so ELF images
 * published under /mach_servers/modules/<class>/, cached in-memory,
 * and resolved via libdl against the BDS's own dynamic symbol table.
 */

#ifndef _DL_LOADER_H_
#define _DL_LOADER_H_

#include "block_server.h"

/*
 * Initialise libdl: set the cache I/O callback and bootstrap the
 * running image into libdl's loaded-object list so dlopen'd modules
 * can resolve undefined symbols against it.  Must be called before
 * any blk_dl_load_class() / blk_dl_load_module() call.
 */
void	blk_dl_init(void);

/*
 * Fetch every module in <class> from the bootstrap (via MIG), dlopen
 * each one and store its block_driver_ops in out_ops[].  Returns the
 * number of modules successfully loaded (capped at max_ops).
 */
int	blk_dl_load_class(const char *class,
			  const struct block_driver_ops **out_ops,
			  int max_ops);

/*
 * Load a single module already present in the cache by name.  Returns
 * NULL on failure (prints error message).
 */
const struct block_driver_ops *blk_dl_load_module(const char *path);

/*
 * Unload a previously loaded module.
 */
void	blk_dl_unload_module(const char *path);

#endif /* _DL_LOADER_H_ */
