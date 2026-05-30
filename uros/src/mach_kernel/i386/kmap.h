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
 * i386/kmap.h
 *
 * Temporary kernel virtual address mappings for highmem physical pages.
 * On i386 with HIGHMEM, physical pages above LOWMEM_LIMIT have no permanent
 * kernel VA.  kmap() provides a temporary VA for kernel access (zero, copy,
 * etc.) and kunmap() releases it.
 *
 * For lowmem pages, kmap() returns phystokv(pa) directly and kunmap() is a
 * no-op, so callers don't need to distinguish lowmem from highmem.
 */

#ifndef _I386_KMAP_H_
#define _I386_KMAP_H_

#include <mach/vm_types.h>
#include <mach/i386/vm_param.h>

/*
 * Number of kmap slots.  Each slot maps one 4KB page.
 * Must be large enough to handle concurrent kmap users
 * (pmap_copy_page needs 2 simultaneously).
 */
#define KMAP_SLOTS		64
#define KMAP_WINDOW_SIZE	(KMAP_SLOTS * I386_PGBYTES)

/*
 * kmap(pa) — map a physical page into kernel VA temporarily.
 * Returns a kernel virtual address valid until kunmap(pa) is called.
 * For lowmem pages (pa < LOWMEM_LIMIT), returns phystokv(pa) directly.
 */
extern vm_offset_t	kmap(vm_offset_t phys_addr);

/*
 * kunmap(pa) — release a temporary mapping created by kmap().
 * No-op for lowmem pages.
 */
extern void		kunmap(vm_offset_t phys_addr);

/*
 * kmap_init() — initialize the kmap subsystem.
 * Called from pmap_bootstrap() after the kmap window VA range is reserved.
 */
extern void		kmap_init(vm_offset_t base_va);

/*
 * Base VA of the kmap window (set by pmap_bootstrap).
 */
extern vm_offset_t	kmap_base;

#endif /* _I386_KMAP_H_ */
