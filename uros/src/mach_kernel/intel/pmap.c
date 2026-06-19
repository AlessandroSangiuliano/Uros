/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */
/*
 * MkLinux
 */
/* CMU_HIST */
/*
 * Revision 2.14.5.9  92/09/15  18:33:27  jeffreyh
 * 	Paragon changes from Intel
 * 	[92/09/15            jeffreyh]
 * 
 * Revision 2.14.5.8  92/09/15  17:20:43  jeffreyh
 * 	Pmap_updates on remote cpus must be processed even if they
 * 	are idle. Some interrupyt handlers (like hd) do access
 * 	remappable memory. Fixed code to ignore idle cpus in case
 * 	of kernel pmap in signal_cpus(). (Like on the Luna H/W).
 * 	[92/07/09            bernadat]
 *
 * Revision 2.14.5.7  92/06/24  17:59:47  jeffreyh
 * 	Added Sharing fault code for the 860. This allows us to remove
 * 	 the flush on the 860 in vm_kern.c.
 * 	[92/06/10            bernard]
 *
 * Revision 2.14.5.6  92/05/28  18:15:44  jeffreyh
 * 	Conditionalized out code that calls flush code for a more 
 * 	recent versions of the i860 then is installed at OSF
 * 
 * Revision 2.14.5.5  92/05/27  01:05:55  jeffreyh
 * 	Changes for a common pmap_bootstrap() for both Paragon and
 * 	iPSC/860 nodes.  Better factoring for i860 vs. iPSC860 and
 * 	PARAGON860.
 * 	[andyp@ssd.intel.com]
 * 	Allow USER access to PERF Counter
 * 	[regnier@ssd.intel.com]
 * 
 * 	Changes to track down problems with aliasing
 * 
 * Revision 2.14.5.4  92/04/08  15:44:09  jeffreyh
 * 	Returned stub for pmap_list_residet_pages
 * 	[92/04/01            jeffreyh]
 * 	Made kernel bss+data cacheable
 * 	[92/04/08            andyp]
 * 
 * Revision 2.14.5.3  92/03/28  10:09:05  jeffreyh
 * 	flush on tlb invalidation 
 * 	[92/03/26            andyp]
 * 	new pmap_bootstrap for i860 
 * 	[92/03/17            andyp]
 * 
 * Revision 2.14.5.2  92/03/03  16:18:13  jeffreyh
 *    Changes from TRUNK
 *    [92/02/26  11:39:43  jeffreyh]
 *
 * Revision 2.15  92/01/14  16:44:01  rpd
 *    Removed pmap_list_resident_pages.
 *    [91/12/31            rpd]
 *
 * Revision 2.14.5.1  92/02/18  19:04:39  jeffreyh
 * 	Increased MOREVM to 32 Megs (device and bootstrap map size increased)
 * 	[91/12/09            bernadat]
 * 
 * 	Include thread.h when compiling with VM_OBJECT_DEBUG
 * 	[91/09/09            bernadat]
 * 
 * 	Added missing calls to vm_object_lock
 * 	[91/08/30            bernadat]
 * 
 * Revision 2.14  91/12/10  16:32:15  jsb
 * 	Fixes from Intel
 * 	[91/12/10  15:51:38  jsb]
 * 
 * Revision 2.13  91/11/18  17:37:09  rvb
 * 	Up morevm for NORMA.
 * 
 * Revision 2.12  91/08/28  11:13:08  jsb
 * 	From Intel SSD: add data cache flush in INVALIDATE_TLB to work around
 * 	some more subtle unknown bug in page table caching; allow user access
 * 	to various bits of I/O space?
 * 	[91/08/26  18:27:04  jsb]
 * 
 * Revision 2.11  91/06/17  15:45:52  jsb
 * 	Fixed reference to XEOD_OFF_PH for i860ipsc dcm module.
 * 	[91/06/17  10:43:40  jsb]
 * 
 * Revision 2.10  91/06/06  17:05:03  jsb
 * 	Defined SPLVM, SPLX as null (vs. splvm, splx) in uniprocessor case.
 * 	[91/05/13  17:12:34  jsb]
 * 
 * Revision 2.9  91/05/18  14:31:14  rpd
 * 	Moved pmap_free_pages, pmap_next_page to a model-dependent file.
 * 	[91/05/15            rpd]
 * 
 * 	Make sure hole_start and hole_end are page-aligned.
 * 	[91/05/01            rpd]
 * 
 * 	Removed pmap_update.
 * 	[91/04/12            rpd]
 * 
 * 	Added inuse_ptepages_count.
 * 	Added vm_page_fictitious_addr assertions.
 * 	[91/04/10            rpd]
 * 	Added check_simple_locks to pmap_expand.
 * 	[91/03/31            rpd]
 * 	Changed vm_page_init to vm_page_insert.
 * 	Added pmap_free_pages, pmap_next_page, pmap_virtual_space.
 * 	[91/03/25            rpd]
 * 
 * Revision 2.8  91/05/14  16:30:24  mrt
 * 	Correcting copyright
 * 
 * Revision 2.7  91/05/08  12:46:31  dbg
 * 	Add volatile declarations where needed.
 * 	Move pmap_valid_page to model_dependent file.
 * 	[91/04/26  14:41:31  dbg]
 * 
 * Revision 2.6  91/03/16  14:47:31  rpd
 * 	Removed some incorrect (?) assertions.
 * 	[91/03/13  14:18:51  rpd]
 * 
 * 	Updated for new kmem_alloc interface.
 * 	[91/03/03            rpd]
 * 	Added continuation argument to VM_PAGE_WAIT.
 * 	[91/02/05            rpd]
 * 
 * Revision 2.5  91/02/14  14:08:11  mrt
 * 	Fixed pmap_expand to use vm_page_grab/VM_PAGE_WAIT.
 * 	[91/01/12            rpd]
 * 
 * Revision 2.4  91/02/05  17:20:34  mrt
 * 	Changed to new Mach copyright
 * 	[91/01/31  18:17:35  mrt]
 * 
 * Revision 2.3  91/01/08  15:12:47  rpd
 * 	Changed pmap_collect to ignore the kernel pmap.
 * 	[91/01/03            rpd]
 * 
 * Revision 2.2  90/12/04  14:50:28  jsb
 * 	First checkin (for intel directory).
 * 	[90/12/03  21:54:31  jsb]
 * 
 * Revision 2.9  90/11/26  14:48:44  rvb
 * 	Slight error in pmap_valid_page.  Pages > last_addr
 * 	must be invalid. (They are probably device buffers.)
 * 	[90/11/23  10:00:56  rvb]
 * 
 * Revision 2.8  90/11/24  15:14:47  jsb
 * 	Replaced "0x1000" in pmap_valid_page with "first_addr".
 * 	[90/11/24  11:49:04  jsb]
 * 
 * Revision 2.7  90/11/05  14:27:27  rpd
 * 	Replace (va < vm_first_phys || va > vm_last_phys) with test
 * 	using valid page.  Otherwise, video buffer memory is treated as
 * 	valid memory and setting dirty bits leads to disasterous results.
 * 	[90/11/05            rvb]
 * 
 * 	Define pmap_valid_page: [0x1000..cnvmem * 1024) and
 * 				[first_avail..)
 * 	as useable memory
 * 	[90/09/05            rvb]
 * 
 * Revision 2.6  90/09/09  14:31:39  rpd
 * 	Use decl_simple_lock_data.
 * 	[90/08/30            rpd]
 * 
 * Revision 2.5  90/08/06  15:07:05  rwd
 * 	Fix bugs in pmap_remove, pmap_protect, phys_attribute routines.
 * 	Allocate pte pages directly from vm_resident page list, via a
 * 	pmap_object.
 * 	[90/07/17            dbg]
 * 
 * Revision 2.4  90/06/19  22:57:46  rpd
 * 	Made MOREVM a variable; increased to 28 meg.
 * 	Commented out pte_to_phys assertions.
 * 	[90/06/04            rpd]
 * 
 * Revision 2.3  90/06/02  14:48:40  rpd
 * 	Added dummy pmap_list_resident_pages, under MACH_VM_DEBUG.
 * 	[90/05/31            rpd]
 * 
 * Revision 2.2  90/05/03  15:37:04  dbg
 * 	Define separate Write and User bits instead of protection codes.
 * 	Write-protect kernel data by invalidating it; the 386 ignores
 * 	write permission in supervisor mode.
 * 	[90/03/25            dbg]
 * 
 * 	Fix pmap_collect to look for VA that maps page table page.
 * 	Since page table pages are allocated with kmem_alloc, their
 * 	virtual and physical addresses are not necessarily the same.
 * 	Rewrite pmap_remove to skip address range when PDE is invalid.
 * 	Combine pmap_remove_all and pmap_copy_on_write into pmap_page_protect.
 * 	Add reference bits.
 * 	[90/03/21            dbg]
 * 
 * 	Fix for pure kernel.  kpde and kptes are dynamically allocated
 * 	by assembly code.  Reverse CHIPBUG test (what was this, Bob?)
 * 	[90/02/14            dbg]
 * 
 * Revision 1.8.1.3  89/12/28  12:43:18  rvb
 * 	v_avail gets phystokv(av_start), in case esym != end.
 * 	[89/12/26            rvb]
 * 
 * Revision 1.8.1.2  89/12/21  17:59:15  rvb
 * 	Revision 1.11  89/11/27  22:54:27  kupfer
 * 	kernacc() moved here from locore (from Lance).
 * 
 * 	Revision 1.10  89/10/24  13:31:38  lance
 * 	Eliminate the boot-time `pause that refreshes'
 * 
 * Revision 1.8  89/09/20  17:26:47  rvb
 * 	The OLIVETTI CACHE bug strikes again.  I am leaving this code in
 * 	as it for now so we can sync up.  BUT all this stuff is going to
 * 	be on a run time switch or a ifdef real soon.
 * 	[89/09/20            rvb]
 * 
 * Revision 1.7  89/07/17  10:38:18  rvb
 * 	pmap_map_bd now flushes the tlb with a call to pmap_update.
 * 	[Lance Berc]
 * 
 * Revision 1.6  89/04/05  12:59:14  rvb
 * 	Can not use zone anymore for directory, since alignment is not
 * 	guaranteed.  Besides the directory is a page.
 * 	[89/03/30            rvb]
 * 
 * 	Move extern out of function scope for gcc.
 * 	[89/03/04            rvb]
 * 
 * Revision 1.5  89/03/09  20:03:25  rpd
 * 	More cleanup.
 * 
 * Revision 1.4  89/02/26  12:33:06  gm0w
 * 	Changes for cleanup.
 * 
 * 31-Dec-88  Robert Baron (rvb) at Carnegie-Mellon University
 *	Derived from MACH2.0 vax release.
 *
 * 17-Jan-88  David Golub (dbg) at Carnegie-Mellon University
 *	Use cpus_idle, not the scheduler's cpu_idle, to determine when a
 *	cpu does not need to be interrupted.  The two are not
 *	synchronized.
 *
 */
/* CMU_ENDHIST */
/*
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */

/*
 *	File:	pmap.c
 *	Author:	Avadis Tevanian, Jr., Michael Wayne Young
 *	(These guys wrote the Vax version)
 *
 *	Physical Map management code for Intel i386/i486 and later.
 *
 *	Manages physical address maps.
 *
 *	In addition to hardware address maps, this
 *	module is called upon to provide software-use-only
 *	maps which may or may not be stored in the same
 *	form as hardware maps.  These pseudo-maps are
 *	used to store intermediate results from copy
 *	operations to and from address spaces.
 *
 *	Since the information managed by this module is
 *	also stored by the logical address mapping module,
 *	this module may throw away valid virtual-to-physical
 *	mappings at almost any time.  However, invalidations
 *	of virtual-to-physical mappings must be done as
 *	requested.
 *
 *	In order to cope with hardware architectures which
 *	make virtual-to-physical map invalidates expensive,
 *	this module may delay invalidate or reduced protection
 *	operations until such time as they are actually
 *	necessary.  This module is given full information as
 *	to which processors are currently using which maps,
 *	and to when physical maps must be made correct.
 */

#include <cpus.h>

#include <string.h>
#include <norma_vm.h>
#include <mach_kdb.h>
#include <mach_ldebug.h>

#include <mach/machine/vm_types.h>

#include <mach/boolean.h>
#include <kern/thread.h>
#include <kern/zalloc.h>

#include <kern/lock.h>
#include <kern/spl.h>

#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#include <mach/vm_param.h>
#include <mach/vm_prot.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_user.h>

#include <mach/machine/vm_param.h>
#include <machine/thread.h>

#include <kern/misc_protos.h>			/* prototyping */
#include <i386/misc_protos.h>
#include <i386/kmap.h>

#if	defined(AT386) && defined(i386)
#include <i386/cpuid.h>
#endif /* defined(AT386) && defined(i386) */

#if	MACH_KDB
#include <ddb/db_command.h>
#include <ddb/db_output.h>
#include <ddb/db_sym.h>
#include <ddb/db_print.h>
#endif	/* MACH_KDB */

#include <kern/xpr.h>

/*
 * Forward declarations for internal functions.
 */
void	pmap_expand(
			pmap_t		map,
			vm_offset_t	v);

extern void	pmap_remove_range(
			pmap_t		pmap,
			vm_offset_t	va,
			pt_entry_t	*spte,
			pt_entry_t	*epte);

void	phys_attribute_clear(
			vm_offset_t	phys,
			int		bits);

boolean_t phys_attribute_test(
			vm_offset_t	phys,
			int		bits);

void pmap_set_modify(vm_offset_t	phys);

void phys_attribute_set(
			vm_offset_t	phys,
			int		bits);


#ifndef	set_dirbase
void	set_dirbase(vm_offset_t	dirbase);
#endif	/* set_dirbase */

#if	i386
#define	PA_TO_PTE(pa)	(pa_to_pte((pa) - VM_MIN_KERNEL_ADDRESS))
#define	iswired(pte)	((pte) & INTEL_PTE_WIRED)

pmap_t	real_pmap[NCPUS];
#endif	/* i386 */

#ifdef	ORC
#define	OLIVETTICACHE	1
#endif	/* ORC */

#ifndef	OLIVETTICACHE
#define	WRITE_PTE(pte_p, pte_entry)		*(pte_p) = (pte_entry);
#define	WRITE_PTE_FAST(pte_p, pte_entry)	*(pte_p) = (pte_entry);
#else	/* OLIVETTICACHE */

/* This gross kludgery is needed for Olivetti XP7 & XP9 boxes to get
 * around an apparent hardware bug. Other than at startup it doesn't
 * affect run-time performance very much, so we leave it in for all
 * machines.
 */
extern	unsigned	*pstart();
#define CACHE_LINE	8
#define CACHE_SIZE	512
#define CACHE_PAGE	0x1000;

extern void	write_pte(
			pt_entry_t	*pte_p,
			pt_entry_t	pte_entry);


#define	WRITE_PTE(pte_p, pte_entry) { write_pte(pte_p, pte_entry); }

void
write_pte(
	pt_entry_t	*pte_p,
	pt_entry_t	pte_entry)
{
	unsigned long count;
	volatile unsigned long hold, *addr1, *addr2;

	if ( pte_entry != *pte_p )
		*pte_p = pte_entry;
	else {
		/* This isn't necessarily the optimal algorithm */
		addr1 = (unsigned long *)pstart;
		for (count = 0; count < CACHE_SIZE; count++) {
			addr2 = addr1 + CACHE_PAGE;
			hold = *addr1;		/* clear cache bank - A - */
			hold = *addr2;		/* clear cache bank - B - */
			addr1 += CACHE_LINE;
		}
	}
}

