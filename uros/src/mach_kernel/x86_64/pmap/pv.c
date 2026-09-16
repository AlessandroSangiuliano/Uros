/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 physical-to-virtual index (#407, MD contract 2/6).
 */

#include <stdint.h>

#include <kern/lock.h>
#include <kern/misc_protos.h>	/* printf: the ablated arm names itself */
#include <machine/cpu_data.h>
#include <cpu/percpu.h>
#include <cpu/regs.h>
#include <sync/atomic.h>
#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/pte.h>
#include <pmap/pv.h>
#include <trap/trap.h>

static pv_entry_t pv_head_table;	/* one entry per physical page */
static uint64_t   pv_pages;		/* how many pages it covers */

/*
 * Entries for the second and later mappings of a page.
 *
 * 🔥 THIS LIST IS GLOBAL, AND UNTIL #558 IT HAD NO LOCK AT ALL (09/2026).
 *
 * `pv_alloc' popped and `pv_free' pushed with neither lock nor atomics, so two
 * processors mapping second-or-later mappings of DIFFERENT pages -- each under
 * its own object lock, which is why nothing else caught it -- could be handed
 * the SAME entry.  One page's list then grafts onto another's, and a walk from
 * one page arrives at a `pv->pmap' belonging to somewhere else entirely.  That
 * is a general protection fault when the value is wild enough to leave the
 * canonical range, and silent corruption when it is not.
 */
static pv_entry_t pv_free_list;
decl_simple_lock_data(static, pv_free_lock)

#define pa_index(pa)	((pa) >> PT_SHIFT)

/*
 * The switch that takes the per-page lock away again (#558).
 *
 * Off means the lock is IN, which is the shipping shape.  On restores exactly
 * what this file had before the lock existed -- a walk that follows pv->next
 * and dereferences pv->pmap with nothing held -- and it exists because the
 * general protection fault this was written for has never been reproduced: the
 * lock was designed from a reading of one reperto, and a fix is verified by
 * REMOVING it and watching the fault come back.
 *
 * ❌ THE FIRST VERSION OF THIS SWITCH LEFT THE FREE LIST LOCKED, "to keep the
 * ablated arm about the WALK".  That excluded, by construction, the mechanism
 * this file names at the top as the one that produces a wild pv->pmap: two
 * processors handed the SAME entry, one page's list grafted onto another's.
 * Eight boots of eight came back clean, and they could not have done otherwise.
 * The switch now means what its name says -- the index as it was before #558,
 * neither lock -- and the free-list arm is where the fault is most likely to
 * live.
 *
 * 🔑 AND IT IS TWO SWITCHES, BECAUSE THERE ARE TWO MECHANISMS (16/09).
 *
 * With both locks off, 18 boots produced six `still mapped' panics (#531) and
 * not one general protection fault -- so the arm that reproduces on demand
 * cannot say WHICH of the two races it is reproducing.  The file itself
 * separates them at the top:
 *
 *   the FREE LIST, unlocked, hands two processors the same entry, and "one
 *   page's list grafts onto another's, and a walk from one page arrives at a
 *   pv->pmap belonging to somewhere else entirely" -- a wild pointer, which
 *   is the literal description of the fault #558 is named for;
 *
 *   the PER-PAGE lock, absent, lets a walk race pv_remove()'s head-copy and
 *   MISS an entry -- a mapping nobody tears down, which is the literal
 *   description of #531's panic.
 *
 * Each has a symptom the other does not obviously produce, so one switch each:
 * UROS_ABLATE_558_PVLOCK for the page lock, UROS_ABLATE_558_PVFREELOCK for the
 * free list.  ⚠️ The campaigns of 16/09 ran with BOTH on, which is now spelled
 * with both options rather than with one.
 *
 * Set them from the build: cmake -DUROS_ABLATE_558_PVLOCK=ON
 * -DUROS_ABLATE_558_PVFREELOCK=ON.  pv_bootstrap() says which arm is running,
 * because a log that does not name the arm is a campaign whose result cannot
 * be attributed -- and with two switches it has to name them separately.
 */
#ifndef	ABLATE_558_PVLOCK
#define	ABLATE_558_PVLOCK	0
#endif
#ifndef	ABLATE_558_PVFREELOCK
#define	ABLATE_558_PVFREELOCK	0
#endif

/*
 * The free list's lock, through the ablation switch below: on, the pushes and
 * pops run exactly as they did before #558 -- no lock and no atomics.
 */
static void pv_free_lock_take(void)
{
#if	!ABLATE_558_PVFREELOCK
	simple_lock(&pv_free_lock);
#endif
}

static void pv_free_lock_drop(void)
{
#if	!ABLATE_558_PVFREELOCK
	simple_unlock(&pv_free_lock);
#endif
}

/*
 * 🔥 THE LISTS THEMSELVES (#558).
 *
 * Half of this index was protected by accident and that is why it lasted: the
 * VM paths reach a page's list with that page's OBJECT locked.  `pmap_destroy'
 * does not -- it reaches pv_remove() through its own page tables, rewriting the
 * list of every page those tables map, pages belonging to arbitrary objects.
 * Two different locks are no exclusion at all, and pv_remove() removing the
 * head COPIES the next entry's contents up and frees that entry, so a reader
 * standing on the list can have the entry under it recycled mid-walk.
 *
 * 🔑 ONE LOCK PER PAGE WOULD COST 8 MB ON 4 GB of RAM, on a table that is
 * already 24 bytes a page.  A hashed array is the same protection for a fixed
 * size, and the size is chosen against CONCURRENT PAGES rather than against
 * processors: with N locks and k operations in flight the collision rate is
 * about k*k/2N, so 64 processors all in the index want thousands, not tens.
 * 1024 gives about half a collision at that width and costs nothing here.
 *
 * ⚠️ AND THEY ARE PADDED TO A CACHE LINE, which is where the memory goes:
 * simple_lock_data_t is ONE byte, so unpadded there would be sixty-four locks
 * per line and two processors taking two DIFFERENT locks would ping-pong the
 * line between them -- contention invented by the layout, invisible in the
 * code.  1024 * 64 B = 64 KB, a quarter of a per-cent of the table it guards.
 *
 * 🔴 LOCK ORDER, and there is only one: a page's head lock may be taken with
 * the free-list lock DOWN and then take it (pv_remove -> pv_free).  Never the
 * other way: pv_enter() takes its entry from pv_alloc() BEFORE locking the
 * head, which is also what keeps a blocking allocation out of the section.
 */

#define PV_LOCKS	1024		/* power of two: the index is a mask */

struct pv_lock {
	volatile uint8_t	l;
} __attribute__((aligned(64)));

static struct pv_lock pv_locks[PV_LOCKS];

static struct pv_lock *pv_lock_for(uint64_t pa)
{
	return &pv_locks[pa_index(pa) & (PV_LOCKS - 1)];
}

/*
 * 🔥 A SPIN LOCK THAT DOES NOT MASK INTERRUPTS, WHICH IS THE WHOLE POINT.
 *
 * simple_lock() masks them (#528, "the mask is the processor's count, not a
 * flag in the lock"), and a walk of one of these lists ends in a TLB
 * SHOOTDOWN: a cross-call to every processor that waits for their
 * acknowledgement.  With interrupts off the acknowledgement cannot arrive, and
 * the kernel says so rather than hanging --
 *
 *     panic(cpu 2): ipi: a cross-call with interrupts off would deadlock
 *
 * -- which is what happened when this was a simple_lock, on the first boot.
 *
 * 🔑 THE MASKING IS NOT WHAT MAKES A SPIN LOCK SAFE; it is what makes a lock
 * that INTERRUPT HANDLERS take safe.  hw_lock_lock() says why: a waiter "may
 * be in interrupt context, where the gate has already cleared IF", so a
 * preempted holder could never be scheduled again.  These lists are reached
 * only from fault and VM paths -- pmap.c is their only caller outside boot,
 * and every one of those arrives holding blocking locks, which is already
 * illegal in a handler.  A lock no handler takes does not need the mask.
 *
 * That is the same choice FreeBSD makes for the pmap (a blocking lock class
 * rather than MTX_SPIN) and the BSDs make with IPL (the lock below the
 * shootdown IPI's level), for exactly this reason.
 *
 * 🔴 PREEMPTION STILL GOES OFF.  A preempted holder makes every other
 * processor spin for a quantum, and disable_preemption() is real on this
 * target -- x86_64/cpu_data.h defines it as an inline over the per-CPU
 * counter, overriding the empty macro <kern/cpu_data.h> supplies when MACH_RT
 * is off (#461).  Verified, not assumed: an empty guard would be worse here
 * than no guard.
 */
static void pv_lock_take(struct pv_lock *p)
{
#if	ABLATE_558_PVLOCK
	(void) p;
	return;
#else
	/*
	 * ❌ THE PRECONDITION I ASSERTED HERE IS FALSE, AND THAT IS MEASURED
	 * (#558, 14/09).
	 *
	 * The assert was `!pmap_initialized || (read_rflags() & RFLAGS_IF)',
	 * on the reasoning that a waiter with interrupts off cannot answer the
	 * shootdown of whoever holds the lock -- hw_lock_lock()'s argument,
	 * turned around.  It fired on the first boot:
	 *
	 *     pv_lock_page <- pv_enter <- pmap_enter <- vm_fault_wire_fast
	 *       <- vm_fault_wire <- vm_map_wire <- kernel_memory_allocate
	 *       <- kmem_alloc_aligned <- stack_alloc <- thread_machine_create
	 *
	 * Wiring a new thread's kernel stack reaches pv_enter with interrupts
	 * ALREADY masked -- a probe at pmap_enter's entry reports
	 * percpu_intr_level = 1 there, so a hw_lock is held somewhere above.
	 * ⚠️ WHICH ONE IS NOT ESTABLISHED: vm_object_lock and
	 * vm_page_lock_queues are both mutexes (atomic_cmpxchg8, no masking),
	 * pmap_writer_lock is inert in the arm that ships, urmach_rcu_read_lock
	 * raises the PREEMPT level and not the interrupt one, and
	 * vm_fault_wire_fast releases the object lock before PMAP_ENTER.  Said
	 * as unknown rather than guessed at a fourth time.
	 *
	 * 🔑 THE DESIGN CONSEQUENCE DOES NOT DEPEND ON THE NAME: legitimate
	 * callers arrive here masked, so a waiter can be unable to answer, so
	 * NOBODY MAY SHOOT DOWN WHILE HOLDING ONE OF THESE.  The lock is still
	 * right for what pv.c does under it -- pv_enter, pv_remove and pv_count
	 * shoot down nothing -- and the two walks in pmap.c that do are back to
	 * unprotected, which is written where they are.
	 */

	disable_preemption();

	for (;;) {
		if (atomic_cmpxchg8(&p->l, 0, 1) == 0)
			return;

		/*
		 * Spin on a plain read rather than on the exchange, so waiters
		 * share the cache line instead of tearing it away from the
		 * holder who needs it to release -- hw_lock_lock()'s reasoning,
		 * and it applies to any spin lock.
		 */
		while (p->l != 0)
			cpu_pause();
	}
#endif	/* ABLATE_558_PVLOCK */
}

static void pv_lock_drop(struct pv_lock *p)
{
#if	ABLATE_558_PVLOCK
	(void) p;
#else
	(void) atomic_swap8(&p->l, 0);
	enable_preemption();
#endif	/* ABLATE_558_PVLOCK */
}

/*
 * For the walks that live in pmap.c and must hold the list still across a whole
 * traversal.  ⚠️ NOT for pmap_page_protect's VM_PROT_NONE loop: that one calls
 * pmap_forget(), which calls pv_remove() on the SAME pa, and holding this
 * across it is a self-deadlock -- the shape of #486.  That loop re-reads
 * pv_head(pa) every iteration instead, which is why it was written that way.
 */
void pv_lock_page(uint64_t pa)
{
	if (pv_managed(pa))
		pv_lock_take(pv_lock_for(pa));
}

void pv_unlock_page(uint64_t pa)
{
	if (pv_managed(pa))
		pv_lock_drop(pv_lock_for(pa));
}

void pv_bootstrap(uint64_t top_of_ram)
{
	uint64_t pages = top_of_ram >> PT_SHIFT;
	uint64_t bytes = pages * sizeof(struct pv_entry);
	uint64_t frames = (bytes + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
	uint64_t table_pa = boot_frames_alloc(frames);

	/*
	 * Without the table every pv_managed() is false, so pv_enter and
	 * pv_remove become no-ops and pmap_page_protect goes on to find no
	 * mappings for pages that have them.  Copy-on-write would arm
	 * nothing, and say so to nobody.
	 */
	if (table_pa == 0)
		panic("pv: no memory for the physical-to-virtual index");

	/*
	 * Reached through the direct map: the frames are consecutive, so the
	 * array is contiguous there too, and needs no mapping of its own.
	 * boot_frames_alloc() returned it zeroed, which is already the right
	 * initial state — a head with no pmap is a page with no mappings.
	 */
	pv_head_table = (pv_entry_t)(uintptr_t)phys_to_direct(table_pa);
	pv_pages = pages;

	simple_lock_init(&pv_free_lock, ETAP_VM_PMAP_FREE);
	for (unsigned i = 0; i < PV_LOCKS; i++)
		pv_locks[i].l = 0;

	/*
	 * Which arm this boot is, said out loud.  A campaign whose log does not
	 * name the arm cannot attribute what it finds -- and this one costs a
	 * line in the ablated arm and nothing at all in the shipping one.
	 */
#if	ABLATE_558_PVLOCK
	printf("pv: the per-page lock is ABLATED — the walks run with nothing "
	       "held, so a walk can race pv_remove and MISS an entry\n");
#endif	/* ABLATE_558_PVLOCK */
#if	ABLATE_558_PVFREELOCK
	printf("pv: the free list is ABLATED — pv_alloc and pv_free push and pop "
	       "with no lock, so two processors can be handed the SAME entry\n");
#endif	/* ABLATE_558_PVFREELOCK */
}

int pv_managed(uint64_t pa)
{
	return pv_head_table != PV_ENTRY_NULL && pa_index(pa) < pv_pages;
}

pv_entry_t pv_head(uint64_t pa)
{
	if (!pv_managed(pa))
		return PV_ENTRY_NULL;

	return &pv_head_table[pa_index(pa)];
}

/*
 * Entries come from the free list, refilled a frame at a time.  A frame
 * holds many, and freed entries come back here — so the churn of mapping
 * and unmapping does not consume memory, even though the frame allocator
 * beneath has no free of its own.
 */
static pv_entry_t pv_alloc(void)
{
	pv_entry_t e;
	uint64_t frame;
	unsigned per_frame = PAGE_SIZE_4K / sizeof(struct pv_entry);

	pv_free_lock_take();
	if (pv_free_list != PV_ENTRY_NULL) {
		e = pv_free_list;
		pv_free_list = e->next;
		pv_free_lock_drop();
		return e;
	}
	pv_free_lock_drop();

	/*
	 * pmap_table_frame(), same class as pmap_create()'s and the large-page
	 * split's (#422): the boot allocator is empty once the VM has taken
	 * physical memory over, and this runs on every second mapping of a page
	 * for as long as the system is up -- so with boot_frame_alloc() the
	 * panic in pv_enter() was not a safeguard, it was a wall a few mappings
	 * away.
	 *
	 * 🔴 AND IT IS CALLED WITH THE LOCK DOWN, WHICH IS THE WHOLE SHAPE OF
	 * THIS FUNCTION.  It can block -- after the VM is up it is vm_page_grab
	 * with a VM_PAGE_WAIT behind it -- and blocking while holding a spin
	 * lock stops every other processor that wants an entry, on a lock whose
	 * holder is asleep.  It is the same rule map.c states for the QSBR read
	 * section next to the large-page split, for the same reason and one
	 * step stronger: "THE FRAME IS TAKEN BEFORE THE SECTION OPENS".
	 *
	 * ⚠️ Two processors that both miss both take a frame, and both thread
	 * theirs in.  That is not a leak and needs no undoing: an extra frame's
	 * worth of entries is simply free entries, and the alternative -- hold
	 * the lock across the allocation to keep the second one from happening
	 * -- is the thing this arrangement exists to avoid.
	 */
	frame = pmap_table_frame();
	if (frame == 0)
		return PV_ENTRY_NULL;

	e = (pv_entry_t)(uintptr_t)phys_to_direct(frame);

	pv_free_lock_take();
	for (unsigned i = 0; i < per_frame; i++) {
		e[i].next = pv_free_list;
		pv_free_list = &e[i];
	}
	e = pv_free_list;
	pv_free_list = e->next;
	pv_free_lock_drop();

	return e;
}

static void pv_free(pv_entry_t e)
{
	e->pmap = PMAP_NULL;

	pv_free_lock_take();
	e->next = pv_free_list;
	pv_free_list = e;
	pv_free_lock_drop();
}

void pv_enter(uint64_t pa, pmap_t pmap, uint64_t va)
{
	pv_entry_t head = pv_head(pa);
	pv_entry_t e;

	if (head == PV_ENTRY_NULL)
		return;

	/*
	 * 🔴 THE ENTRY IS TAKEN BEFORE THE LOCK, ALWAYS -- even on the first
	 * mapping, which will not need it.
	 *
	 * pv_alloc() can refill, and refilling calls pmap_table_frame(), which
	 * BLOCKS.  Sleeping while holding this page's spin lock would stop
	 * every other processor that touches the page, on a lock whose holder
	 * is asleep.  So the allocation happens outside and the spare is handed
	 * back if the list turns out to be empty: map.c's large-page split does
	 * exactly this, and says why -- "allocate outside, decide inside, give
	 * the page back if you lost the argument".
	 *
	 * ⚠️ The cost is an alloc and a free on the common path, which is the
	 * price of not holding a lock across a wait.  It is paid once per
	 * mapping, inside a page fault.
	 *
	 * The mapping exists whether or not it is recorded, and an unrecorded
	 * one is invisible to every operation that starts from the physical
	 * page — so pmap_page_protect would leave it writable while reporting
	 * that it had protected the page.  There is no honest way to return
	 * from here.
	 */
	e = pv_alloc();
	if (e == PV_ENTRY_NULL)
		panic("pv: out of entries, a mapping would go unrecorded");

	pv_lock_page(pa);

	/* The common case: first mapping of this page, the spare goes back. */
	if (head->pmap == PMAP_NULL) {
		head->pmap = pmap;
		head->va = va;
		head->next = PV_ENTRY_NULL;
		pv_unlock_page(pa);
		pv_free(e);
		return;
	}

	/*
	 * Push in after the head rather than at the end: the head cannot move,
	 * since its address is what identifies the page.
	 */
	e->pmap = pmap;
	e->va = va;
	e->next = head->next;
	head->next = e;

	pv_unlock_page(pa);
}

void pv_remove(uint64_t pa, pmap_t pmap, uint64_t va)
{
	pv_entry_t head = pv_head(pa);
	pv_entry_t prev, e;

	if (head == PV_ENTRY_NULL)
		return;

	pv_lock_page(pa);

	if (head->pmap == PMAP_NULL) {
		pv_unlock_page(pa);
		return;
	}

	/*
	 * Removing the head keeps the head where it is: the next entry's
	 * contents are copied up into it and that entry is freed.
	 *
	 * 🔥 THOSE THREE STORES ARE WHY A READER NEEDS THIS LOCK AND NOT A
	 * GRACE PERIOD.  A reader walking without it can take `pmap' from the
	 * entry that was here and `va' from the one that replaced it -- an
	 * entry that never existed, naming a real address in the wrong space.
	 * That does not fault; it protects the wrong page, silently.  Deferring
	 * the free would stop the use-after-free and leave this untouched.
	 */
	if (head->pmap == pmap && head->va == va) {
		e = head->next;
		if (e == PV_ENTRY_NULL) {
			head->pmap = PMAP_NULL;
			pv_unlock_page(pa);
			return;
		}
		head->pmap = e->pmap;
		head->va = e->va;
		head->next = e->next;
		pv_unlock_page(pa);
		pv_free(e);
		return;
	}

	prev = head;
	for (e = head->next; e != PV_ENTRY_NULL; prev = e, e = e->next)
		if (e->pmap == pmap && e->va == va) {
			prev->next = e->next;
			pv_unlock_page(pa);
			pv_free(e);
			return;
		}

	pv_unlock_page(pa);
}

unsigned pv_count(uint64_t pa)
{
	pv_entry_t head = pv_head(pa);
	unsigned n = 0;

	if (head == PV_ENTRY_NULL)
		return 0;

	pv_lock_page(pa);
	if (head->pmap != PMAP_NULL)
		for (pv_entry_t e = head; e != PV_ENTRY_NULL; e = e->next)
			n++;
	pv_unlock_page(pa);

	return n;
}
