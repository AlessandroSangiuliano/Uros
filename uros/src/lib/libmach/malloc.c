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
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988,1987 Carnegie Mellon University
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
 *	File:	kern/kalloc.c
 *	Author:	Avadis Tevanian, Jr.
 *	Date:	1985
 *
 *	General kernel memory allocator.  This allocator is designed
 *	to be used by the kernel to manage dynamic memory fast.
 */

#include <mach.h>
#include <mach/mach_traps.h>		/* swtch_pri — see kalloc_lock_acquire */
#include <stdlib.h>

/*
 * ── One block belongs to one caller (#540) ───────────────────────────────
 *
 * 🔴 EVERY PIECE OF STATE BELOW IS FILE-SCOPE AND WAS REACHED BY ANY NUMBER
 * OF THREADS WITHOUT SERIALISATION.  bootstrap starts its demuxer thread
 * before the first server is launched -- deliberately, and the comment there
 * says why -- so two threads calling malloc at once is the normal case for
 * every server that answers messages while doing work: bootstrap,
 * default_pager, ext_server, name_server.
 *
 * What that cost, once in fifty-two boots: two sixteen-byte allocations were
 * handed the same block, and bootstrap relocated pci_scan.so while it had
 * asked for irq_claim_test.
 *
 * 🔑 The lock is built here, out of a compiler atomic and a Mach trap, and
 * not out of a pthread mutex.  libmach is linked into programs that do not
 * link libpthreads at all -- crt0 itself comes from here -- so an allocator
 * that needed the thread library would make every program need it.  The
 * first attempt used thread_switch() and the linker said so at once:
 * `undefined reference to thread_switch' from name_server, which links
 * libmach and nothing else.  swtch_pri() is a trap, and it is already here.
 *
 * ⚠️ Bounded spin THEN yield, in that order and not either alone.  The
 * critical sections are a handful of instructions, so on a multiprocessor
 * spinning wins; on a uniprocessor the holder cannot make progress while a
 * spinner burns the timeslice, so a spinner that never yields would be a
 * livelock.  Uniprocessor and multiprocessor are both first class here.
 */
#define	KALLOC_SPINS	64

static volatile int	kalloc_lock;

static void
kalloc_lock_acquire(void)
{
	unsigned int	spins = 0;

	while (__atomic_exchange_n(&kalloc_lock, 1, __ATOMIC_ACQUIRE) != 0) {
		if (++spins < KALLOC_SPINS) {
#if defined(__i386__) || defined(__x86_64__)
			__builtin_ia32_pause();
#endif
			continue;
		}
		spins = 0;
		(void) swtch_pri(0);
	}
}

static void
kalloc_lock_release(void)
{
	__atomic_store_n(&kalloc_lock, 0, __ATOMIC_RELEASE);
}

/*
 *	All allocations of size less than kalloc_max are rounded to the
 *	next highest power of 2.
 */
#define	MAX_SMALL_ALLOC	(16*1024)	/* max before using vm_allocate */
static vm_size_t	kalloc_max;
#define		MINSIZE	8		/* minimum allocation size */

union header {
	vm_size_t	size;		/* size of item */
	union header	*next;		/* next item on free list */
	double		align;		/* unused - force alignment */
};

#define	KLIST_MAX	12
					/* sizes: 8, 16, 32, 64,
						128, 256, 512, 1024,
						2048, 4096, 8192, 16384 */
static union header	kfree_list[KLIST_MAX];

static vm_offset_t	kalloc_next_space = 0;
static vm_offset_t	kalloc_end_of_space = 0;

static vm_size_t	kalloc_wasted_space = 0;

static boolean_t	kalloc_initialized = FALSE;

/*
 *	Initialize the memory allocator.  This should be called only
 *	once on a system wide basis (i.e. first processor to get here
 *	does the initialization).
 *
 *	This initializes all of the zones.
 */