#define	WRITE_PTE_FAST(pte_p, pte_entry)*pte_p = pte_entry;

#endif	/* OLIVETTICACHE */

/*
 *	Private data structures.
 */

/*
 *	For each vm_page_t, there is a list of all currently
 *	valid virtual mappings of that page.  An entry is
 *	a pv_entry_t; the list is the pv_table.
 */

typedef struct pv_entry {
	struct pv_entry	*next;		/* next pv_entry */
	pmap_t		pmap;		/* pmap where mapping lies */
	vm_offset_t	va;		/* virtual address for mapping */
} *pv_entry_t;

#define PV_ENTRY_NULL	((pv_entry_t) 0)

pv_entry_t	pv_head_table;		/* array of entries, one per page */

/*
 *	pv_list entries are kept on a list that can only be accessed
 *	with the pmap system locked (at SPLVM, not in the cpus_active set).
 *	The list is refilled from the pv_list_zone if it becomes empty.
 */
pv_entry_t	pv_free_list;		/* free list at SPLVM */
decl_simple_lock_data(,pv_free_list_lock)

#define	PV_ALLOC(pv_e) { \
	simple_lock(&pv_free_list_lock); \
	if ((pv_e = pv_free_list) != 0) { \
	    pv_free_list = pv_e->next; \
	} \
	simple_unlock(&pv_free_list_lock); \
}

#define	PV_FREE(pv_e) { \
	simple_lock(&pv_free_list_lock); \
	pv_e->next = pv_free_list; \
	pv_free_list = pv_e; \
	simple_unlock(&pv_free_list_lock); \
}

zone_t		pv_list_zone;		/* zone of pv_entry structures */

/*
 *	#330: type-stable page-table page pool.
 *
 *	pmap_extract() now walks page tables lock-free.  A concurrent
 *	pmap_collect() can unlink (clear the PDE of) an empty PT page while a
 *	reader still holds a pointer into it.  To make that safe WITHOUT a
 *	grace period (which a non-preemptive kernel cannot provide cheaply --
 *	see #336), reclaimed PT pages are never returned to the VM allocator:
 *	pmap_collect() pushes them here and pmap_expand() pops them back.  The
 *	pages stay wired and registered in pmap_object, so a stale reader only
 *	ever dereferences PT-shaped memory (an empty PT page reads as invalid
 *	PTEs; a recycled one reads as some other pmap's PTEs -- never freed or
 *	repurposed memory).  Pages are linked through the unused pageq.next of
 *	a wired page.  pmap_destroy() still frees its PT pages to the allocator
 *	directly: it runs at ref_count==0, so no reader can be walking them.
 */
decl_simple_lock_data(, pmap_pt_pool_lock)
vm_page_t	pmap_pt_pool_list = VM_PAGE_NULL;	/* LIFO of recycled PT pages */
int		pmap_pt_pool_count = 0;

/*
 *	Each entry in the pv_head_table is locked by a bit in the
 *	pv_lock_table.  The lock bits are accessed by the physical
 *	address of the page they lock.
 */

char	*pv_lock_table;		/* pointer to array of bits */
#define pv_lock_table_size(n)	(((n)+BYTE_SIZE-1)/BYTE_SIZE)

/*
 *	First and last physical addresses that we maintain any information
 *	for.  Initialized to zero so that pmap operations done before
 *	pmap_init won't touch any non-existent structures.
 */
vm_offset_t	vm_first_phys = (vm_offset_t) 0;
vm_offset_t	vm_last_phys  = (vm_offset_t) 0;
boolean_t	pmap_initialized = FALSE;/* Has pmap_init completed? */

/*
 *	Index into pv_head table, its lock bits, and the modify/reference
 *	bits starting at vm_first_phys.
 */

#define pa_index(pa)	(atop(pa - vm_first_phys))

#define pai_to_pvh(pai)		(&pv_head_table[pai])
#define lock_pvh_pai(pai)	bit_lock(pai, (void *)pv_lock_table)
#define try_lock_pvh_pai(pai)	bit_lock_try(pai, (void *)pv_lock_table)
#define unlock_pvh_pai(pai)	bit_unlock(pai, (void *)pv_lock_table)

/*
 *	Array of physical page attribites for managed pages.
 *	One byte per physical page.
 */
char	*pmap_phys_attributes;

/*
 *	Physical page attributes.  Copy bits from PTE definition.
 */
#define	PHYS_MODIFIED	INTEL_PTE_MOD	/* page modified */
#define	PHYS_REFERENCED	INTEL_PTE_REF	/* page referenced */

/*
 *	Amount of virtual memory mapped by one
 *	page-directory entry.
 */
#define	PDE_MAPPED_SIZE		(pdetova(1))

/*
 *	We allocate page table pages directly from the VM system
 *	through this object.  It maps physical memory.
 */
vm_object_t	pmap_object = VM_OBJECT_NULL;

/*
 *	Locking and TLB invalidation
 */

/*
 *	Locking Protocols:
 *
 *	There are two structures in the pmap module that need locking:
 *	the pmaps themselves, and the per-page pv_lists (which are locked
 *	by locking the pv_lock_table entry that corresponds to the pv_head
 *	for the list in question.)  Most routines want to lock a pmap and
 *	then do operations in it that require pv_list locking -- however
 *	pmap_remove_all and pmap_copy_on_write operate on a physical page
 *	basis and want to do the locking in the reverse order, i.e. lock
 *	a pv_list and then go through all the pmaps referenced by that list.
 *	To protect against deadlock between these two cases, the pmap_lock
 *	is used.  There are three different locking protocols as a result:
 *
 *  1.  pmap operations only (pmap_extract, pmap_access, ...)  Lock only
 *		the pmap.
 *
 *  2.  pmap-based operations (pmap_enter, pmap_remove, ...)  Get a read
 *		lock on the pmap_lock (shared read), then lock the pmap
 *		and finally the pv_lists as needed [i.e. pmap lock before
 *		pv_list lock.]
 *
 *  3.  pv_list-based operations (pmap_remove_all, pmap_copy_on_write, ...)
 *		Get a write lock on the pmap_lock (exclusive write); this
 *		also guaranteees exclusive access to the pv_lists.  Lock the
 *		pmaps as needed.
 *
 *	At no time may any routine hold more than one pmap lock or more than
 *	one pv_list lock.  Because interrupt level routines can allocate
 *	mbufs and cause pmap_enter's, the pmap_lock and the lock on the
 *	kernel_pmap can only be held at splvm.
 */

#if	NCPUS > 1
/*
 *	We raise the interrupt level to splvm, to block interprocessor
 *	interrupts during pmap operations.  We must take the CPU out of
 *	the cpus_active set while interrupts are blocked.
 */
#define SPLVM(spl)	{ \
	spl = splvm(); \
	mp_disable_preemption(); \
	i_bit_clear(cpu_number(), &cpus_active); \
	mp_enable_preemption(); \
}

#define SPLX(spl)	{ \
	mp_disable_preemption(); \
	i_bit_set(cpu_number(), &cpus_active); \
	mp_enable_preemption(); \
	splx(spl); \
}

/*
 *	#329 stage C: the global pmap_system_lock ("Giant") has been removed.
 *	Cross-pmap pv-list consistency is now provided by the per-page PVH bit
 *	lock alone -- always taken in the canonical PVH->pmap order: protocol-3
 *	writers (pmap_page_protect, phys_attribute_set/clear) take the PVH first
 *	and the per-pmap lock per entry; protocol-2 paths (pmap_enter,
 *	pmap_remove_range) reach the same order via LOCK_PVH_TRY + back-off.
 *	SPLVM/SPLX still bracket every operation: they carry the cpus_active
 *	bookkeeping the TLB-shootdown protocol (PMAP_UPDATE_TLBS) relies on, and
 *	block the IPI while a pmap lock is held.
 */
#define PMAP_READ_LOCK(pmap, spl) {	\
	SPLVM(spl);			\
	simple_lock(&(pmap)->lock);	\
}

#define PMAP_WRITE_LOCK(spl) {		\
	SPLVM(spl);			\
}

#define PMAP_READ_UNLOCK(pmap, spl) {		\
	simple_unlock(&(pmap)->lock);		\
	SPLX(spl);				\
}

#define PMAP_WRITE_UNLOCK(spl) {		\
	SPLX(spl);				\
}

#define PMAP_WRITE_TO_READ_LOCK(pmap) {		\
	simple_lock(&(pmap)->lock);		\
}

/*
 * Pure pv-list reader spanning several pmaps (phys_attribute_test,
 * pmap_verify_free): no per-pmap lock, just SPLVM + the per-page PVH lock
 * (taken by the caller) + lockless PTE reads.
 */
#define PMAP_SYS_READ_LOCK(spl) {	\
	SPLVM(spl);			\
}

#define PMAP_SYS_READ_UNLOCK(spl) {		\
	SPLX(spl);				\
}

#define LOCK_PVH(index)		lock_pvh_pai(index)

#define LOCK_PVH_TRY(index)	try_lock_pvh_pai(index)

#define UNLOCK_PVH(index)	unlock_pvh_pai(index)

#if	USLOCK_DEBUG && MACH_KDB
extern	int	max_lock_loops;
#define LOOP_VAR	int	loop_count = 0

#define LOOP_CHECK(msg, pmap)						\
	if (loop_count++ > max_lock_loops) {				\
		mp_disable_preemption();				\
	    	db_printf("%s: cpu %d looping on pmap %x\n",		\
			  msg, cpu_number(), pmap);			\
		mp_enable_preemption();					\
            	Debugger("deadlock detection");				\
	    	loop_count = 0;						\
	}
#else	/* USLOCK_DEBUG && MACH_KDB */
#define LOOP_VAR
#define LOOP_CHECK(msg, pmap)
#endif	/* USLOCK_DEBUG && MACH_KDB */

#define PMAP_UPDATE_TLBS(pmap, s, e)					\
{									\
	cpu_set	cpu_mask;						\
	cpu_set	users;							\
									\
	mp_disable_preemption();					\
	cpu_mask = 1 << cpu_number();					\
									\
	/* Since the pmap is locked, other updates are locked */ 	\
	/* out, and any pmap_activate has finished. */ 			\
 									\
	/* find other cpus using the pmap */ 				\
	users = (pmap)->cpus_using & ~cpu_mask;        			\
	if (users) { 							\
            LOOP_VAR;							\
	    /* signal them, and wait for them to acknowledge the	\
	     * flush.  We spin on the monotone cpu_update_needed ack	\
	     * (pmap_tlb_ack_outstanding), NOT on the transient		\
	     * cpus_active bit — the latter is reset by the handler so	\
	     * fast that this spin could miss it and hang (#310).  */	\
	    signal_cpus(users, (pmap), (s), (e));      			\
	    while (pmap_tlb_ack_outstanding(users)) {			\
		LOOP_CHECK("PMAP_UPDATE_TLBS", pmap);			\
		continue; 						\
	    }								\
	} 								\
	/* invalidate our own TLB if pmap is in use */ 			\
 									\
	if ((pmap)->cpus_using & cpu_mask) {   				\
	    INVALIDATE_TLB((s), (e)); 					\
	} 								\
									\
	mp_enable_preemption();						\
}

#else	/* NCPUS > 1 */

#if	MACH_RT
#define SPLVM(spl)			{ (spl) = splvm(); }
#define SPLX(spl)			splx (spl)
#else	/* MACH_RT */
#define SPLVM(spl)
#define SPLX(spl)
#endif	/* MACH_RT */

#define PMAP_READ_LOCK(pmap, spl)	SPLVM(spl)
#define PMAP_WRITE_LOCK(spl)		SPLVM(spl)
#define PMAP_READ_UNLOCK(pmap, spl)	SPLX(spl)
#define PMAP_WRITE_UNLOCK(spl)		SPLX(spl)
#define PMAP_WRITE_TO_READ_LOCK(pmap)
#define PMAP_SYS_READ_LOCK(spl)		SPLVM(spl)
#define PMAP_SYS_READ_UNLOCK(spl)	SPLX(spl)

#if	MACH_RT
#define LOCK_PVH(index)			disable_preemption()
#define LOCK_PVH_TRY(index)		(disable_preemption(), TRUE)
#define UNLOCK_PVH(index)		enable_preemption()
#else	/* MACH_RT */
#define LOCK_PVH(index)
#define LOCK_PVH_TRY(index)		(TRUE)
#define UNLOCK_PVH(index)
#endif	/* MACH_RT */

#define PMAP_UPDATE_TLBS(pmap, s, e) { \
	/* invalidate our own TLB if pmap is in use */ \
	if ((pmap)->cpus_using) { \
	    INVALIDATE_TLB((s), (e)); \
	} \
}

#endif	/* NCPUS > 1 */

#define MAX_TBIS_SIZE	32		/* > this -> TBIA */ /* XXX */

#define INVALIDATE_TLB(s, e) { \
	flush_tlb(); \
}


#if	NCPUS > 1
/*
 *	Structures to keep track of pending TLB invalidations
 */
cpu_set			cpus_active;
cpu_set			cpus_idle;
volatile boolean_t	cpu_update_needed[NCPUS];

#define UPDATE_LIST_SIZE	4

struct pmap_update_item {
	pmap_t		pmap;		/* pmap to invalidate */
	vm_offset_t	start;		/* start address to invalidate */
	vm_offset_t	end;		/* end address to invalidate */
} ;

typedef	struct pmap_update_item	*pmap_update_item_t;

/*
 *	List of pmap updates.  If the list overflows,
 *	the last entry is changed to invalidate all.
 */
struct pmap_update_list {
	decl_simple_lock_data(,lock)
	int			count;
	struct pmap_update_item	item[UPDATE_LIST_SIZE];
} ;
typedef	struct pmap_update_list	*pmap_update_list_t;

struct pmap_update_list	cpu_update_list[NCPUS];

extern void signal_cpus(
			cpu_set		use_list,
			pmap_t		pmap,
			vm_offset_t	start,
			vm_offset_t	end);

/*
 *	#304/#310: ack predicate for the PMAP_UPDATE_TLBS initiator spin.
 *
 *	`wait_set` is the set of other CPUs that might be using the pmap.
 *	We only wait for those that are BOTH still in cpus_active (i.e. not
 *	parked at splvm with IPIs masked — re-read every call so a CPU that
 *	drops to splvm falls out of the wait) AND still have their
 *	cpu_update_needed flag set (the monotone ACK: signal_cpus set it
 *	before the IPI, pmap_tlb_shootdown_handler clears it after flushing).
 *	A CPU that parked at splvm before acking will flush on resume via
 *	MARK_CPU_ACTIVE, so dropping it here is safe.
 */
static __inline__ boolean_t
pmap_tlb_ack_outstanding(cpu_set wait_set)
{
	register int	cpu;

	wait_set &= cpus_active;
	for (cpu = 0; wait_set != 0; cpu++, wait_set >>= 1) {
		if ((wait_set & 1) && cpu_update_needed[cpu])
			return (TRUE);
	}
	return (FALSE);
}

#endif	/* NCPUS > 1 */

/*
 *	Other useful macros.
 */
