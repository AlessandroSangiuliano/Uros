/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 memory barriers (#410, MD contract 5/6).
 *
 * Each barrier here is named and documented by *what it orders*, not by
 * which instruction it emits, and that is a deliberate discipline rather
 * than a style preference.
 *
 * x86-64 is x86-TSO, the same model i386 has: loads are not reordered with
 * loads, stores are not reordered with stores, and a store is not reordered
 * ahead of an older load.  Exactly one reordering is visible — a load may be
 * satisfied before an older store to a different address becomes visible to
 * others.  Store-then-load is the only shape that needs a fence.
 *
 * That is why two of the three barriers below emit no instruction at all:
 * the ordering is already guaranteed, and the only thing left to restrain
 * is the compiler.  It is also why a comment naming an instruction would be
 * worthless — `mfence` says nothing about which two accesses must not be
 * seen out of order, and it is that claim, not the instruction, that has to
 * survive the day this kernel meets a weak-memory architecture.  There the
 * emitted code changes and the claim does not.
 *
 * #350 proved the pmap shootdown barrier necessary *and* sufficient under
 * x86-TSO with herd7.  That proof carries to x86-64 unchanged, because the
 * model does.  It will not carry to the next architecture, and the litmus
 * tests are where that gets re-established rather than re-guessed.
 */

#ifndef _X86_64_SYNC_BARRIER_H_
#define _X86_64_SYNC_BARRIER_H_

/*
 * Order nothing in the machine, everything in the compiler: no access
 * written before this may be moved after it, or the reverse.  Needed
 * wherever the hardware already provides the ordering and only the
 * optimiser could take it away — which on this architecture is most places.
 */
#define barrier()	__asm__ volatile("" ::: "memory")

/*
 * Loads before this are observed before loads after it.
 *
 * The hardware already guarantees this: TSO does not reorder loads with
 * loads.  A compiler barrier is the whole of it, and an lfence here would
 * be a cost with nothing bought.
 */
#define smp_rmb()	barrier()

/*
 * Stores before this become visible before stores after it.
 *
 * Also already guaranteed: TSO does not reorder stores with stores.  The
 * one exception is the non-temporal stores, which are their own contract
 * and are not used here; if they ever are, this is the definition that has
 * to grow an sfence, and the place to notice is here.
 */
#define smp_wmb()	barrier()

/*
 * A store before this is visible to other CPUs before a load after it is
 * satisfied.
 *
 * This is the one shape TSO permits to be reordered, so it is the one that
 * costs an instruction.  It is the ordering behind every "publish, then
 * check whether anyone is waiting" — including the pmap shootdown, where
 * the entry must be written before the responses are read, and where #350
 * showed that without this the reader can see neither.
 */
#define smp_mb()	__asm__ volatile("mfence" ::: "memory")

/*
 * A locked read-modify-write is already a full barrier, so code that has
 * just done one does not need smp_mb() as well.  Stated here because the
 * redundancy is invisible at the call site: the atomic looks like it only
 * touches its own word.
 */

#endif	/* _X86_64_SYNC_BARRIER_H_ */
