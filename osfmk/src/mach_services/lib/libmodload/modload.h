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
 * modload.h — runtime module loader for class servers.
 *
 * Class servers (block_device_server, hal_server, net_server, ...) use
 * this library to pull their plug-in .so modules from the bootstrap over
 * MIG (bootstrap_list_modules / bootstrap_get_module) and dlopen them
 * against the server's own dynamic symbol table.
 *
 * The library is pure infrastructure: it knows nothing about the shape
 * of the module vtable.  Each caller passes the symbol suffix it expects
 * (e.g. "_module_ops") and casts the returned void * back to its own
 * vtable type.
 */

#ifndef _MODLOAD_H_
#define _MODLOAD_H_

/*
 * One-shot initialisation.  Must be called before any modload_load_*.
 *
 * @tag: short string used as a prefix in log messages ("blk", "hal", ...).
 *       Not copied; the caller must keep the pointer alive.
 */
void	modload_init(const char *tag);

/*
 * Fetch every module in <class> from the bootstrap, dlopen each one,
 * look up the symbol <module_basename><sym_suffix> and write the result
 * into out_ops[].  Returns the number of modules successfully loaded
 * (capped at max_ops).
 *
 * Example: modload_load_class("block", "_module_ops", ops, 16) pulls
 *          /mach_servers/modules/block/ahci.so and /virtio_blk.so,
 *          reads ahci_module_ops and virtio_blk_module_ops from each.
 */
int	modload_load_class(const char *class, const char *sym_suffix,
			   void **out_ops, int max_ops);

/*
 * Load a single already-cached module by basename (as returned by
 * bootstrap_list_modules) and return the symbol <basename><sym_suffix>.
 * Returns NULL on failure.
 */
void   *modload_load_module(const char *name, const char *sym_suffix);

#endif /* _MODLOAD_H_ */