#define current_pmap()		(vm_map_pmap(current_act()->map))
#define pmap_in_use(pmap, cpu)	(((pmap)->cpus_using & (1 << (cpu))) != 0)

struct pmap	kernel_pmap_store;
pmap_t		kernel_pmap;

struct zone	*pmap_zone;		/* zone of pmap structures */

int		pmap_debug = 0;		/* flag for debugging prints */
int		ptes_per_vm_page;	/* number of hardware ptes needed
					   to map one VM page. */
unsigned int	inuse_ptepages_count = 0;	/* debugging */

/*
 *	Pmap cache.  Cache is threaded through ref_count field of pmap.
 *	Max will eventually be constant -- variable for experimentation.
 */
int		pmap_cache_max = 32;
int		pmap_alloc_chunk = 8;
pmap_t		pmap_cache_list;
int		pmap_cache_count;
decl_simple_lock_data(,pmap_cache_lock)

extern char end;

/*
 * Page directory for kernel.
 */
pt_entry_t	*kpde = (pt_entry_t *)1; /* set by start.s - keep out of bss */

#if  DEBUG_ALIAS
#define PMAP_ALIAS_MAX 32
struct pmap_alias {
        vm_offset_t rpc;
        pmap_t pmap;
        vm_offset_t va;
        int cookie;
#define PMAP_ALIAS_COOKIE 0xdeadbeef
} pmap_aliasbuf[PMAP_ALIAS_MAX];
int pmap_alias_index = 0;
extern vm_offset_t get_rpc();

#endif  /* DEBUG_ALIAS */

/*
 * #333: recursive page-table self-map.
 *
 * PDE PTDPTDI points at the page directory itself, so the page tables appear
 * at a fixed virtual window (PTmap) and the page directory appears as an array
 * of PTEs (PTD).  These windows resolve through whatever PD is currently in
 * CR3 -- which always carries a valid recursive slot (set in pmap_bootstrap
 * for the kernel PD and in pmap_create for every task PD) -- so a kernel-VA
 * page-table read through them never faults, even when the kernel PD's
 * direct-map alias lands in the boot hole (#241) at high CPU counts.  Kernel
 * mappings are global (identical in every PD), so reading the current window
 * is always correct for a kernel VA on any pmap.
 *
 * The recursive PDE must NOT be INTEL_PTE_GLOBAL: each pmap's slot points at
 * its own PD, so a stale global TLB entry would shadow the next pmap's tables.
 */
#define PTDPTDI		0x3FF			/* self-map PDE slot (top 4 MB) */
#define PTMAP_BASE	((vm_offset_t)PTDPTDI << PDESHIFT)		   /* 0xFFC00000 */
#define PTD_BASE	(PTMAP_BASE + ((vm_offset_t)PTDPTDI << PTESHIFT))  /* 0xFFFFF000 */
#define PTmap		((pt_entry_t *)PTMAP_BASE)
#define PTD		((pt_entry_t *)PTD_BASE)
#define vtopte(va)	(&PTmap[(vm_offset_t)(va) >> PTESHIFT])
#define vtopde(va)	(&PTD[pdenum(kernel_pmap, (vm_offset_t)(va))])

/*
 *	Given an offset and a map, compute the address of the
 *	pte.  If the address is invalid with respect to the map
 *	then PT_ENTRY_NULL is returned (and the map may need to grow).
 *
 *	This is only used in machine-dependent code.
 */

pt_entry_t *
pmap_pte(
	register pmap_t		pmap,
	register vm_offset_t	addr)
{
	register pt_entry_t	*ptp;
	register pt_entry_t	pte;

	/*
	 * #333: a kernel virtual address is global -- its PDE/PTE are identical
	 * in every pmap -- so resolve it through the always-mapped recursive
	 * window of the current CR3.  This never faults, even when the kernel
	 * PD's direct-map alias is in the boot hole (#241).  A user VA keeps the
	 * direct-map walk (its PD/PT are never in the hole, and it may belong to
	 * a pmap that is not the one currently loaded).
	 */
	if (addr >= VM_MIN_KERNEL_ADDRESS) {
		if ((*vtopde(addr) & INTEL_PTE_VALID) == 0)
			return(PT_ENTRY_NULL);
		return(vtopte(addr));
	}

	pte = pmap->dirbase[pdenum(pmap, addr)];
	if ((pte & INTEL_PTE_VALID) == 0)
		return(PT_ENTRY_NULL);
	ptp = (pt_entry_t *)ptetokv(pte);
	return(&ptp[ptenum(addr)]);

}

#define	pmap_pde(pmap, addr) (&(pmap)->dirbase[pdenum(pmap, addr)])

#define DEBUG_PTE_PAGE	0

#if	DEBUG_PTE_PAGE
void
ptep_check(
	ptep_t	ptep)
{
	register pt_entry_t	*pte, *epte;
	int			ctu, ctw;

	/* check the use and wired counts */
	if (ptep == PTE_PAGE_NULL)
		return;
	pte = pmap_pte(ptep->pmap, ptep->va);
	epte = pte + INTEL_PGBYTES/sizeof(pt_entry_t);
	ctu = 0;
	ctw = 0;
	while (pte < epte) {
		if (pte->pfn != 0) {
			ctu++;
			if (pte->wired)
				ctw++;
		}
		pte += ptes_per_vm_page;
	}

	if (ctu != ptep->use_count || ctw != ptep->wired_count) {
		printf("use %d wired %d - actual use %d wired %d\n",
		    	ptep->use_count, ptep->wired_count, ctu, ctw);
		panic("pte count");
	}
}
#endif	/* DEBUG_PTE_PAGE */

/*
 *	Map memory at initialization.  The physical addresses being
 *	mapped are not managed and are never unmapped.
 *
 *	For now, VM is already on, we only need to map the
 *	specified memory.
 */
vm_offset_t
pmap_map(
	register vm_offset_t	virt,
	register vm_offset_t	start,
	register vm_offset_t	end,
	register vm_prot_t	prot)
{
	register int		ps;

	ps = PAGE_SIZE;
	while (start < end) {
		pmap_enter(kernel_pmap, virt, start, prot, FALSE);
		virt += ps;
		start += ps;
	}
	return(virt);
}

/*
 *	Back-door routine for mapping kernel VM at initialization.  
 * 	Useful for mapping memory outside the range
 *      Sets no-cache, A, D.
 *	[vm_first_phys, vm_last_phys) (i.e., devices).
 *	Otherwise like pmap_map.
 */
vm_offset_t
pmap_map_bd(
	register vm_offset_t	virt,
	register vm_offset_t	start,
	register vm_offset_t	end,
	vm_prot_t		prot)
{
	register pt_entry_t	template;
	register pt_entry_t	*pte;

	template = pa_to_pte(start)
		| INTEL_PTE_NCACHE
		| INTEL_PTE_REF
		| INTEL_PTE_MOD
		| INTEL_PTE_WIRED
		| INTEL_PTE_VALID;
	if (prot & VM_PROT_WRITE)
	    template |= INTEL_PTE_WRITE;

	while (start < end) {
		pte = pmap_pte(kernel_pmap, virt);
		if (pte == PT_ENTRY_NULL)
			panic("pmap_map_bd: Invalid kernel address\n");
		WRITE_PTE_FAST(pte, template)
		pte_increment_pa(template);
		virt += PAGE_SIZE;
		start += PAGE_SIZE;
	}
	flush_tlb();
	return(virt);
}

extern int		cnvmem;
extern	char		*first_avail;
extern	vm_offset_t	virtual_avail, virtual_end;
extern	vm_offset_t	avail_start, avail_end;

/*
 *	Bootstrap the system enough to run with virtual memory.
 *	Map the kernel's code and data, and allocate the system page table.
 *	Called with mapping OFF.  Page_size must already be set.
 *
 *	Parameters:
 *	load_start:	PA where kernel was loaded
 *	avail_start	PA of first available physical page -
 *			   after kernel page tables
 *	avail_end	PA of last available physical page
 *	virtual_avail	VA of first available page -
 *			   after kernel page tables
 *	virtual_end	VA of last available page -
 *			   end of kernel address space
 *
 *	&start_text	start of kernel text
 *	&etext		end of kernel text
 */

/*
 * #307: skip the kernel image / boot page-table hole when picking a new
 * VA for a bootstrap page-table page.  Without this, in NCPUS > 1 builds
 * `virtual_avail` walks straight into kernel .text (the +0x2000 offset for
 * the AP boot pages is enough to make the trick collide) and the second
 * loop in pmap_bootstrap fills the kernel image with zeros.  See
 * project_307_pt_pool_real_fix.md for the eventual rework.
 *
 * hole_start/hole_end are physical addresses (set in model_dep.c::i386_init).
 * va is a kernel virtual address: convert before comparing.
 */
extern vm_offset_t hole_start, hole_end;
static __inline__ vm_offset_t
pmap_bootstrap_skip_hole(vm_offset_t va)
{
	vm_offset_t pa = va - VM_MIN_KERNEL_ADDRESS;
	if (pa >= hole_start && pa < hole_end)
		return hole_end + VM_MIN_KERNEL_ADDRESS;
	return va;
}

void
pmap_bootstrap(
	vm_offset_t	load_start)
{
	vm_offset_t	va, tva;
	pt_entry_t	template;
	pt_entry_t	*pde, *pte, *ptend;
	vm_size_t	morevm;		/* VM space for kernel map */

	/*
	 *	Set ptes_per_vm_page for general use.
	 */
	ptes_per_vm_page = PAGE_SIZE / INTEL_PGBYTES;

	/*
	 *	The kernel's pmap is statically allocated so we don't
	 *	have to use pmap_create, which is unlikely to work
	 *	correctly at this part of the boot sequence.
	 */

	kernel_pmap = &kernel_pmap_store;

	/* #329 stage C: pmap_system_lock removed -- per-page PVH + per-pmap
	 * locks now provide all pmap/pv-list serialization. */

	simple_lock_init(&kernel_pmap->lock, ETAP_VM_PMAP_KERNEL);
	simple_lock_init(&pv_free_list_lock, ETAP_VM_PMAP_FREE);

	kernel_pmap->ref_count = 1;

	/*
	 *	The kernel page directory has been allocated;
	 *	its virtual address is in kpde.
	 *
	 *	Enough kernel page table pages have been allocated
	 *	to map low system memory, kernel text, kernel data/bss,
	 *	kdb's symbols, and the page directory and page tables.
	 *
	 *	No other physical memory has been allocated.
	 */

	/*
	 * Start mapping virtual memory to physical memory, 1-1,
	 * at end of mapped memory.
	 *
	 * HIGHMEM: only map physical RAM up to LOWMEM_LIMIT.
	 * Pages above this threshold have vm_page entries but
	 * no permanent kernel VA — use kmap() for access.
	 */
	{
		vm_offset_t map_end = avail_end;
		if (map_end > LOWMEM_LIMIT)
			map_end = LOWMEM_LIMIT;
		virtual_avail = phystokv(avail_start);
		virtual_end = phystokv(map_end);
	}

	pde = kpde;
	pde += pdenum(kernel_pmap, virtual_avail);

	if (pte_to_pa(*pde) == 0) {
	    /* This pte has not been allocated */
	    pte = 0; ptend = 0;
	}
	else {
	    pte = (pt_entry_t *)ptetokv(*pde);
						/* first pte of page */
	    ptend = pte+NPTES;			/* last pte of page */
	    pte += ptenum(virtual_avail);	/* point to pte that
						   maps first avail VA */
	    pde++;	/* point pde to first empty slot */
	}

	template = pa_to_pte(avail_start)
		| INTEL_PTE_VALID
		| INTEL_PTE_WRITE
		| INTEL_PTE_GLOBAL;

	for (va = virtual_avail; va < virtual_end; va += INTEL_PGBYTES) {
	    if (pte >= ptend) {
		virtual_avail = pmap_bootstrap_skip_hole(virtual_avail);
		pte = (pt_entry_t *)virtual_avail;
		ptend = pte + NPTES;
		virtual_avail = (vm_offset_t)ptend;
		*pde = PA_TO_PTE((vm_offset_t) pte)
			| INTEL_PTE_VALID
			| INTEL_PTE_WRITE
			| INTEL_PTE_GLOBAL;
		pde++;
	    }
	    WRITE_PTE_FAST(pte, template)
	    pte++;
	    pte_increment_pa(template);
	}

	avail_start = virtual_avail - VM_MIN_KERNEL_ADDRESS;

	/*
	 *	Figure out maximum kernel address.
	 *	Kernel virtual space is:
	 *		- at least three times physical memory
	 *		- at least VM_MIN_KERNEL_ADDRESS
	 *		- limited by VM_MAX_KERNEL_ADDRESS
	 */

	/*
	 * Compute overflow-safe: clamp morevm so that
	 * virtual_end + morevm < VM_MAX_KERNEL_ADDRESS to prevent
	 * the page table allocation loop from wrapping around 32-bit.
	 */
	{
		/*
		 * #333: reserve the top page-directory slot (PTDPTDI, the 4 MB
		 * at PTMAP_BASE) for the recursive self-map by capping the
		 * kernel VM range below it.  The kmap window, when active, is
		 * carved further down, so it never collides with PTmap/PTD.
		 */
		vm_size_t max_morevm =
		    trunc_page(PTMAP_BASE) - virtual_end;

		morevm = 3*avail_end;
		if (morevm > max_morevm)
			morevm = max_morevm;

		/*
		 * Leave room for kernel-loaded servers, which have been
		 * linked at addresses from VM_MIN_KERNEL_LOADED_ADDRESS to
		 * VM_MAX_KERNEL_LOADED_ADDRESS.
		 */
		if (VM_MAX_KERNEL_LOADED_ADDRESS > virtual_end) {
			vm_size_t loaded_need =
			    trunc_page(VM_MAX_KERNEL_LOADED_ADDRESS)
			    - virtual_end;
			if (morevm < loaded_need)
				morevm = loaded_need;
			if (morevm > max_morevm)
				morevm = max_morevm;
		}
	}

	*(int *)&template = 0;

	virtual_end += morevm;
	for (tva = va; tva < virtual_end; tva += INTEL_PGBYTES) {
	    if (pte >= ptend) {
		virtual_avail = pmap_bootstrap_skip_hole(virtual_avail);
		pte = (pt_entry_t *)virtual_avail;
		ptend = pte + NPTES;
		virtual_avail = (vm_offset_t)ptend;
		avail_start += INTEL_PGBYTES;
		*pde = PA_TO_PTE((vm_offset_t) pte)
			| INTEL_PTE_VALID
			| INTEL_PTE_WRITE
			| INTEL_PTE_GLOBAL;
		pde++;
	    }
	    WRITE_PTE_FAST(pte, template)
	    pte++;
	}
	virtual_avail = va;
	/*
	 *	c.f. comment above
	 *
	 */
	virtual_end = va + morevm;

	/*
	 * HIGHMEM: reserve a kmap window at the top of kernel VA space.
	 * Page table entries are already allocated (zeroed) by the morevm
	 * loop above.  Shrink virtual_end so the VM system won't use
	 * these VAs for submaps.
	 */
	if (avail_end > LOWMEM_LIMIT) {
		virtual_end -= KMAP_WINDOW_SIZE;
		kmap_init(virtual_end);
	}
	while (pte < ptend)
	    *pte++ = 0;

	/*
	 *	invalidate user virtual addresses 
	 */
	memset((char *)kpde,
	       0,
	       pdenum(kernel_pmap,VM_MIN_KERNEL_ADDRESS)*sizeof(pt_entry_t));
	kernel_pmap->dirbase = kpde;
	printf("Kernel virtual space from 0x%x to 0x%x.\n",
			VM_MIN_KERNEL_ADDRESS, virtual_end);

	printf("Available physical space from 0x%x to 0x%x\n",
			avail_start, avail_end);
	if (avail_end > LOWMEM_LIMIT)
		printf("HIGHMEM: %d MB above lowmem limit (%d MB mapped 1:1)\n",
			(avail_end - LOWMEM_LIMIT) / (1024*1024),
			LOWMEM_LIMIT / (1024*1024));

	/*
	 * #333: install the recursive self-map slot BEFORE the first
	 * kvtophys()/pmap_pte() on a kernel VA -- kvtophys() resolves kernel VAs
	 * through this very window, so it must be live first (chicken-and-egg).
	 * The kernel PD's physical base is its direct-map alias minus the kernel
	 * base (kpde == phystokv(pd_phys)); we cannot call kvtophys() to obtain
	 * it here for the same reason.  Not GLOBAL (see PTDPTDI).
	 */
	kpde[PTDPTDI] = pa_to_pte((vm_offset_t)kpde - VM_MIN_KERNEL_ADDRESS)
		      | INTEL_PTE_VALID | INTEL_PTE_WRITE;

	kernel_pmap->pdirbase = kvtophys((vm_offset_t)kernel_pmap->dirbase);

}

