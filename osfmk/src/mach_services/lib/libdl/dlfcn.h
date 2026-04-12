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
 * dlfcn.h — POSIX dynamic linking API for Uros.
 */

#ifndef _DLFCN_H_
#define _DLFCN_H_

/* Flags for dlopen (phase 1: only RTLD_NOW is meaningful) */
#define RTLD_NOW	0x0001
#define RTLD_LAZY	0x0002	/* accepted but treated as RTLD_NOW */
#define RTLD_GLOBAL	0x0004

/* I/O operations — must be set before calling dlopen */
struct dl_io_ops;
struct dl_host_sym;

void	dl_set_io_ops(const struct dl_io_ops *ops);
void	dl_set_host_symbols(const struct dl_host_sym *symtab);

/* Standard POSIX API */
void	*dlopen(const char *path, int mode);
void	*dlsym(void *handle, const char *name);
int	dlclose(void *handle);
const char *dlerror(void);

#endif /* _DLFCN_H_ */