static void
kalloc_init(void)
{
	int i;

	/*
	 * Support free lists for items up to vm_page_size or
	 * 16Kbytes, whichever is less.
	 */

	if (vm_page_size > MAX_SMALL_ALLOC)
		kalloc_max = MAX_SMALL_ALLOC;
	else
		kalloc_max = vm_page_size;

	for (i = 0; i < KLIST_MAX; i++)
	    kfree_list[i].next = 0;

	/*
	 * Do not allocate memory at address 0.
	 */
	kalloc_next_space = vm_page_size;
	kalloc_end_of_space = vm_page_size;
}

/*
 * Contiguous space allocator for items of less than a page size.
 */
static union header *
kget_space(vm_offset_t allocsize)
{
	vm_size_t	space_to_add = 0;
	vm_offset_t	new_space = 0;
	union header	*addr;

	while (kalloc_next_space + allocsize > kalloc_end_of_space) {
	    /*
	     * Add at least one page to allocation area.
	     */
	    space_to_add = round_page(allocsize);

	    if (new_space == 0) {
		/*
		 * Allocate memory.
		 * Try to make it contiguous with the last
		 * allocation area.
		 */
		new_space = kalloc_end_of_space;
		if (vm_map(mach_task_self(),
			   &new_space, space_to_add, (vm_offset_t) 0, TRUE,
			   MEMORY_OBJECT_NULL, (vm_offset_t) 0, FALSE,
			   VM_PROT_DEFAULT, VM_PROT_ALL, VM_INHERIT_DEFAULT)
			!= KERN_SUCCESS)
		    return 0;
		continue;
	    }

	    /*
	     * Memory was allocated in a previous iteration.
	     * Check whether the new region is contiguous with the
	     * old one.
	     */
	    if (new_space != kalloc_end_of_space) {
		/*
		 * Throw away the remainder of the old space,
		 * and start a new one.
		 */
		kalloc_wasted_space +=
			kalloc_end_of_space - kalloc_next_space;
		kalloc_next_space = new_space;
	    }
	    kalloc_end_of_space = new_space + space_to_add;

	    new_space = 0;
	}

	addr = (union header *)kalloc_next_space;
	kalloc_next_space += allocsize;

	if (new_space != 0)
	    (void) vm_deallocate(mach_task_self(), new_space, space_to_add);

	return addr;
}

static vm_size_t
get_allocsize(size_t size, union header **flp)
{
	vm_size_t allocsize;
	union header *fl;

	if (size >= kalloc_max) {
	    /*
	     * ⚠️ A big allocation has no free list, and this used to return
	     * without touching *flp -- leaving the caller's `fl' whatever the
	     * stack held.  It was safe, but by an invariant nobody wrote down:
	     * every caller happens to guard its use of fl with the same
	     * comparison made here.  Saying NULL makes it safe by
	     * construction, and turns a caller that forgets the guard into a
	     * null dereference instead of a walk through a stale pointer.
	     */
	    if (flp != NULL)
		*flp = NULL;
	    return round_page(size);
	}
	allocsize = MINSIZE;
	fl = kfree_list;
	while (allocsize < size) {
	    allocsize <<= 1;
	    fl++;
	}
	if (flp != NULL)
	    *flp = fl;
	return allocsize;
}