void
pmap_virtual_space(
	vm_offset_t *startp,
	vm_offset_t *endp)
{
	*startp = virtual_avail;
	*endp = virtual_end;
}

/*
 *	Initialize the pmap module.
 *	Called by vm_init, to initialize any structures that the pmap
 *	system needs to map virtual memory.
 */
void
pmap_init(void)
{
	register long		npages;
	vm_offset_t		addr;
	register vm_size_t	s;
	int			i;

	/*
	 *	Allocate memory for the pv_head_table and its lock bits,
	 *	the modify bit array, and the pte_page table.
	 */

	npages = atop(avail_end - avail_start);
	s = (vm_size_t) (sizeof(struct pv_entry) * npages
				+ pv_lock_table_size(npages)
				+ npages);

	s = round_page(s);
	if (kmem_alloc_wired(kernel_map, &addr, s) != KERN_SUCCESS)
		panic("pmap_init");

	memset((char *)addr, 0, s);

	/*
	 *	Allocate the structures first to preserve word-alignment.
	 */
	pv_head_table = (pv_entry_t) addr;
	addr = (vm_offset_t) (pv_head_table + npages);

	pv_lock_table = (char *) addr;
	addr = (vm_offset_t) (pv_lock_table + pv_lock_table_size(npages));

	pmap_phys_attributes = (char *) addr;

	/*
	 *	Create the zone of physical maps,
	 *	and of the physical-to-virtual entries.
	 */
	s = (vm_size_t) sizeof(struct pmap);
	pmap_zone = zinit(s, 400*s, 4096, "pmap"); /* XXX */
	s = (vm_size_t) sizeof(struct pv_entry);
	pv_list_zone = zinit(s, 10000*s, 4096, "pv_list"); /* XXX */

	simple_lock_init(&pmap_pt_pool_lock, ETAP_VM_PMAP);	/* #330 */

#if	NCPUS > 1
	/*
	 *	Set up the pmap request lists
	 */
	for (i = 0; i < NCPUS; i++) {
	    pmap_update_list_t	up = &cpu_update_list[i];

	    simple_lock_init(&up->lock, ETAP_VM_PMAP_UPDATE);
	    up->count = 0;
	}
#endif	/* NCPUS > 1 */

	/*
	 * WORKAROUND-TCG (#190 / #192) — DISABLED.
	 *
	 * Under QEMU TCG the pv_list_zone would re-grow itself during
	 * early boot and panic in zalloc("recursive grow ... before
	 * scheduler is up").  The fix below pre-fills pv_free_list from
	 * a 256 KB BSS buffer so the zone never needs to grow before
	 * the scheduler is up.  An earlier variant used kmem_alloc_wired
	 * for the prefill, but #192 showed the first 4 KB of any fresh
	 * kmem region is a "ghost" page under TCG (writes silently
	 * dropped) — using BSS sidesteps that.
	 *
	 * It's a TCG-only patch; on KVM and real HW the original code
	 * works, and this BSS allocation is wasted.  Flip the #if to
	 * re-enable for TCG runs.
	 */
#if 0	/* TCG-only workaround */
	{
		static char pv_prefill_storage[256 * 1024];
		const vm_size_t	pv_prefill_bytes = sizeof(pv_prefill_storage);
		vm_offset_t	pv_addr = (vm_offset_t)pv_prefill_storage;
		vm_size_t	off;
		memset(pv_prefill_storage, 0, pv_prefill_bytes);
		for (off = 0; off + sizeof(struct pv_entry) <=
		     pv_prefill_bytes; off += sizeof(struct pv_entry)) {
			pv_entry_t e = (pv_entry_t)(pv_addr + off);
			e->next = pv_free_list;
			pv_free_list = e;
		}
	}
#endif

	/*
	 *	Only now, when all of the data structures are allocated,
	 *	can we set vm_first_phys and vm_last_phys.  If we set them
	 *	too soon, the kmem_alloc_wired above will try to use these
	 *	data structures and blow up.
	 */

	vm_first_phys = avail_start;
	vm_last_phys = avail_end;
	pmap_initialized = TRUE;

	/*
	 *	Initializie pmap cache.
	 */
	pmap_cache_list = PMAP_NULL;
	pmap_cache_count = 0;
	simple_lock_init(&pmap_cache_lock, ETAP_VM_PMAP_CACHE);
}

#ifdef	AT386
extern	vm_offset_t	hole_start, hole_end;

#define	pmap_valid_page(x)	((avail_start <= x) && (x < avail_end))
#endif


#define valid_page(x) (pmap_initialized && pmap_valid_page(x))

boolean_t
pmap_verify_free(
	vm_offset_t	phys)
{
	pv_entry_t	pv_h;
	int		pai;
	spl_t		spl;
	boolean_t	result;

	assert(phys != vm_page_fictitious_addr);
	if (!pmap_initialized)
		return(TRUE);

	if (!pmap_valid_page(phys))
		return(FALSE);

	/* #329 stage C: read the pv-list head under the per-page PVH lock
	 * (was the global write lock). */
	PMAP_SYS_READ_LOCK(spl);

	pai = pa_index(phys);
	LOCK_PVH(pai);
	pv_h = pai_to_pvh(pai);

	result = (pv_h->pmap == PMAP_NULL);

	UNLOCK_PVH(pai);
	PMAP_SYS_READ_UNLOCK(spl);

	return(result);
}

/*
 *	Create and return a physical map.
 *
 *	If the size specified for the map
 *	is zero, the map is an actual physical
 *	map, and may be referenced by the
 *	hardware.
 *
 *	If the size specified is non-zero,
 *	the map will be used in software only, and
 *	is bounded by that size.
 */
pmap_t
pmap_create(
	vm_size_t	size)
{
	register pmap_t			p;
	register pmap_statistics_t	stats;

	/*
	 *	A software use-only map doesn't even need a map.
	 */

	if (size != 0) {
		return(PMAP_NULL);
	}

	/*
	 *	Try to get cached pmap, if this fails,
	 *	allocate a pmap struct from the pmap_zone.  Then allocate
	 *	the page descriptor table from the pd_zone.
	 */

	simple_lock(&pmap_cache_lock);
	while ((p = pmap_cache_list) == PMAP_NULL) {

		vm_offset_t		dirbases;
		register int		i;

		simple_unlock(&pmap_cache_lock);

#if	NCPUS > 1
	/*
	 * XXX	NEEDS MP DOING ALLOC logic so that if multiple processors
	 * XXX	get here, only one allocates a chunk of pmaps.
	 * (for now we'll just let it go - safe but wasteful)
	 */
#endif

		/*
		 *	Allocate a chunck of pmaps.  Single kmem_alloc_wired
		 *	operation reduces kernel map fragmentation.
		 */

		if (kmem_alloc_wired(kernel_map, &dirbases,
				     pmap_alloc_chunk * INTEL_PGBYTES)
							!= KERN_SUCCESS)
			panic("pmap_create.1");

		for (i = pmap_alloc_chunk; i > 0 ; i--) {
			p = (pmap_t) zalloc(pmap_zone);
			if (p == PMAP_NULL)
				panic("pmap_create.2");

			/*
			 *	Initialize pmap.  Don't bother with
			 *	ref count as cache list is threaded
			 *	through it.  It'll be set on cache removal.
			 */
			p->dirbase = (pt_entry_t *) dirbases;
			dirbases += INTEL_PGBYTES;
			p->pdirbase = kvtophys((vm_offset_t)p->dirbase);
			/*
			 * #333: the kernel PD's KV alias (kpde) can be unmapped
			 * when it lands in the boot hole (>=8 CPUs), so seed the
			 * shared kernel PDEs from the always-mapped recursive
			 * window (PTD) instead of memcpy(kpde).  User PDEs start
			 * empty; the recursive slot points at this pmap's own PD.
			 */
			{
				int kbase = pdenum(kernel_pmap,
						   VM_MIN_KERNEL_ADDRESS);

				memset(p->dirbase, 0,
				       kbase * sizeof(pt_entry_t));
				memcpy(&p->dirbase[kbase], &PTD[kbase],
				       (NPDES - kbase) * sizeof(pt_entry_t));
				p->dirbase[PTDPTDI] = pa_to_pte(p->pdirbase)
						    | INTEL_PTE_VALID
						    | INTEL_PTE_WRITE;
			}

			simple_lock_init(&p->lock, ETAP_VM_PMAP);
			p->cpus_using = 0;

			/*
			 *	Initialize statistics.
			 */
			stats = &p->stats;
			stats->resident_count = 0;
			stats->wired_count = 0;
			
			/*
			 *	Insert into cache
			 */
			simple_lock(&pmap_cache_lock);
			p->ref_count = (int) pmap_cache_list;
			pmap_cache_list = p;
			pmap_cache_count++;
			simple_unlock(&pmap_cache_lock);
		}
		simple_lock(&pmap_cache_lock);
	}

	assert(p->stats.resident_count == 0);
	assert(p->stats.wired_count == 0);
	p->stats.resident_count = 0;
	p->stats.wired_count = 0;

	pmap_cache_list = (pmap_t) p->ref_count;
	p->ref_count = 1;
	pmap_cache_count--;
	simple_unlock(&pmap_cache_lock);

	return(p);
}

/*
 *	Retire the given physical map from service.
 *	Should only be called if the map contains
 *	no valid mappings.
 */

void
pmap_destroy(
	register pmap_t	p)
{
	register pt_entry_t	*pdep;
	register vm_offset_t	pa;
	register int		c;
	spl_t                   s;
	register vm_page_t	m;

	if (p == PMAP_NULL)
		return;

	SPLVM(s);
	simple_lock(&p->lock);
	c = --p->ref_count;
	if (c == 0) {
		register int    my_cpu;

		mp_disable_preemption();
		my_cpu = cpu_number();

		/* 
		 * If some cpu is not using the physical pmap pointer that it
		 * is supposed to be (see set_dirbase), we might be using the
		 * pmap that is being destroyed! Make sure we are
		 * physically on the right pmap:
		 */
#if	NCPUS > 1
		/* force pmap/cr3 update */
		PMAP_UPDATE_TLBS(p,
				 VM_MIN_ADDRESS,
				 VM_MAX_KERNEL_ADDRESS);
#endif	/* NCPUS > 1 */

		if (real_pmap[my_cpu] == p) {
			PMAP_CPU_CLR(p, my_cpu);
			real_pmap[my_cpu] = kernel_pmap;
			set_cr3(kernel_pmap->pdirbase);
		}
		mp_enable_preemption();
	}
	simple_unlock(&p->lock);
	SPLX(s);

	if (c != 0) {
	    return;	/* still in use */
	}

	/*
	 *	Free the memory maps, then the
	 *	pmap structure.
	 */
	pdep = p->dirbase;
	while (pdep < &p->dirbase[pdenum(p, VM_MIN_KERNEL_ADDRESS)]) {
	    if (*pdep & INTEL_PTE_VALID) {
		pa = pte_to_pa(*pdep);
		vm_object_lock(pmap_object);
		m = vm_page_lookup(pmap_object, pa);
		if (m == VM_PAGE_NULL)
		    panic("pmap_destroy: pte page not in object");
		vm_page_lock_queues();
		vm_page_free(m);
		inuse_ptepages_count--;
		vm_object_unlock(pmap_object);
		vm_page_unlock_queues();

		/*
		 *	Clear pdes, this might be headed for the cache.
		 */
		c = ptes_per_vm_page;
		do {
		    *pdep = 0;
		    pdep++;
		} while (--c > 0);
	    }
	    else {
		pdep += ptes_per_vm_page;
	    }
	
	}
	assert(p->stats.resident_count == 0);
	assert(p->stats.wired_count == 0);

	/*
	 *	Add to cache if not already full
	 */
	simple_lock(&pmap_cache_lock);
	if (pmap_cache_count <= pmap_cache_max) {
		p->ref_count = (int) pmap_cache_list;
		pmap_cache_list = p;
		pmap_cache_count++;
		simple_unlock(&pmap_cache_lock);
	}
	else {
		simple_unlock(&pmap_cache_lock);
		kmem_free(kernel_map, (vm_offset_t)p->dirbase, INTEL_PGBYTES);
		zfree(pmap_zone, (vm_offset_t) p);
	}
}

/*
 *	Add a reference to the specified pmap.
 */

void
pmap_reference(
	register pmap_t	p)
{
	spl_t	s;

	if (p != PMAP_NULL) {
		SPLVM(s);
		simple_lock(&p->lock);
		p->ref_count++;
		simple_unlock(&p->lock);
		SPLX(s);
	}
}

/*
 *	Remove a range of hardware page-table entries.
 *	The entries given are the first (inclusive)
 *	and last (exclusive) entries for the VM pages.
 *	The virtual address is the va for the first pte.
 *
 *	The pmap must be locked.
 *	If the pmap is not the kernel pmap, the range must lie
 *	entirely within one pte-page.  This is NOT checked.
 *	Assumes that the pte-page exists.
 */

