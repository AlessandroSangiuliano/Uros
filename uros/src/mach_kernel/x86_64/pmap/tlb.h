/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Taking a translation away from every processor that has it (#438, #407).
 *
 * A processor caches the translations it uses, and nothing tells it when
 * another processor edits the page tables underneath.  Changing an entry is
 * therefore two operations, and only the first of them is a store: the
 * table now says one thing while every processor that has walked that
 * address still believes another.  On one processor the second operation is
 * a single instruction and easy to forget about.  On several it is a
 * message, an answer from each, and a wait — and what makes it hard is that
 * forgetting it produces no fault, no report, and no symptom until some
 * unrelated code reads through a translation that should no longer exist.
 *
 * ── Why the ordering costs nothing here ───────────────────────────────
 *
 * The store into the page table must be visible to another processor before
 * that processor is asked to flush, or it will flush and then reload the
 * entry it was told to discard.  This architecture gives that for free:
 * stores are not reordered with other stores, and the message is itself a
 * store — to the interrupt command register.  So the page-table write is
 * already ahead of the message that follows it, and no fence is needed
 * between them, only the compiler's promise not to move them.
 *
 * That is the same argument #350 checked with herd7 for the split page-table
 * locks, and it carries here for the same reason: x86-64 is still x86-TSO.
 * It is worth saying out loud that this is the one place the second
 * architecture costs nothing, because the next architecture will not be so
 * generous — a weakly-ordered machine needs a real barrier at exactly this
 * point, and the reason it needs one is written above.
 */

#ifndef _X86_64_PMAP_TLB_H_
#define _X86_64_PMAP_TLB_H_

#include <stdint.h>

/*
 * Forward-declared rather than included (#439).
 *
 * These calls need to name an address space and nothing more; pulling in
 * <pmap/pmap.h> for that would put the whole pmap interface -- and rcu.h,
 * and atomic.h, and regs.h behind it -- into every file that only wants to
 * discard a translation.  The pointer is all the type information a
 * prototype needs.
 */
struct pmap;

/*
 * Above this many pages, discard everything rather than name each address.
 *
 * An address at a time is cheaper until it is not: each one is an
 * instruction and a bus transaction, while discarding everything is one
 * operation and a slow refill afterwards.  Thirty-two is where the two are
 * usually said to cross, and it is a guess until #431 measures it on this
 * kernel — the number is here, alone, so that measuring it means changing
 * one line.
 */
#define TLB_FLUSH_PAGE_LIMIT	32

/*
 * Discard translations on this processor alone.
 *
 * For the paths that know no other processor can be using the address —
 * chiefly anything that runs before the others are woken.  Everywhere else
 * this is half of the job, and the half that hides the bug.
 */
void tlb_flush_local_range(uint64_t va, uint64_t size);
void tlb_flush_local_all(void);

/*
 * Discard them everywhere they could be, and return when every processor
 * asked has done it.
 *
 * Return, not send: the caller's next act is usually to reuse the frame
 * that translation pointed at, and it may not do that while another
 * processor could still reach the old page.  So this waits — which is the
 * reason for the range form rather than only the whole-table one.
 *
 * ⚠️ WHOSE translations, and it is not a convenience parameter (#439).  A
 * shootdown only concerns processors that could be holding this mapping,
 * which means the processors with this pmap loaded; for a user address
 * space that is usually one, or none.  Without the pmap this call has no
 * way to ask that question and can only broadcast, which is what it did.
 *
 * PMAP_NULL means "everybody", and is what the kernel's own mappings use:
 * every processor has the kernel half loaded at all times, so for those the
 * broadcast is not pessimism, it is the answer.
 *
 * Must be called with interrupts enabled; see <cpu/ipi.h> for why.  Before
 * the other processors are woken it costs nothing and does the local half,
 * so early callers need no special case.
 */
void tlb_flush_range(struct pmap *pmap, uint64_t va, uint64_t size);
void tlb_flush_all(struct pmap *pmap);

/* One page, the common case, spelt so the call sites read as what they do. */
static inline void tlb_flush_page(struct pmap *pmap, uint64_t va)
{
	tlb_flush_range(pmap, va, 0x1000);
}

/* How many shootdowns this processor has serviced, for the boot-time proof. */
uint64_t tlb_flushes_served(uint32_t apic_id);

#endif	/* _X86_64_PMAP_TLB_H_ */
