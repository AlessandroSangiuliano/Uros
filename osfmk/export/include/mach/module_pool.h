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
 * module_pool.h — shared types for the bootstrap module-pool RPC.
 *
 * Class servers (block_device_server, net_server, ...) use
 *   bootstrap_list_modules(class) → newline-separated names
 *   bootstrap_get_module(class, name) → raw .so bytes
 * to fetch plug-in modules the bootstrap publishes from the boot
 * filesystem under /mach_servers/modules/<class>/.
 */

#ifndef _MACH_MODULE_POOL_H_
#define _MACH_MODULE_POOL_H_

#define MODULE_NAME_MAX		64

/*
 * Matches `type module_name_t = c_string[*:64]` in bootstrap.defs.
 * MIG embeds this as the C field type in the generated request/reply
 * messages, so the typedef must be visible before including
 * <mach/bootstrap.h> or <mach/bootstrap_server.h>.
 */
typedef char module_name_t[MODULE_NAME_MAX];

#endif /* _MACH_MODULE_POOL_H_ */