/* static */
void
pmap_remove_range(
	pmap_t			pmap,
	vm_offset_t		va,
	pt_entry_t		*spte,
	pt_entry_t		*epte)
{
	register pt_entry_t	*cpte;
	int			num_removed, num_unwired;
	int			pai;
	vm_offset_t		pa;

#if	DEBUG_PTE_PAGE
	if (pmap != kernel_pmap)
		ptep_check(get_pte_page(spte));
#endif	/* DEBUG_PTE_PAGE */
	num_removed = 0;
	num_unwired = 0;

	for (cpte = spte; cpte < epte;
	     cpte += ptes_per_vm_page, va += PAGE_SIZE) {

	    pa = pte_to_pa(*cpte);
	    if (pa == 0)
		continue;

	    if (!valid_page(pa)) {

		/*
		 *	Outside range of managed physical memory.
		 *	Just remove the mappings.
		 */
		register int	i = ptes_per_vm_page;
		register pt_entry_t	*lpte = cpte;

		num_removed++;
		if (iswired(*cpte))
		    num_unwired++;
		do {
		    *lpte = 0;
		    lpte++;
		} while (--i > 0);
		continue;
	    }

	    pai = pa_index(pa);
	    /*
	     * #329: take the PVH in canonical PVH->pmap order.  We hold
	     * pmap->lock; if the PVH is busy, drop ONLY pmap->lock (the caller
	     * keeps spl and the system lock), wait for the PVH, re-acquire
	     * pmap->lock and re-do this slot.  This lets a protocol-3 writer
	     * that holds the PVH and wants pmap->lock make progress, breaking
	     * the lock-order inversion the giant lock used to hide.  *cpte is
	     * re-read on the retry, so a slot that changed while unlocked is
	     * handled correctly.
	     */
	    if (!LOCK_PVH_TRY(pai)) {
		simple_unlock(&pmap->lock);
		LOCK_PVH(pai);
		UNLOCK_PVH(pai);
		simple_lock(&pmap->lock);
		cpte -= ptes_per_vm_page;
		va  -= PAGE_SIZE;
		continue;
	    }

	    num_removed++;
	    if (iswired(*cpte))
		num_unwired++;

	    /*
	     *	Get the modify and reference bits.
	     */
	    {
		register int		i;
		register pt_entry_t	*lpte;

		i = ptes_per_vm_page;
		lpte = cpte;
		do {
		    pmap_phys_attributes[pai] |=
			*lpte & (PHYS_MODIFIED|PHYS_REFERENCED);
		    *lpte = 0;
		    lpte++;
		} while (--i > 0);
	    }

	    /*
	     *	Remove the mapping from the pvlist for
	     *	this physical page.
	     */
	    {
		register pv_entry_t	pv_h, prev, cur;

		pv_h = pai_to_pvh(pai);
		if (pv_h->pmap == PMAP_NULL) {
		    /*
		     * Stale PTE with empty pv_list — PTE was
		     * already cleared above; skip pv removal.
		     */
		    UNLOCK_PVH(pai);
		}
		else if (pv_h->va == va && pv_h->pmap == pmap) {
		    /*
		     * Header is the pv_entry.  Copy the next one
		     * to header and free the next one (we cannot
		     * free the header)
		     */
		    cur = pv_h->next;
		    if (cur != PV_ENTRY_NULL) {
			*pv_h = *cur;
			PV_FREE(cur);
		    }
		    else {
			pv_h->pmap = PMAP_NULL;
		    }
		    UNLOCK_PVH(pai);
		}
		else {
		    boolean_t found = FALSE;
		    cur = pv_h;
		    do {
			prev = cur;
			cur = prev->next;
			if (cur == PV_ENTRY_NULL)
			    break;
		    } while (cur->va != va || cur->pmap != pmap);
		    if (cur != PV_ENTRY_NULL) {
			prev->next = cur->next;
			PV_FREE(cur);
			found = TRUE;
		    }
		    UNLOCK_PVH(pai);
		    /*
		     * If not found, the PTE was orphaned — already
		     * cleared above, nothing more to do.
		     */
		}
	    }
	}

	/*
	 *	Update the counts
	 */
	assert(pmap->stats.resident_count >= num_removed);
	pmap->stats.resident_count -= num_removed;
	assert(pmap->stats.wired_count >= num_unwired);
	pmap->stats.wired_count -= num_unwired;
}

/*
 *	Remove the given range of addresses
 *	from the specified map.
 *
 *	It is assumed that the start and end are properly
 *	rounded to the hardware page size.
 */

void
pmap_remove(
	pmap_t		map,
	vm_offset_t	s,
	vm_offset_t	e)
{
	spl_t			spl;
	register pt_entry_t	*pde;
	register pt_entry_t	*spte, *epte;
	vm_offset_t		l;

	if (map == PMAP_NULL)
		return;

	PMAP_READ_LOCK(map, spl);

	/*
	 *	Invalidate the translation buffer first
	 */
	PMAP_UPDATE_TLBS(map, s, e);

	/* #333: kernel VAs resolve through the fault-free recursive window */
	pde = (map == kernel_pmap) ? vtopde(s) : pmap_pde(map, s);
	while (s < e) {
	    l = (s + PDE_MAPPED_SIZE) & ~(PDE_MAPPED_SIZE-1);
	    if (l == 0 || l > e)
		l = e;
	    if (*pde & INTEL_PTE_VALID) {
		spte = (map == kernel_pmap) ? vtopte(s)
		     : &((pt_entry_t *)ptetokv(*pde))[ptenum(s)];
		epte = &spte[intel_btop(l-s)];
		pmap_remove_range(map, s, spte, epte);
	    }
	    s = l;
	    pde++;
	}

	PMAP_READ_UNLOCK(map, spl);
}

/*
 *	Routine:	pmap_page_protect
 *
 *	Function:
 *		Lower the permission for all mappings to a given
 *		page.
 */
void
pmap_page_protect(
	vm_offset_t	phys,
	vm_prot_t	prot)
{
	pv_entry_t		pv_h, prev;
	register pv_entry_t	pv_e;
	register pt_entry_t	*pte;
	int			pai;
	register pmap_t		pmap;
	spl_t			spl;
	boolean_t		remove;

	assert(phys != vm_page_fictitious_addr);
	if (!valid_page(phys)) {
	    /*
	     *	Not a managed page.
	     */
	    return;
	}

	/*
	 * Determine the new protection.
	 */
	switch (prot) {
	    case VM_PROT_READ:
	    case VM_PROT_READ|VM_PROT_EXECUTE:
		remove = FALSE;
		break;
	    case VM_PROT_ALL:
		return;	/* nothing to do */
	    default:
		remove = TRUE;
		break;
	}

	/*
	 *	Lock the pmap system first, since we will be changing
	 *	several pmaps.
	 */

	PMAP_WRITE_LOCK(spl);

	pai = pa_index(phys);
	pv_h = pai_to_pvh(pai);

	/*
	 * #329: protocol-3 writer -- take this page's PVH lock FIRST (canonical
	 * PVH->pmap order), then the per-pmap lock per entry below.  With
	 * pmap_system_lock gone (stage C) the PVH lock is what serializes this
	 * pv-list walk against concurrent pmap_enter/pmap_remove on this page.
	 */
	LOCK_PVH(pai);

	/*
	 * Walk down PV list, changing or removing all mappings.
	 * We do not have to lock the pv_list because we have
	 * the entire pmap system locked.
	 */
	if (pv_h->pmap != PMAP_NULL) {

	    prev = pv_e = pv_h;
	    do {
		pmap = pv_e->pmap;
		/*
		 * Lock the pmap to block pmap_extract and similar routines.
		 */
		simple_lock(&pmap->lock);

		{
		    register vm_offset_t va;

		    va = pv_e->va;
		    pte = pmap_pte(pmap, va);

		    /*
		     * Consistency checks.
		     */
		    /* assert(*pte & INTEL_PTE_VALID); XXX */
		    /* assert(pte_to_phys(*pte) == phys); */

		    /*
		     * Invalidate TLBs for all CPUs using this mapping.
		     */
		    PMAP_UPDATE_TLBS(pmap, va, va + PAGE_SIZE);
		}

		/*
		 * Remove the mapping if new protection is NONE
		 * or if write-protecting a kernel mapping.
		 */
		if (remove || pmap == kernel_pmap) {
		    /*
		     * Remove the mapping, collecting any modify bits.
		     */
		    if (iswired(*pte))
			panic("pmap_remove_all removing a wired page");

		    {
			register int	i = ptes_per_vm_page;

			do {
			    pmap_phys_attributes[pai] |=
				*pte & (PHYS_MODIFIED|PHYS_REFERENCED);
			    *pte++ = 0;
			} while (--i > 0);
		    }

		    assert(pmap->stats.resident_count >= 1);
		    pmap->stats.resident_count--;

		    /*
		     * Remove the pv_entry.
		     */
		    if (pv_e == pv_h) {
			/*
			 * Fix up head later.
			 */
			pv_h->pmap = PMAP_NULL;
		    }
		    else {
			/*
			 * Delete this entry.
			 */
			prev->next = pv_e->next;
			PV_FREE(pv_e);
		    }
		}
		else {
		    /*
		     * Write-protect.
		     */
		    register int i = ptes_per_vm_page;

		    do {
			*pte &= ~INTEL_PTE_WRITE;
			pte++;
		    } while (--i > 0);

		    /*
		     * Advance prev.
		     */
		    prev = pv_e;
		}

		simple_unlock(&pmap->lock);

	    } while ((pv_e = prev->next) != PV_ENTRY_NULL);

	    /*
	     * If pv_head mapping was removed, fix it up.
	     */
	    if (pv_h->pmap == PMAP_NULL) {
		pv_e = pv_h->next;
		if (pv_e != PV_ENTRY_NULL) {
		    *pv_h = *pv_e;
		    PV_FREE(pv_e);
		}
	    }
	}

	UNLOCK_PVH(pai);

	PMAP_WRITE_UNLOCK(spl);
}

/*
 *	Set the physical protection on the
 *	specified range of this map as requested.
 *	Will not increase permissions.
 */
void
pmap_protect(
	pmap_t		map,
	vm_offset_t	s,
	vm_offset_t	e,
	vm_prot_t	prot)
{
	register pt_entry_t	*pde;
	register pt_entry_t	*spte, *epte;
	vm_offset_t		l;
	spl_t		spl;


	if (map == PMAP_NULL)
		return;

	/*
	 * Determine the new protection.
	 */
	switch (prot) {
	    case VM_PROT_READ:
	    case VM_PROT_READ|VM_PROT_EXECUTE:
		break;
	    case VM_PROT_READ|VM_PROT_WRITE:
	    case VM_PROT_ALL:
		return;	/* nothing to do */
	    default:
		pmap_remove(map, s, e);
		return;
	}

	/*
	 * If write-protecting in the kernel pmap,
	 * remove the mappings; the i386 ignores
	 * the write-permission bit in kernel mode.
	 *
	 * XXX should be #if'd for i386
	 */
#if	defined(AT386) && defined(i386)
	if (cpuid_family == CPUID_FAMILY_386)
#endif	/* defined(AT386) && defined(i386) */
	    if (map == kernel_pmap) {
		    pmap_remove(map, s, e);
		    return;
	    }

	SPLVM(spl);
	simple_lock(&map->lock);

	/*
	 *	Invalidate the translation buffer first
	 */
	PMAP_UPDATE_TLBS(map, s, e);

	/* #333: kernel VAs resolve through the fault-free recursive window */
	pde = (map == kernel_pmap) ? vtopde(s) : pmap_pde(map, s);
	while (s < e) {
	    l = (s + PDE_MAPPED_SIZE) & ~(PDE_MAPPED_SIZE-1);
	    if (l == 0 || l > e)
		l = e;
	    if (*pde & INTEL_PTE_VALID) {
		spte = (map == kernel_pmap) ? vtopte(s)
		     : &((pt_entry_t *)ptetokv(*pde))[ptenum(s)];
		epte = &spte[intel_btop(l-s)];

		while (spte < epte) {
		    if (*spte & INTEL_PTE_VALID)
			*spte &= ~INTEL_PTE_WRITE;
		    spte++;
		}
	    }
	    s = l;
	    pde++;
	}

	simple_unlock(&map->lock);
	SPLX(spl);
}



/*
 *	Insert the given physical page (p) at
 *	the specified virtual address (v) in the
 *	target physical map with the protection requested.
 *
 *	If specified, the page will be wired down, meaning
 *	that the related pte cannot be reclaimed.
 *
 *	NB:  This is the only routine which MAY NOT lazy-evaluate
 *	or lose information.  That is, this routine must actually
 *	insert this page into the given map NOW.
 */
