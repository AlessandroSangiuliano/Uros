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
 * i386/kmap.c
 *
 * Temporary kernel virtual address mappings for highmem physical pages.
 * Provides kmap()/kunmap() for kernel code that needs to access physical
 * pages that have no permanent 1:1 kernel VA (above LOWMEM_LIMIT).
 */

#include <mach/vm_param.h>
#include <mach/machine/vm_param.h>
#include <vm/pmap.h>
#include <intel/pmap.h>
#include <i386/kmap.h>
#include <kern/lock.h>
#include <kern/spl.h>

/*
 * Base VA of the kmap window, set by pmap_bootstrap via kmap_init().
 */
vm_offset_t	kmap_base;

/*
 * Per-slot state: physical address currently mapped (0 = free).
 */
static vm_offset_t	kmap_slot_pa[KMAP_SLOTS];

/*
 * Lock protecting the kmap slot array.
 */
decl_simple_lock_data(static, kmap_lock)

/*
 * Inline invlpg for single-page TLB invalidation (i486+).
 */
static inline void
invlpg(vm_offset_t va)
{
	__asm__ __volatile__("invlpg (%0)" : : "r"(va) : "memory");
}

/*
 * kmap_init — initialize the kmap subsystem.
 * Called from pmap_bootstrap() with the base VA of the kmap window.
 * The window must already have page table entries allocated (zeroed).
 */
void
kmap_init(vm_offset_t base_va)
{
	int i;

	kmap_base = base_va;
	simple_lock_init(&kmap_lock, 0);

	for (i = 0; i < KMAP_SLOTS; i++)
		kmap_slot_pa[i] = 0;
}

/*
 * kmap — map a physical page into kernel VA temporarily.
 *
 * For lowmem pages (pa < LOWMEM_LIMIT), returns phystokv(pa) directly.
 * For highmem pages, finds a free kmap slot, writes the PTE, invalidates
 * the TLB entry, and returns the slot VA.
 *
 * The caller must call kunmap(pa) when done.
 * kmap may be called with interrupts disabled (e.g., from pmap_expand).
 */
vm_offset_t
kmap(vm_offset_t phys_addr)
{
	int		i;
	vm_offset_t	va;
	pt_entry_t	*pte;
	spl_t		s;

	if (pa_is_lowmem(phys_addr))
		return phystokv(phys_addr);

	s = splhi();
	simple_lock(&kmap_lock);

	for (i = 0; i < KMAP_SLOTS; i++) {
		if (kmap_slot_pa[i] == 0) {
			kmap_slot_pa[i] = phys_addr;
			simple_unlock(&kmap_lock);
			splx(s);

			va = kmap_base + i * I386_PGBYTES;
			pte = pmap_pte(kernel_pmap, va);
			if (pte == PT_ENTRY_NULL)
				panic("kmap: no PTE for slot %d (va 0x%x)", i, va);
			*pte = pa_to_pte(phys_addr)
				| INTEL_PTE_VALID
				| INTEL_PTE_WRITE;
			invlpg(va);
			return va;
		}
	}

	simple_unlock(&kmap_lock);
	splx(s);
	panic("kmap: no free slots (pa 0x%x)", phys_addr);
	/* NOTREACHED */
	return 0;
}

/*
 * kunmap — release a temporary kmap mapping.
 * No-op for lowmem pages.
 */
void
kunmap(vm_offset_t phys_addr)
{
	int		i;
	vm_offset_t	va;
	pt_entry_t	*pte;
	spl_t		s;

	if (pa_is_lowmem(phys_addr))
		return;

	s = splhi();
	simple_lock(&kmap_lock);

	for (i = 0; i < KMAP_SLOTS; i++) {
		if (kmap_slot_pa[i] == phys_addr) {
			kmap_slot_pa[i] = 0;
			simple_unlock(&kmap_lock);
			splx(s);

			va = kmap_base + i * I386_PGBYTES;
			pte = pmap_pte(kernel_pmap, va);
			if (pte != PT_ENTRY_NULL)
				*pte = 0;
			invlpg(va);
			return;
		}
	}

	simple_unlock(&kmap_lock);
	splx(s);
	panic("kunmap: pa 0x%x not found in kmap slots", phys_addr);
}