void *
malloc(size_t size)
{
	vm_size_t allocsize;
	union header *addr;
	union header *fl;

	if (size <= 0)
		return NULL;

	/*
	 * ⚠️ The lazy init is inside the lock, and that is not tidiness.  Two
	 * threads could both read FALSE and both run kalloc_init(), which
	 * clears every free list head -- losing every block on them, and
	 * handing the second caller a list the first is already walking.
	 */
	kalloc_lock_acquire();
	if (!kalloc_initialized) {
	    kalloc_init();
	    kalloc_initialized = TRUE;
	}

	/* compute the size of the block that we will actually allocate */

	size += sizeof(union header);
	allocsize = get_allocsize(size, &fl);

	/*
	 * If our size is still small enough, check the queue for that size
	 * and allocate.
	 */

	if (allocsize < kalloc_max) {
	    if ((addr = fl->next) != 0) {
		fl->next = addr->next;
	    }
	    else {
		addr = kget_space(allocsize);
	    }
	    kalloc_lock_release();
	}
	else {
	    /*
	     * 🔑 Released first: a page-sized request touches nothing this
	     * lock protects, and holding it across vm_allocate would stall
	     * every eight-byte allocation in the task for a kernel call that
	     * has nothing to do with them.
	     */
	    kalloc_lock_release();

	    /* This will allocate page 0 if it is free, but the header
	       will prevent us from returning a 0 pointer.  */
	    if (vm_allocate(mach_task_self(), (vm_offset_t *)&addr,
			    allocsize, TRUE) != KERN_SUCCESS)
		return(0);
	}

	if (addr == NULL)
		return NULL;

	addr->size = allocsize;
	return (void *) (addr + 1);
}

void
free(void *data)
{
	vm_size_t freesize;
	union header *fl;
	union header *addr;

	if (data == NULL)
		return;

	addr = ((union header *)data) - 1;

	kalloc_lock_acquire();
	freesize = get_allocsize(addr->size, &fl);

	if (freesize < kalloc_max) {
	    /*
	     * ── #223's dormant double-free guard used to sit here (#540) ──
	     *
	     * It walked this bucket's free list looking for the block being
	     * freed and returned early if it found it, and it was left `#if 0'
	     * with a note saying it would be switched back on if the symptom
	     * ever returned.
	     *
	     * ⚠️ THE TEMPTING CONCLUSION IS THE WRONG ONE.  #223's trace
	     * "looked like the same block being pushed twice onto the
	     * freelist", and #540 is exactly that happening for real -- so it
	     * is very easy to say #223 was right all along and switch the
	     * guard back on.  It was not: cap_server, where that crash
	     * happened, creates no threads at all, so this defect cannot have
	     * been its cause.  #223's own answer -- normal LIFO recycling, with
	     * the real fault in the printf %llu bug -- still stands.
	     *
	     * 🔑 So it goes for reasons of its own.  Written as it was, its
	     * early return now leaves this lock held: a dormant guard turned
	     * into a deadlock waiting for somebody to enable it.  And a guard
	     * that is switched off, carrying a promise about when it would be
	     * switched on, is not a guard -- it reads as cover for a property
	     * the code does not have.  git remembers it if it is ever wanted.
	     */
	    addr->next = fl->next;
	    fl->next = addr;
	    kalloc_lock_release();
	}
	else {
	    kalloc_lock_release();
	    (void) vm_deallocate(mach_task_self(), (vm_offset_t)addr,
				 freesize);
	}
}

void *realloc(void *data, size_t size);

/* The most common use of realloc is to manage a buffer of unlimited size
   that is grown as it fills.  So we try to optimise the case where you
   are growing the last object allocated to avoid copies.  */