void
pmap_enter(
	register pmap_t		pmap,
	vm_offset_t		v,
	register vm_offset_t	pa,
	vm_prot_t		prot,
	boolean_t		wired)
{
	register pt_entry_t	*pte;
	register pv_entry_t	pv_h;
	register int		i, pai;
	pv_entry_t		pv_e;
	pt_entry_t		template;
	spl_t			spl;
	vm_offset_t		old_pa;

	XPR(0x80000000, "%x/%x: pmap_enter %x/%x/%x\n",
	    current_thread()->top_act,
	    current_thread(), 
	    pmap, v, pa);

	assert(pa != vm_page_fictitious_addr);
	if (pmap_debug)
		printf("pmap(%x, %x)\n", v, pa);
	if (pmap == PMAP_NULL)
		return;

#if	defined(AT386) && defined(i386)
	if (cpuid_family == CPUID_FAMILY_386)
#endif	/* defined(AT386) && defined(i386) */
	if (pmap == kernel_pmap && (prot & VM_PROT_WRITE) == 0
	    && !wired /* hack for io_wire */ ) {
	    /*
	     *	Because the 386 ignores write protection in kernel mode,
	     *	we cannot enter a read-only kernel mapping, and must
	     *	remove an existing mapping if changing it.
	     *
	     *  XXX should be #if'd for i386
	     */
	    PMAP_READ_LOCK(pmap, spl);

	    pte = pmap_pte(pmap, v);
	    if (pte != PT_ENTRY_NULL && pte_to_pa(*pte) != 0) {
		/*
		 *	Invalidate the translation buffer,
		 *	then remove the mapping.
		 */
		PMAP_UPDATE_TLBS(pmap, v, v + PAGE_SIZE);
		pmap_remove_range(pmap, v, pte,
				  pte + ptes_per_vm_page);
	    }
	    PMAP_READ_UNLOCK(pmap, spl);
	    return;
	}

	/*
	 *	Must allocate a new pvlist entry while we're unlocked;
	 *	zalloc may cause pageout (which will lock the pmap system).
	 *	If we determine we need a pvlist entry, we will unlock
	 *	and allocate one.  Then we will retry, throughing away
	 *	the allocated entry later (if we no longer need it).
	 */
	pv_e = PV_ENTRY_NULL;

	/*
	 * #329: structured retry loop (replaces the old "Retry:"/goto).
	 * "continue" restarts the operation after we drop the pmap lock to
	 * acquire a PVH in the canonical PVH->pmap order (or to refill the
	 * pv_entry zone); "break" finishes.  Each pass re-reads the pte, so
	 * any work already committed to the page table is re-derived safely.
	 */
	for (;;) {
	PMAP_READ_LOCK(pmap, spl);

	/*
	 *	Expand pmap to include this pte.  Assume that
	 *	pmap is always expanded to include enough hardware
	 *	pages to map one VM page.
	 */

	while ((pte = pmap_pte(pmap, v)) == PT_ENTRY_NULL) {
		/*
		 *	Must unlock to expand the pmap.
		 */
		PMAP_READ_UNLOCK(pmap, spl);

		pmap_expand(pmap, v);

		PMAP_READ_LOCK(pmap, spl);
	}
	/*
	 *	Special case if the physical page is already mapped
	 *	at this address.
	 */
	old_pa = pte_to_pa(*pte);
	if (old_pa == pa) {
	    /*
	     *	May be changing its wired attribute or protection
	     */
		
	    template = pa_to_pte(pa) | INTEL_PTE_VALID;
	    if (pmap != kernel_pmap)
		template |= INTEL_PTE_USER;
	    if (prot & VM_PROT_WRITE)
		template |= INTEL_PTE_WRITE;
	    if (wired) {
		template |= INTEL_PTE_WIRED;
		if (!iswired(*pte))
		    pmap->stats.wired_count++;
	    }
	    else {
		if (iswired(*pte)) {
		    assert(pmap->stats.wired_count >= 1);
		    pmap->stats.wired_count--;
		}
	    }

	    PMAP_UPDATE_TLBS(pmap, v, v + PAGE_SIZE);
	    i = ptes_per_vm_page;
	    do {
		if (*pte & INTEL_PTE_MOD)
		    template |= INTEL_PTE_MOD;
		WRITE_PTE(pte, template)
		pte++;
		pte_increment_pa(template);
	    } while (--i > 0);

	    break;
	}

	/*
	 *	Outline of code from here:
	 *	   1) If va was mapped, update TLBs, remove the mapping
	 *	      and remove old pvlist entry.
	 *	   2) Add pvlist entry for new mapping
	 *	   3) Enter new mapping.
	 *
	 *	If the old physical page is not managed step 1) is skipped
	 *	(except for updating the TLBs), and the mapping is
	 *	overwritten at step 3).  If the new physical page is not
	 *	managed, step 2) is skipped.
	 */

	if (old_pa != (vm_offset_t) 0) {

	    PMAP_UPDATE_TLBS(pmap, v, v + PAGE_SIZE);

#if	DEBUG_PTE_PAGE
	    if (pmap != kernel_pmap)
		ptep_check(get_pte_page(pte));
#endif	/* DEBUG_PTE_PAGE */

	    /*
	     *	Don't do anything to pages outside valid memory here.
	     *	Instead convince the code that enters a new mapping
	     *	to overwrite the old one.
	     */

	    if (valid_page(old_pa)) {

		pai = pa_index(old_pa);
		/*
		 * #329: take the PVH in canonical PVH->pmap order.  We hold
		 * pmap->lock, so if the PVH is busy we must not spin (that is
		 * the order inversion the giant lock used to hide): drop the
		 * pmap lock, wait for the PVH, then retry.  Nothing has been
		 * committed to the page table yet, so the retry is clean.
		 */
		if (!LOCK_PVH_TRY(pai)) {
		    PMAP_READ_UNLOCK(pmap, spl);
		    LOCK_PVH(pai);
		    UNLOCK_PVH(pai);
		    continue;
		}

		assert(pmap->stats.resident_count >= 1);
		pmap->stats.resident_count--;
	    	if (iswired(*pte)) {
		    assert(pmap->stats.wired_count >= 1);
		    pmap->stats.wired_count--;
		}
		i = ptes_per_vm_page;
		do {
		    pmap_phys_attributes[pai] |=
			*pte & (PHYS_MODIFIED|PHYS_REFERENCED);
		    WRITE_PTE(pte, 0)
		    pte++;
		    pte_increment_pa(template);
		} while (--i > 0);

		/*
		 * Put pte back to beginning of page since it'll be
		 * used later to enter the new page.
		 */
		pte -= ptes_per_vm_page;

		/*
		 *	Remove the mapping from the pvlist for
		 *	this physical page.
		 *
		 *	If the PTE existed but the pv_list has no
		 *	matching entry, the PTE was orphaned (cleared
		 *	above); treat old_pa as unmanaged and proceed.
		 */
		{
		    register pv_entry_t	prev, cur;
		    boolean_t		pv_found = TRUE;

		    pv_h = pai_to_pvh(pai);
		    if (pv_h->pmap == PMAP_NULL) {
			pv_found = FALSE;
		    }
		    else if (pv_h->va == v && pv_h->pmap == pmap) {
			/*
			 * Header is the pv_entry.  Copy the next one
			 * to header and free the next one (we cannot
			 * free the header)
			 */
			cur = pv_h->next;
			if (cur != PV_ENTRY_NULL) {
			    *pv_h = *cur;
			    pv_e = cur;
			}
			else {
			    pv_h->pmap = PMAP_NULL;
			}
		    }
		    else {
			pv_found = FALSE;
			cur = pv_h;
			do {
			    prev = cur;
			    cur = prev->next;
			    if (cur == PV_ENTRY_NULL)
				break;
			} while (cur->va != v || cur->pmap != pmap);
			if (cur != PV_ENTRY_NULL) {
			    prev->next = cur->next;
			    pv_e = cur;
			    pv_found = TRUE;
			}
		    }
		    UNLOCK_PVH(pai);
		    if (!pv_found)
			old_pa = (vm_offset_t) 0;
		}
	    }
	    else {

		/*
		 *	old_pa is not managed.  Pretend it's zero so code
		 *	at Step 3) will enter new mapping (overwriting old
		 *	one).  Do removal part of accounting.
		 */
		old_pa = (vm_offset_t) 0;
		assert(pmap->stats.resident_count >= 1);
		pmap->stats.resident_count--;
		if (iswired(*pte)) {
		    assert(pmap->stats.wired_count >= 1);
		    pmap->stats.wired_count--;
		}
	    }
	}

	if (valid_page(pa)) {

	    /*
	     *	Step 2) Enter the mapping in the PV list for this
	     *	physical page.
	     */

	    pai = pa_index(pa);

	    /* #329: canonical PVH->pmap order (see old-mapping case above). */
	    if (!LOCK_PVH_TRY(pai)) {
		PMAP_READ_UNLOCK(pmap, spl);
		LOCK_PVH(pai);
		UNLOCK_PVH(pai);
		continue;
	    }
	    pv_h = pai_to_pvh(pai);

	    if (pv_h->pmap == PMAP_NULL) {
		/*
		 *	No mappings yet
		 */
		pv_h->va = v;
		pv_h->pmap = pmap;
		pv_h->next = PV_ENTRY_NULL;
	    }
	    else {
#if	DEBUG
		{
		    /*
		     * check that this mapping is not already there
		     * or there is no alias for this mapping in the same map
		     */
		    pv_entry_t	e = pv_h;
		    while (e != PV_ENTRY_NULL) {
			if (e->pmap == pmap && e->va == v)
                            panic("pmap_enter: already in pv_list");
			e = e->next;
		    }
		}
#endif	/* DEBUG */
#if DEBUG_ALIAS
                {
                    /*
                     * check for aliases within the same address space.
                     */
		    pv_entry_t	e = pv_h;
                    vm_offset_t     rpc = get_rpc();

		    while (e != PV_ENTRY_NULL) {
			if (e->pmap == pmap) {
                            /*
                             * log this entry in the alias ring buffer
			     * if it's not there already.
                             */
                            struct pmap_alias *pma;
                            int ii, logit;

                            logit = TRUE;
                            for (ii = 0; ii < pmap_alias_index; ii++) {
                                if (pmap_aliasbuf[ii].rpc == rpc) {
                                    /* found it in the log already */
                                    logit = FALSE;
                                    break;
				}
			    }
                            if (logit) {
                                pma = &pmap_aliasbuf[pmap_alias_index];
                                pma->pmap = pmap;
                                pma->va = v;
                                pma->rpc = rpc;
                                pma->cookie = PMAP_ALIAS_COOKIE;
                                if (++pmap_alias_index >= PMAP_ALIAS_MAX)
                                    panic("pmap_enter: exhausted alias log");
			    }
			}
                        e = e->next;
		    }
		}
#endif /* DEBUG_ALIAS */
		/*
		 *	Add new pv_entry after header.
		 */
		if (pv_e == PV_ENTRY_NULL) {
		    PV_ALLOC(pv_e);
		    if (pv_e == PV_ENTRY_NULL) {
			UNLOCK_PVH(pai);
			PMAP_READ_UNLOCK(pmap, spl);

			/*
			 * Refill from zone.
			 */
			pv_e = (pv_entry_t) zalloc(pv_list_zone);
			continue;
		    }
		}
		pv_e->va = v;
		pv_e->pmap = pmap;
		pv_e->next = pv_h->next;
		pv_h->next = pv_e;
		/*
		 *	Remember that we used the pvlist entry.
		 */
		pv_e = PV_ENTRY_NULL;
	    }
	    UNLOCK_PVH(pai);
	}

	/*
	 * Step 3) Enter and count the mapping.
	 */

	pmap->stats.resident_count++;

	/*
	 *	Build a template to speed up entering -
	 *	only the pfn changes.
	 */
	template = pa_to_pte(pa) | INTEL_PTE_VALID;
	if (pmap != kernel_pmap)
		template |= INTEL_PTE_USER;
	if (prot & VM_PROT_WRITE)
		template |= INTEL_PTE_WRITE;
	if (wired) {
		template |= INTEL_PTE_WIRED;
		pmap->stats.wired_count++;
	}
	i = ptes_per_vm_page;
	do {
		WRITE_PTE(pte, template)
		pte++;
		pte_increment_pa(template);
	} while (--i > 0);

	break;
	}	/* for (;;) retry loop */

	if (pv_e != PV_ENTRY_NULL) {
	    PV_FREE(pv_e);
	}

	PMAP_READ_UNLOCK(pmap, spl);
}

/*
 *	Routine:	pmap_change_wiring
 *	Function:	Change the wiring attribute for a map/virtual-address
 *			pair.
 *	In/out conditions:
 *			The mapping must already exist in the pmap.
 */
void
pmap_change_wiring(
	register pmap_t	map,
	vm_offset_t	v,
	boolean_t	wired)
{
	register pt_entry_t	*pte;
	register int		i;
	spl_t			spl;

	/*
	 *	We must grab the pmap system lock because we may
	 *	change a pte_page queue.
	 */
	PMAP_READ_LOCK(map, spl);

	if ((pte = pmap_pte(map, v)) == PT_ENTRY_NULL)
		panic("pmap_change_wiring: pte missing");

	if (wired && !iswired(*pte)) {
	    /*
	     *	wiring down mapping
	     */
	    map->stats.wired_count++;
	    i = ptes_per_vm_page;
	    do {
		*pte++ |= INTEL_PTE_WIRED;
	    } while (--i > 0);
	}
	else if (!wired && iswired(*pte)) {
	    /*
	     *	unwiring mapping
	     */
	    assert(map->stats.wired_count >= 1);
	    map->stats.wired_count--;
	    i = ptes_per_vm_page;
	    do {
		*pte++ &= ~INTEL_PTE_WIRED;
	    } while (--i > 0);
	}

	PMAP_READ_UNLOCK(map, spl);
}

/*
 *	Routine:	pmap_extract
 *	Function:
 *		Extract the physical page address associated
 *		with the given map/virtual_address pair.
 */

vm_offset_t
pmap_extract(
	register pmap_t	pmap,
	vm_offset_t	va)
{
	register pt_entry_t	*pte;
	register vm_offset_t	pa;

	/*
	 * #330: lock-free read.  pmap->lock no longer guards readers and no spl
	 * is needed: page-table pages are type-stable (pmap_collect recycles
	 * empty ones onto pmap_pt_pool_list and never returns them to the VM
	 * allocator), so this walk can never dereference a page repurposed for
	 * unrelated data -- no use-after-free.  The PDE/PTE words are 4-byte
	 * aligned, so each load is atomic: we observe old-or-new, never torn.
	 * The result is inherently racy (the mapping may change right after we
	 * return); callers needing a stable answer hold the relevant vm_map
	 * lock, which also keeps a live mapping's PT page out of pmap_collect.
	 */
	if ((pte = pmap_pte(pmap, va)) == PT_ENTRY_NULL)
	    pa = (vm_offset_t) 0;
	else if (!(*pte & INTEL_PTE_VALID))
	    pa = (vm_offset_t) 0;
	else
	    pa = pte_to_pa(*pte) + (va & INTEL_OFFMASK);
	return(pa);
}

/*
 *	Routine:	pmap_expand
 *
 *	Expands a pmap to be able to map the specified virtual address.
 *
 *	Allocates new virtual memory for the P0 or P1 portion of the
 *	pmap, then re-maps the physical pages that were in the old
 *	pmap to be in the new pmap.
 *
 *	Must be called with the pmap system and the pmap unlocked,
 *	since these must be unlocked to use vm_allocate or vm_deallocate.
 *	Thus it must be called in a loop that checks whether the map
 *	has been expanded enough.
 *	(We won't loop forever, since page tables aren't shrunk.)
 */
void
pmap_expand(
	register pmap_t		map,
	register vm_offset_t	v)
{
	pt_entry_t		*pdp;
	register vm_page_t	m;
	register vm_offset_t	pa;
	register int		i;
	spl_t			spl;

	if (map == kernel_pmap)
	    panic("pmap_expand");

	/*
	 *	We cannot allocate the pmap_object in pmap_init,
	 *	because it is called before the zone package is up.
	 *	Allocate it now if it is missing.
	 */
	if (pmap_object == VM_OBJECT_NULL)
	    pmap_object = vm_object_allocate(avail_end);

	/*
	 *	#330: reuse a recycled PT page from the type-stable pool if one
	 *	is available -- it is already wired and registered in pmap_object.
	 *	Otherwise grab a fresh page and install it the usual way.
	 */
	m = VM_PAGE_NULL;
	simple_lock(&pmap_pt_pool_lock);
	if (pmap_pt_pool_list != VM_PAGE_NULL) {
		m = pmap_pt_pool_list;
		pmap_pt_pool_list = (vm_page_t) m->pageq.next;
		pmap_pt_pool_count--;
	}
	simple_unlock(&pmap_pt_pool_lock);

	if (m != VM_PAGE_NULL) {
		pa = m->phys_addr;
	} else {
		/*
		 *	Allocate a VM page for the level 2 page table entries.
		 */
		while ((m = vm_page_grab()) == VM_PAGE_NULL)
			VM_PAGE_WAIT();

		/*
		 *	Map the page to its physical address so that it
		 *	can be found later.
		 */
		pa = m->phys_addr;
		vm_object_lock(pmap_object);
		vm_page_insert(m, pmap_object, pa);
		vm_page_lock_queues();
		vm_page_wire(m);
		inuse_ptepages_count++;
		vm_object_unlock(pmap_object);
		vm_page_unlock_queues();
	}

	/*
	 *	Zero the page.
	 */
	{
		vm_offset_t ptva = kmap(pa);
		memset((void *)ptva, 0, PAGE_SIZE);
		kunmap(pa);
	}
	PMAP_READ_LOCK(map, spl);
	/*
	 *	See if someone else expanded us first
	 */
	if (pmap_pte(map, v) != PT_ENTRY_NULL) {
		PMAP_READ_UNLOCK(map, spl);
		/*
		 *	#330: return the unused page to the type-stable pool
		 *	(it is wired, zeroed and in pmap_object) rather than to
		 *	the VM allocator -- keeps every PT page type-stable.
		 */
		simple_lock(&pmap_pt_pool_lock);
		m->pageq.next = (queue_entry_t) pmap_pt_pool_list;
		pmap_pt_pool_list = m;
		pmap_pt_pool_count++;
		simple_unlock(&pmap_pt_pool_lock);
		return;
	}

	/*
	 *	Set the page directory entry for this page table.
	 *	If we have allocated more than one hardware page,
	 *	set several page directory entries.
	 */

	i = ptes_per_vm_page;
	pdp = &map->dirbase[pdenum(map, v) & ~(i-1)];
	do {
	    *pdp = pa_to_pte(pa)
		| INTEL_PTE_VALID
		| INTEL_PTE_USER
		| INTEL_PTE_WRITE;
	    pdp++;
	    pa += INTEL_PGBYTES;
	} while (--i > 0);
	PMAP_READ_UNLOCK(map, spl);
	return;
}

