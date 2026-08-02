/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What the machine-independent VM asks this machine before it can run (#453,
 * on #407's ground).
 *
 * Two of these are the bootstrap handshake and the rest are questions about
 * physical pages.  The handshake is the interesting part: at startup the VM
 * has no page allocator of its own -- it is building one -- so it asks the
 * machine for physical pages one at a time, and for the range of kernel
 * virtual addresses it may hand out.  Once vm_page_bootstrap() has taken
 * what it needs, the machine is never asked again.
 */

#include <stdint.h>

#include <mach/machine/vm_param.h>
#include <vm/pmap.h>

#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>
#include <pmap/pv.h>
#include <pmap/walk.h>
#include <trap/trap.h>

/*
 * One more physical page for the VM to take over, or FALSE when there are
 * none left.
 *
 * The pages come from the boot frame allocator, which is the only thing that
 * knows what the loader left free.  Handing them over one at a time looks
 * wasteful and is not: the VM calls this until it says no, so the loop is
 * the transfer, and doing it in bulk would need an interface that says how
 * many are coming, which the machine-independent side does not have.
 */
boolean_t
pmap_next_page(vm_offset_t *paddr)
{
	uint64_t	pa = boot_frame_alloc();

	if (pa == 0)
		return FALSE;

	*paddr = (vm_offset_t) pa;
	return TRUE;
}

/*
 * How many are left.  Used to size the page array before the transfer above
 * begins, so it must not be an underestimate -- a short array is a buffer
 * overrun in vm_page_bootstrap(), not a smaller page cache.
 */
unsigned int
pmap_free_pages(void)
{
	return (unsigned int) (boot_frames_total() - boot_frames_used());
}

/*
 * The kernel virtual addresses the VM may allocate from.
 *
 * ⚠️ The heap, and not the whole kernel half.  Everything else in the upper
 * half is already spoken for by the layout -- the direct map, the per-CPU
 * blocks, the entry stacks, the device map, the image -- and handing any of
 * it to the VM would let kmem_alloc() return an address that already means
 * something else.  <pmap/layout.h> is where those boundaries live and this
 * is the one range it leaves free.
 */
void
pmap_virtual_space(vm_offset_t *startp, vm_offset_t *endp)
{
	*startp = (vm_offset_t) KERNEL_HEAP_BASE;
	*endp   = (vm_offset_t) (KERNEL_HEAP_BASE + KERNEL_HEAP_SIZE);
}

/*
 * Advisory: the caller is telling the machine that a range will or will not
 * be paged.  Nothing to do here, and nothing on i386 either.
 *
 * It is a hint from an era when a machine might have wanted to pre-build
 * page tables for a range about to be wired, and this one does not: tables
 * are built on demand by pmap_enter() and there is nothing cheaper to do
 * with advance notice.  Kept because the interface has it, empty because
 * the honest answer is that it costs nothing to ignore.
 */
void
pmap_pageable(pmap_t pmap, vm_offset_t start, vm_offset_t end,
	      boolean_t pageable)
{
	(void) pmap;
	(void) start;
	(void) end;
	(void) pageable;
}

/*
 * Give back page-table pages that no longer map anything.
 *
 * Nothing yet, and this one is a real gap rather than a hint that does not
 * apply: a space that maps and unmaps a large range keeps the empty tables
 * that mapped it, which is up to 2 MiB of tables per GiB of range touched.
 *
 * ⚠️ i386 DOES implement this -- intel/pmap.c walks the directory, finds
 * tables whose pages are unreferenced, and frees them under PMAP_READ_LOCK
 * and #338's per-table locks.  So the work is known to be possible and the
 * locking is known to have an answer; what this pmap has not got yet is that
 * answer, because the walk must be safe against another processor entering a
 * mapping into the table being freed.
 *
 * It is empty rather than absent because kern/task.c and kern/thread_swap.c
 * call it under memory pressure and a missing symbol would not link.  The
 * pageout daemon asking for memory back and getting none is a slower kernel;
 * a freed table another processor is walking into is a corrupted one.
 *
 * ▶️ #455 carries it, gated on the pmap's locking discipline -- which is
 * being decided rather than inherited, alongside the mutex (#452, done) and
 * the spl levels (#454).  i386's answer is #338's per-table locks, arrived
 * at by measurement on 8 processors; this kernel is NCPUS=64 and whether
 * that granularity still holds is a question with a number in front of it.
 */
void
pmap_collect(pmap_t pmap)
{
	(void) pmap;
}

/*
 * Is this physical page mapped by nobody?
 *
 * Asked before a page is handed out again, so a wrong TRUE gives a task
 * somebody else's memory.  The physical index is the authority: it lists
 * every mapping of every managed page, so an empty list is the answer and
 * an unmanaged page has no list to be empty.
 */
boolean_t
pmap_verify_free(vm_offset_t paddr)
{
	if (!pv_managed((uint64_t) paddr))
		return TRUE;

	return pv_count((uint64_t) paddr) == 0;
}

boolean_t
pmap_page_still_mapped(vm_offset_t paddr)
{
	return !pmap_verify_free(paddr);
}

/*
 * Mark a range modified.
 *
 * The caller is about to write to the pages through some other mapping --
 * the direct map, typically -- and the hardware will not set the dirty bit
 * on the mapping the pager later looks at.  So it is set by hand, and it is
 * set on the entry rather than on a software page record because the pager
 * reads the entry.
 *
 * ⚠️ No TLB shootdown afterwards, and that is correct rather than an
 * omission: the dirty bit is one the processor only ever sets, never clears
 * or tests for permission, so a stale cached entry cannot cause a wrong
 * access.  Clearing it would be a different matter and would need one.
 */
void
pmap_modify_pages(pmap_t pmap, vm_offset_t s, vm_offset_t e)
{
	vm_offset_t	va;

	if (pmap == PMAP_NULL)
		return;

	/*
	 * ⚠️ x86_64_trunc_page and not trunc_page.  The machine-independent
	 * spelling reads `page_mask', a global the VM sets at startup -- so
	 * using it here would make this file depend on the VM being up in
	 * order to tell the VM about pages, and would put a memory load in
	 * front of every iteration of a loop whose page size is a constant on
	 * this machine (#453).
	 */
	for (va = x86_64_trunc_page(s); va < x86_64_round_page(e);
	     va += X86_64_PGBYTES) {
		pt_entry_t	*entry = pmap_walk(pmap->root_pa, va, 0);

		if (entry != PT_ENTRY_NULL && pte_is_valid(*entry))
			*entry |= INTEL_PTE_MOD;
	}
}