void *
realloc(void *data, size_t size)
{
	void *p;
	union header *addr = ((union header *) data) - 1;
	vm_address_t vmaddr = (vm_address_t) addr;
	vm_address_t newaddr;
	vm_size_t oldsize, allocsize;
	size_t tocopy;

	if (data == NULL)
	    return malloc(size);

	oldsize = addr->size;
	allocsize = get_allocsize(size + sizeof(union header), NULL);
	if (allocsize == oldsize)
	    return data;

	/* Deal with every case where we don't want to do a simple
	   malloc+memcpy+free.  Otherwise it is a "simple case" in the
	   comments.  */
	if (allocsize < oldsize) {
	    /* Shrinking.  We favour space over time here since if time is
	       really important you can just not do the realloc.  */
	    if (oldsize >= kalloc_max) {
		/* Shrinking a lot.  */
		if (allocsize >= kalloc_max) {
		    (void) vm_deallocate(mach_task_self(), vmaddr + allocsize,
					 oldsize - allocsize);
		    addr->size = allocsize;
		    return data;
		}
		/* Simple case: shrinking from a whole page or pages to less
		   than a page.  */
	    } else {
		/*
		 * ⚠️ The test and the assignment have to be one indivisible
		 * step: "am I the last block handed out" stops being true the
		 * instant another thread is handed one.
		 */
		kalloc_lock_acquire();
		if (vmaddr + oldsize == kalloc_next_space) {
		    /* Shrinking the last item in the current page.  */
		    kalloc_next_space = vmaddr + allocsize;
		    kalloc_lock_release();
		    addr->size = allocsize;
		    return data;
		}
		kalloc_lock_release();
		/* Simple case: shrinking enough to fit in a smaller power
		   of two.  */
	    }
	    tocopy = size;
	} else {
	    /* Growing.  */
	    if (allocsize >= kalloc_max) {
		/* Growing a lot.  */
		if (oldsize >= kalloc_max) {
		    /* We could try to vm_allocate extra pages after the old
		       data, but vm_allocate + vm_copy is not much more
		       expensive than that, even if it does fragment the
		       address space a bit more.  */
		    newaddr = vmaddr;
		    if (vm_allocate(mach_task_self(), &newaddr, allocsize,
				    TRUE) != KERN_SUCCESS ||
			vm_copy(mach_task_self(), vmaddr, oldsize, newaddr)
			!= KERN_SUCCESS)
			return NULL;
		    (void) vm_deallocate(mach_task_self(), vmaddr, oldsize);
		    addr = (union header *) newaddr;
		    addr->size = allocsize;
		    return (void *) (addr + 1);
		}
		/* Simple case: growing from less than a page to one or more
		   whole pages.  */
	    } else {
		/* Growing from a within-page size to a larger within-page
		   size.  Frequently the item being grown is the last one
		   allocated so try to avoid copies in that case.  */
		kalloc_lock_acquire();
		if (vmaddr + oldsize == kalloc_next_space) {
		    if (vmaddr + allocsize <= kalloc_end_of_space) {
			kalloc_next_space = vmaddr + allocsize;
			kalloc_lock_release();
			addr->size = allocsize;
			return data;
		    } else {
			/*
			 * 🔑 Held across vm_allocate here, unlike the big
			 * path in malloc: this call is only worth making
			 * while this block is still the last one handed out,
			 * and letting go to make it would decide on an answer
			 * that another thread has already changed.
			 */
			newaddr = round_page(vmaddr);
			if (vm_allocate(mach_task_self(), &newaddr,
					vm_page_size, FALSE)
			    == KERN_SUCCESS) {
			    kalloc_next_space = vmaddr + allocsize;
			    kalloc_end_of_space = newaddr + vm_page_size;
			    kalloc_lock_release();
			    addr->size = allocsize;
			    return (void *) (addr + 1);
			}
			/* Simple case: growing the last object in the page
			   past the end of the page when the next page is
			   unavailable.  */
		    }
		}
		kalloc_lock_release();
		/* Simple case: growing a within-page object that is not the
		   last object allocated. */
	    }
	    tocopy = oldsize - sizeof(union header);
	}

	/* So if we get here, we can't do any better than this: */
	p = malloc(size);
	if (p != NULL) {
	    memcpy(p, data, tocopy);
	    free(data);
	}
	return p;
}

void *calloc(size_t nmemb, size_t size);
void *calloc(size_t nmemb, size_t size)
{
	void *addr = malloc(nmemb * size);
	if (addr != NULL)
		memset(addr, 0, nmemb * size);
	return addr;
}

/* This is in linux libc too - sigh */
void cfree(void *ptr);

void cfree(void *ptr)
{
	free(ptr);
}