/*
 *	Copy the range specified by src_addr/len
 *	from the source map to the range dst_addr/len
 *	in the destination map.
 *
 *	This routine is only advisory and need not do anything.
 */
#if	0
void
pmap_copy(
	pmap_t		dst_pmap,
	pmap_t		src_pmap,
	vm_offset_t	dst_addr,
	vm_size_t	len,
	vm_offset_t	src_addr)
{
#ifdef	lint
	dst_pmap++; src_pmap++; dst_addr++; len++; src_addr++;
#endif	/* lint */
}
#endif/* 	0 */

int	collect_ref;
int	collect_unref;

/*
 *	Routine:	pmap_collect
 *	Function:
 *		Garbage collects the physical map system for
 *		pages which are no longer used.
 *		Success need not be guaranteed -- that is, there
 *		may well be pages which are not referenced, but
 *		others may be collected.
 *	Usage:
 *		Called by the pageout daemon when pages are scarce.
 */
void
pmap_collect(
	pmap_t 		p)
{
	register pt_entry_t	*pdp, *ptp;
	pt_entry_t		*eptp;
	vm_offset_t		pa;
	int			wired;
	spl_t                   spl;

	if (p == PMAP_NULL)
		return;

	if (p == kernel_pmap)
		return;

	/*
	 *	Garbage collect map.
	 */
	PMAP_READ_LOCK(p, spl);
	PMAP_UPDATE_TLBS(p, VM_MIN_ADDRESS, VM_MAX_ADDRESS);

	for (pdp = p->dirbase;
	     pdp < &p->dirbase[pdenum(p, VM_MIN_KERNEL_ADDRESS)];
	     pdp += ptes_per_vm_page)
	{
	    if (*pdp & INTEL_PTE_VALID) 
	      if(*pdp & INTEL_PTE_REF) {
		*pdp &= ~INTEL_PTE_REF;
		collect_ref++;
	      } else {
		collect_unref++;
		pa = pte_to_pa(*pdp);
		ptp = (pt_entry_t *)phystokv(pa);
		eptp = ptp + NPTES*ptes_per_vm_page;

		/*
		 * If the pte page has any wired mappings, we cannot
		 * free it.
		 */
		wired = 0;
		{
		    register pt_entry_t *ptep;
		    for (ptep = ptp; ptep < eptp; ptep++) {
			if (iswired(*ptep)) {
			    wired = 1;
			    break;
			}
		    }
		}
		if (!wired) {
		    /*
		     * Remove the virtual addresses mapped by this pte page.
		     */
		    pmap_remove_range(p,
				pdetova(pdp - p->dirbase),
				ptp,
				eptp);

		    /*
		     * Invalidate the page directory pointer.
		     */
		    {
			register int i = ptes_per_vm_page;
			register pt_entry_t *pdep = pdp;
			do {
			    *pdep++ = 0;
			} while (--i > 0);
		    }

		    PMAP_READ_UNLOCK(p, spl);

		    /*
		     * #330: the PDE is now cleared, so the PT page is unlinked.
		     * A lock-free pmap_extract on another CPU may still be
		     * walking it, so we cannot return it to the VM allocator.
		     * Recycle it onto the type-stable pool instead: it stays
		     * wired and in pmap_object, its PTEs are all zero (cleared
		     * by pmap_remove_range above), and pmap_expand will reuse
		     * it.  A stale reader thus only ever reads PT-shaped memory.
		     */
		    {
			register vm_page_t m;

			vm_object_lock(pmap_object);
			m = vm_page_lookup(pmap_object, pa);
			if (m == VM_PAGE_NULL)
			    panic("pmap_collect: pte page not in object");
			vm_object_unlock(pmap_object);

			simple_lock(&pmap_pt_pool_lock);
			m->pageq.next = (queue_entry_t) pmap_pt_pool_list;
			pmap_pt_pool_list = m;
			pmap_pt_pool_count++;
			simple_unlock(&pmap_pt_pool_lock);
		    }

		    PMAP_READ_LOCK(p, spl);
		}
	    }
	}
	PMAP_READ_UNLOCK(p, spl);
	return;

}

/*
 *	Routine:	pmap_kernel
 *	Function:
 *		Returns the physical map handle for the kernel.
 */
#if	0
pmap_t
pmap_kernel(void)
{
    	return (kernel_pmap);
}
#endif/* 	0 */

/*
 *	pmap_zero_page zeros the specified (machine independent) page.
 *	See machine/phys.c or machine/phys.s for implementation.
 */
#if	0
void
pmap_zero_page(
	register vm_offset_t	phys)
{
	register int	i;

	assert(phys != vm_page_fictitious_addr);
	i = PAGE_SIZE / INTEL_PGBYTES;
	phys = intel_pfn(phys);

	while (i--)
		zero_phys(phys++);
}
#endif/* 	0 */

/*
 *	pmap_copy_page copies the specified (machine independent) page.
 *	See machine/phys.c or machine/phys.s for implementation.
 */
#if	0
void
pmap_copy_page(
	vm_offset_t	src,
	vm_offset_t	dst)
{
	int	i;

	assert(src != vm_page_fictitious_addr);
	assert(dst != vm_page_fictitious_addr);
	i = PAGE_SIZE / INTEL_PGBYTES;

	while (i--) {
		copy_phys(intel_pfn(src), intel_pfn(dst));
		src += INTEL_PGBYTES;
		dst += INTEL_PGBYTES;
	}
}
#endif/* 	0 */

/*
 *	Routine:	pmap_pageable
 *	Function:
 *		Make the specified pages (by pmap, offset)
 *		pageable (or not) as requested.
 *
 *		A page which is not pageable may not take
 *		a fault; therefore, its page table entry
 *		must remain valid for the duration.
 *
 *		This routine is merely advisory; pmap_enter
 *		will specify that these pages are to be wired
 *		down (or not) as appropriate.
 */
void
pmap_pageable(
	pmap_t		pmap,
	vm_offset_t	start,
	vm_offset_t	end,
	boolean_t	pageable)
{
#ifdef	lint
	pmap++; start++; end++; pageable++;
#endif	/* lint */
}

/*
 *	Clear specified attribute bits.
 */
void
phys_attribute_clear(
	vm_offset_t	phys,
	int		bits)
{
	pv_entry_t		pv_h;
	register pv_entry_t	pv_e;
	register pt_entry_t	*pte;
	int			pai;
	register pmap_t		pmap;
	spl_t			spl;

	assert(phys != vm_page_fictitious_addr);
	if (!valid_page(phys)) {
	    /*
	     *	Not a managed page.
	     */
	    return;
	}

	/*
	 *	Lock the pmap system first, since we will be changing
	 *	several pmaps.
	 */

	PMAP_WRITE_LOCK(spl);

	pai = pa_index(phys);
	pv_h = pai_to_pvh(pai);

	/* #329 stage A: canonical PVH->pmap order (redundant under global write). */
	LOCK_PVH(pai);

	/*
	 * Walk down PV list, clearing all modify or reference bits.
	 * We do not have to lock the pv_list because we have
	 * the entire pmap system locked.
	 */
	if (pv_h->pmap != PMAP_NULL) {
	    /*
	     * There are some mappings.
	     */
	    for (pv_e = pv_h; pv_e != PV_ENTRY_NULL; pv_e = pv_e->next) {

		pmap = pv_e->pmap;
		/*
		 * Lock the pmap to block pmap_extract and similar routines.
		 */
		simple_lock(&pmap->lock);

		{
		    register vm_offset_t va;

		    va = pv_e->va;
		    pte = pmap_pte(pmap, va);

#if	0
		    /*
		     * Consistency checks.
		     */
		    assert(*pte & INTEL_PTE_VALID);
		    /* assert(pte_to_phys(*pte) == phys); */
#endif

		    /*
		     * Invalidate TLBs for the mapping we are about to alter.
		     *
		     * #304: clearing the REFERENCE (accessed) bit is advisory —
		     * it only feeds the pageout clock, and a stale accessed bit
		     * in another CPU's TLB just makes a page look more recently
		     * used (it gets re-faulted, never corrupted).  vm_pageout
		     * calls pmap_clear_reference() on every page it scans, so a
		     * cross-CPU shootdown per mapping here turned into a storm of
		     * hundreds of thousands of IPIs that stalled SMP boot.  Do a
		     * local flush only for reference-only clears (UP-equivalent),
		     * and reserve the full cross-CPU shootdown for the MODIFY
		     * (dirty) bit, where another CPU's stale dirty TLB entry
		     * could write back over a page we have already cleaned.
		     */
		    if (bits & PHYS_MODIFIED) {
			PMAP_UPDATE_TLBS(pmap, va, va + PAGE_SIZE);
		    } else {
			INVALIDATE_TLB(va, va + PAGE_SIZE);
		    }
		}

		/*
		 * Clear modify or reference bits.
		 */
		{
		    register int	i = ptes_per_vm_page;
		    do {
			*pte++ &= ~bits;
		    } while (--i > 0);
		}
		simple_unlock(&pmap->lock);
	    }
	}

	pmap_phys_attributes[pai] &= ~bits;

	UNLOCK_PVH(pai);

	PMAP_WRITE_UNLOCK(spl);
}

/*
 *	Check specified attribute bits.
 */
boolean_t
phys_attribute_test(
	vm_offset_t	phys,
	int		bits)
{
	pv_entry_t		pv_h;
	register pv_entry_t	pv_e;
	register pt_entry_t	*pte;
	int			pai;
	register pmap_t		pmap;
	spl_t			spl;

	assert(phys != vm_page_fictitious_addr);
	if (!valid_page(phys)) {
	    /*
	     *	Not a managed page.
	     */
	    return (FALSE);
	}

	/*
	 * #329: pure read of one page's ref/mod state, serialized by that
	 * page's PVH lock alone (originally protocol-3 under the global write
	 * lock; since stage C there is no global lock):
	 *
	 *  - the PVH lock keeps this page's pv list stable against concurrent
	 *    protocol-2 inserts/removes and protocol-3 writers (all take it);
	 *  - a held pv entry implies the mapping (hence its page-table page) is
	 *    live, so each PTE is read locklessly (atomic on x86) with no
	 *    per-pmap lock -- which also avoids any pmap<->PVH order inversion.
	 */
	pai = pa_index(phys);
	pv_h = pai_to_pvh(pai);

	PMAP_SYS_READ_LOCK(spl);
	LOCK_PVH(pai);

	if (pmap_phys_attributes[pai] & bits) {
	    UNLOCK_PVH(pai);
	    PMAP_SYS_READ_UNLOCK(spl);
	    return (TRUE);
	}

	/*
	 * Walk down PV list, checking all mappings.  The PVH lock keeps the
	 * list stable; PTEs are read without the per-pmap lock (atomic read,
	 * page-table page kept live by the held mapping).
	 */
	if (pv_h->pmap != PMAP_NULL) {
	    /*
	     * There are some mappings.
	     */
	    for (pv_e = pv_h; pv_e != PV_ENTRY_NULL; pv_e = pv_e->next) {

		pmap = pv_e->pmap;

		{
		    register vm_offset_t va;

		    va = pv_e->va;
		    pte = pmap_pte(pmap, va);

#if	0
		    /*
		     * Consistency checks.
		     */
		    assert(*pte & INTEL_PTE_VALID);
		    /* assert(pte_to_phys(*pte) == phys); */
#endif
		}

		/*
		 * Check modify or reference bits.
		 */
		{
		    register int	i = ptes_per_vm_page;

		    do {
			if (*pte++ & bits) {
			    UNLOCK_PVH(pai);
			    PMAP_SYS_READ_UNLOCK(spl);
			    return (TRUE);
			}
		    } while (--i > 0);
		}
	    }
	}
	UNLOCK_PVH(pai);
	PMAP_SYS_READ_UNLOCK(spl);
	return (FALSE);
}

/*
 *	Set specified attribute bits.
 */
void
phys_attribute_set(
	vm_offset_t	phys,
	int		bits)
{
	int			spl;
	int			pai;

	assert(phys != vm_page_fictitious_addr);
	if (!valid_page(phys)) {
	    /*
	     *	Not a managed page.
	     */
	    return;
	}

	/*
	 *	Lock the pmap system and set the requested bits in
	 *	the phys attributes array.  Don't need to bother with
	 *	ptes because the test routine looks here first.
	 */

	PMAP_WRITE_LOCK(spl);
	pai = pa_index(phys);
	/* #329 stage A: canonical PVH->pmap order (redundant under global write). */
	LOCK_PVH(pai);
	pmap_phys_attributes[pai] |= bits;
	UNLOCK_PVH(pai);
	PMAP_WRITE_UNLOCK(spl);
}

/*
 *	Set the modify bit on the specified physical page.
 */

void pmap_set_modify(
	register vm_offset_t	phys)
{
	phys_attribute_set(phys, PHYS_MODIFIED);
}

/*
 *	Clear the modify bits on the specified physical page.
 */

void
pmap_clear_modify(
	register vm_offset_t	phys)
{
	phys_attribute_clear(phys, PHYS_MODIFIED);
}

/*
 *	pmap_is_modified:
 *
 *	Return whether or not the specified physical page is modified
 *	by any physical maps.
 */

boolean_t
pmap_is_modified(
	register vm_offset_t	phys)
{
	return (phys_attribute_test(phys, PHYS_MODIFIED));
}

/*
 *	pmap_clear_reference:
 *
 *	Clear the reference bit on the specified physical page.
 */

void
pmap_clear_reference(
	vm_offset_t	phys)
{
	phys_attribute_clear(phys, PHYS_REFERENCED);
}

/*
 *	pmap_is_referenced:
 *
 *	Return whether or not the specified physical page is referenced
 *	by any physical maps.
 */

boolean_t
pmap_is_referenced(
	vm_offset_t	phys)
{
	return (phys_attribute_test(phys, PHYS_REFERENCED));
}

/*
 *	Set the modify bit on the specified range
 *	of this map as requested.
 *
 *	This optimization stands only if each time the dirty bit
 *	in vm_page_t is tested, it is also tested in the pmap.
 */
void
pmap_modify_pages(
	pmap_t		map,
	vm_offset_t	s,
	vm_offset_t	e)
{
	spl_t			spl;
	register pt_entry_t	*pde;
	register pt_entry_t	*spte, *epte;
	vm_offset_t		l;

	if (map == PMAP_NULL)
		return;

	PMAP_READ_LOCK(map, spl);

	/*
	 *	Invalidate the translation buffer first
	 */
	PMAP_UPDATE_TLBS(map, s, e);

	/* #333: kernel VAs resolve through the fault-free recursive window */
	pde = (map == kernel_pmap) ? vtopde(s) : pmap_pde(map, s);
	while (s < e) {
	    l = (s + PDE_MAPPED_SIZE) & ~(PDE_MAPPED_SIZE-1);
	    if (l == 0 || l > e)
		l = e;
	    if (*pde & INTEL_PTE_VALID) {
		spte = (map == kernel_pmap) ? vtopte(s)
		     : &((pt_entry_t *)ptetokv(*pde))[ptenum(s)];
		epte = &spte[intel_btop(l-s)];
		while (spte < epte) {
		    if (*spte & INTEL_PTE_VALID) {
			*spte |= (INTEL_PTE_MOD | INTEL_PTE_WRITE);
		    }
		    spte++;
		}
	    }
	    s = l;
	    pde++;
	}
	PMAP_READ_UNLOCK(map, spl);
}


#if	NCPUS > 1
/*
*	    TLB Coherence Code (TLB "shootdown" code)
* 
* Threads that belong to the same task share the same address space and
* hence share a pmap.  However, they  may run on distinct cpus and thus
* have distinct TLBs that cache page table entries. In order to guarantee
* the TLBs are consistent, whenever a pmap is changed, all threads that
* are active in that pmap must have their TLB updated. To keep track of
* this information, the set of cpus that are currently using a pmap is
* maintained within each pmap structure (cpus_using). Pmap_activate() and
* pmap_deactivate add and remove, respectively, a cpu from this set.
* Since the TLBs are not addressable over the bus, each processor must
* flush its own TLB; a processor that needs to invalidate another TLB
* needs to interrupt the processor that owns that TLB to signal the
* update.
* 
* Whenever a pmap is updated, the lock on that pmap is locked, and all
* cpus using the pmap are signaled to invalidate. All threads that need
* to activate a pmap must wait for the lock to clear to await any updates
* in progress before using the pmap. They must ACQUIRE the lock to add
* their cpu to the cpus_using set. An implicit assumption made
* throughout the TLB code is that all kernel code that runs at or higher
* than splvm blocks out update interrupts, and that such code does not
* touch pageable pages.
* 
* A shootdown interrupt serves another function besides signaling a
* processor to invalidate. The interrupt routine (pmap_update_interrupt)
* waits for the both the pmap lock (and the kernel pmap lock) to clear,
* preventing user code from making implicit pmap updates while the
* sending processor is performing its update. (This could happen via a
* user data write reference that turns on the modify bit in the page
* table). It must wait for any kernel updates that may have started
* concurrently with a user pmap update because the IPC code
* changes mappings.
* Spinning on the VALUES of the locks is sufficient (rather than
* having to acquire the locks) because any updates that occur subsequent
* to finding the lock unlocked will be signaled via another interrupt.
* (This assumes the interrupt is cleared before the low level interrupt code 
* calls pmap_update_interrupt()). 
* 
* The signaling processor must wait for any implicit updates in progress
* to terminate before continuing with its update. Thus it must wait for an
* acknowledgement of the interrupt from each processor for which such
* references could be made. For maintaining this information, a set
* cpus_active is used. A cpu is in this set if and only if it can 
* use a pmap. When pmap_update_interrupt() is entered, a cpu is removed from
* this set; when all such cpus are removed, it is safe to update.
* 
* Before attempting to acquire the update lock on a pmap, a cpu (A) must
* be at least at the priority of the interprocessor interrupt
* (splip<=splvm). Otherwise, A could grab a lock and be interrupted by a
* kernel update; it would spin forever in pmap_update_interrupt() trying
* to acquire the user pmap lock it had already acquired. Furthermore A
* must remove itself from cpus_active.  Otherwise, another cpu holding
* the lock (B) could be in the process of sending an update signal to A,
* and thus be waiting for A to remove itself from cpus_active. If A is
* spinning on the lock at priority this will never happen and a deadlock
* will result.
*/

/*
 *	Signal another CPU that it must flush its TLB
 */
void
signal_cpus(
	cpu_set		use_list,
	pmap_t		pmap,
	vm_offset_t	start,
	vm_offset_t	end)
{
	register int		which_cpu, j;
	register pmap_update_list_t	update_list_p;

	while ((which_cpu = ffs((unsigned long)use_list)) != 0) {
	    which_cpu -= 1;	/* convert to 0 origin */

	    update_list_p = &cpu_update_list[which_cpu];
	    simple_lock(&update_list_p->lock);

	    j = update_list_p->count;
	    if (j >= UPDATE_LIST_SIZE) {
		/*
		 *	list overflowed.  Change last item to
		 *	indicate overflow.
		 */
		update_list_p->item[UPDATE_LIST_SIZE-1].pmap  = kernel_pmap;
		update_list_p->item[UPDATE_LIST_SIZE-1].start = VM_MIN_ADDRESS;
		update_list_p->item[UPDATE_LIST_SIZE-1].end   = VM_MAX_KERNEL_ADDRESS;
	    }
	    else {
		update_list_p->item[j].pmap  = pmap;
		update_list_p->item[j].start = start;
		update_list_p->item[j].end   = end;
		update_list_p->count = j+1;
	    }
	    cpu_update_needed[which_cpu] = TRUE;
	    simple_unlock(&update_list_p->lock);

	    /*
	     * #313: defer the flush for any CPU that is idle.  The update is
	     * already queued (cpu_update_needed, above) and MARK_CPU_ACTIVE
	     * replays it before the CPU resumes real work; pmap_tlb_ack_-
	     * outstanding already excludes idle CPUs (they leave cpus_active)
	     * so we never spin waiting for them.
	     *
	     * Previously kernel_pmap forced an immediate IPI even to idle CPUs
	     * -- and since an idle CPU runs the idle thread on the kernel pmap
	     * (real_pmap == kernel_pmap), that path fired on essentially every
	     * kernel-map teardown.  It was ~100% of the SMP shootdown tax and
	     * the bulk of the slow boot (idle AP, IPI + ack-spin per IPC).
	     *
	     * Deferring kernel_pmap to an idle CPU is safe: the kernel page
	     * directory is never freed, so the idle CPU's CR3 stays valid and
	     * only some leaf TLB entries go stale -- harmless until it touches
	     * that VA, which the idle loop never does, and it flushes on wakeup.
	     * We must STILL interrupt an idle CPU whose CR3 points at THIS
	     * *user* pmap (real_pmap == pmap && pmap != kernel_pmap): a user
	     * page directory can be torn down under it, so it has to reload CR3
	     * now.  (Safe under #311: device IRQs go to the BSP only, so an idle
	     * AP runs no driver ISR that could touch the freed kernel VA before
	     * its wakeup flush -- revisit when #312 adds IRQ affinity.)
	     */
	    if (((cpus_idle & (1 << which_cpu)) == 0) ||
		(pmap != kernel_pmap && real_pmap[which_cpu] == pmap)) {
		interrupt_processor(which_cpu);
	    }
	    /* else: idle + deferrable -- the update is already queued in
	     * cpu_update_list/cpu_update_needed and MARK_CPU_ACTIVE replays it
	     * before the CPU runs real work. */
	    use_list &= ~(1 << which_cpu);
	}
}

void
process_pmap_updates(
	register pmap_t		my_pmap)
{
	register int		my_cpu;
	register pmap_update_list_t	update_list_p;
	register int		j;
	register pmap_t		pmap;

	mp_disable_preemption();
	my_cpu = cpu_number();
	update_list_p = &cpu_update_list[my_cpu];
	simple_lock(&update_list_p->lock);

	for (j = 0; j < update_list_p->count; j++) {
	    pmap = update_list_p->item[j].pmap;
	    if (pmap == my_pmap ||
		pmap == kernel_pmap) {

	      	if (pmap->ref_count <= 0) {
			PMAP_CPU_CLR(pmap, my_cpu);
			real_pmap[my_cpu] = kernel_pmap;
			set_cr3(kernel_pmap->pdirbase);
		} else
			INVALIDATE_TLB(update_list_p->item[j].start,
				       update_list_p->item[j].end);
	    }
	} 	
	update_list_p->count = 0;
	cpu_update_needed[my_cpu] = FALSE;
	simple_unlock(&update_list_p->lock);
	mp_enable_preemption();
}

/*
 *	Interrupt routine for TBIA requested from other processor.
 *	This routine can also be called at all interrupts time if
 *	the cpu was idle. Some driver interrupt routines might access
 *	newly allocated vm. (This is the case for hd)
 */
void
pmap_update_interrupt(void)
{
	register int		my_cpu;
	spl_t			s;
	register pmap_t		my_pmap;

	mp_disable_preemption();
	my_cpu = cpu_number();
	

	my_pmap = real_pmap[my_cpu];

	if (!(my_pmap && pmap_in_use(my_pmap, my_cpu)))
		my_pmap = kernel_pmap;

	/*
	 *	Raise spl to splvm (above splip) to block out pmap_extract
	 *	from IO code (which would put this cpu back in the active
	 *	set).
	 */
	s = splvm();

	do {
	    LOOP_VAR;

	    /*
	     *	Indicate that we're not using either user or kernel
	     *	pmap.
	     */
	    i_bit_clear(my_cpu, &cpus_active);

	    /*
	     *	Wait for any pmap updates in progress, on either user
	     *	or kernel pmap.
	     */
	    while (*(volatile hw_lock_t)&my_pmap->lock.interlock ||
		   *(volatile hw_lock_t)&kernel_pmap->lock.interlock) {
	    	LOOP_CHECK("pmap_update_interrupt", my_pmap);
		continue;
	    }

	    process_pmap_updates(my_pmap);

	    i_bit_set(my_cpu, &cpus_active);

	} while (cpu_update_needed[my_cpu]);

	splx(s);
	mp_enable_preemption();
}

/*
 *	pmap_tlb_shootdown_handler() — #310/#304 robust TLB-shootdown IPI body.
 *
 *	The historical pmap_update_interrupt()/process_pmap_updates() pair
 *	consults real_pmap[my_cpu] and, for a pmap whose ref_count has hit
 *	zero, reloads CR3 with set_cr3(kernel_pmap->pdirbase) in the middle
 *	of interrupt handling.  On an AP that took the IPI from user mode
 *	this corrupts the interrupted context: real_pmap[]/the update list
 *	can be stale relative to what the AP is actually running, and the
 *	mid-flight CR3 reload leaves the user thread executing on the kernel
 *	page directory, producing near-NULL dereferences in the scheduler
 *	(act_lock_thread) a few instructions after the iret.
 *
 *	For the first correct SMP pass we need neither range precision nor
 *	the dead-pmap CR3 dance.  An unconditional local TLB flush is always
 *	safe, and the only part of the old protocol the initiator actually
 *	waits on is the cpus_active bit (see PMAP_UPDATE_TLBS).  So: drop out
 *	of cpus_active, flush, drain our queue, rejoin; re-check
 *	cpu_update_needed so a request that races in after the flush is still
 *	honoured before we return.  No locks, no CR3 reload, no dependency on
 *	current_thread()/real_pmap[].
 *
 *	Like the INVALIDATE_TLB this replaces, the flush is a CR3 reload and
 *	therefore does not evict INTEL_PTE_GLOBAL kernel entries; that
 *	matches the previous behaviour and is acceptable for the common case
 *	(user-space unmaps).  Global-kernel-page shootdown is a documented
 *	follow-up.
 */
void
pmap_tlb_shootdown_handler(void)
{
	register int	my_cpu;

	mp_disable_preemption();
	my_cpu = cpu_number();

	/*
	 *	Lock-free TLB-shootdown handler.  Two properties matter:
	 *
	 *	1. No locks.  The IPI is delivered at IF=1 (a CPU holding a
	 *	   pmap lock is at splvm/IF=0, so the IPI is masked there), but
	 *	   the interrupted code may hold an unrelated simple_lock (a
	 *	   PV-list lock, a zone lock, …).  If we spun on any lock the
	 *	   initiator might hold, while the initiator waited on the lock
	 *	   we interrupted, the two CPUs would deadlock.  An
	 *	   unconditional flush_tlb() needs no lock and is always safe.
	 *
	 *	2. cpu_update_needed[] is the ACK, and it is monotone: signal_
	 *	   cpus() sets it TRUE before sending the IPI, and we clear it
	 *	   only AFTER the flush.  The initiator waits on it (see
	 *	   pmap_tlb_ack_outstanding()).  We must NOT touch cpus_active
	 *	   here: the old code cleared+reset it as the ack, but that bit
	 *	   is reset so fast that an initiator which started spinning
	 *	   after we finished would wait on it forever (the boot hang).
	 *	   cpus_active is owned by SPLVM/SPLX and MARK_CPU_IDLE/ACTIVE.
	 *
	 *	Clear-before-flush + re-check picks up any shootdown that races
	 *	in while we are flushing, before we return.
	 */
	do {
		cpu_update_needed[my_cpu] = FALSE;
		INVALIDATE_TLB(VM_MIN_ADDRESS, VM_MAX_KERNEL_ADDRESS);
		cpu_update_list[my_cpu].count = 0;
	} while (cpu_update_needed[my_cpu]);

	mp_enable_preemption();
}
#endif	/* NCPUS > 1 */

#if	MACH_KDB

/* show phys page mappings and attributes */

extern void	db_show_page(vm_offset_t pa);

void
db_show_page(vm_offset_t pa)
{
	pv_entry_t	pv_h;
	int		pai;
	char 		attr;
	
	pai = pa_index(pa);
	pv_h = pai_to_pvh(pai);

	attr = pmap_phys_attributes[pai];
	printf("phys page %x ", pa);
	if (attr & PHYS_MODIFIED)
		printf("modified, ");
	if (attr & PHYS_REFERENCED)
		printf("referenced, ");
	if (pv_h->pmap || pv_h->next)
		printf(" mapped at\n");
	else
		printf(" not mapped\n");
	for (; pv_h; pv_h = pv_h->next)
		if (pv_h->pmap)
			printf("%x in pmap %x\n", pv_h->va, pv_h->pmap);
}

#endif /* MACH_KDB */


#if	MACH_KDB
void db_kvtophys(vm_offset_t);
void db_show_vaddrs(pt_entry_t  *);

/*
 *	print out the results of kvtophys(arg)
 */
void
db_kvtophys(
	vm_offset_t	vaddr)
{
	db_printf("0x%x", kvtophys(vaddr));
}

/*
 *	Walk the pages tables.
 */
void
db_show_vaddrs(
	pt_entry_t	*dirbase)
{
	pt_entry_t	*ptep, *pdep, tmp;
	int		x, y, pdecnt, ptecnt;

	if (dirbase == 0) {
		dirbase = kernel_pmap->dirbase;
	}
	if (dirbase == 0) {
		db_printf("need a dirbase...\n");
		return;
	}
	dirbase = (pt_entry_t *) ((unsigned long) dirbase & ~INTEL_OFFMASK);

	db_printf("dirbase: 0x%x\n", dirbase);

	pdecnt = ptecnt = 0;
	pdep = &dirbase[0];
	for (y = 0; y < NPDES; y++, pdep++) {
		if (((tmp = *pdep) & INTEL_PTE_VALID) == 0) {
			continue;
		}
		pdecnt++;
		ptep = (pt_entry_t *) ((*pdep) & ~INTEL_OFFMASK);
		db_printf("dir[%4d]: 0x%x\n", y, *pdep);
		for (x = 0; x < NPTES; x++, ptep++) {
			if (((tmp = *ptep) & INTEL_PTE_VALID) == 0) {
				continue;
			}
			ptecnt++;
			db_printf("   tab[%4d]: 0x%x, va=0x%x, pa=0x%x\n",
				x,
				*ptep,
				(y << 22) | (x << 12),
				*ptep & ~INTEL_OFFMASK);
		}
	}

	db_printf("total: %d tables, %d page table entries.\n", pdecnt, ptecnt);

}
#endif	/* MACH_KDB */

#include <mach_vm_debug.h>
#if	MACH_VM_DEBUG
#include <vm/vm_debug.h>

int
pmap_list_resident_pages(
	register pmap_t		pmap,
	register vm_offset_t	*listp,
	register int		space)
{
	return 0;
}
#endif	/* MACH_VM_DEBUG */
