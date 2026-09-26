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
 * page_cache.h — Generic block-level page cache for filesystem servers
 *
 * Provides an LRU cache of disk blocks keyed by physical block number.
 * Designed to be filesystem-independent: any server that reads blocks
 * via device_read() can use this cache to avoid repeated IPC round-trips.
 *
 * Usage:
 *   struct page_cache *pc = page_cache_create(256, my_writeback, dev);
 *   dev->cache = pc;
 *   // ... filesystem reads will check/populate the cache ...
 *   page_cache_destroy(pc);
 */

#ifndef _PAGE_CACHE_H_
#define _PAGE_CACHE_H_

#include <mach.h>
#include <mach/vm_types.h>
#include <pthread.h>
#include <stdint.h>

/*
 * Hash buckets.  Must scale with the entry count or lookups degrade into
 * long linear chain walks.  The DMA page cache holds up to 4096 entries
 * (#265); with the old 128 buckets that was a load factor of 32 and made
 * cached reads measurably slower.  4096 buckets keeps the load factor at
 * ~1 (array is 16 KB per cache — negligible).
 */
#define PAGE_CACHE_HASH_BUCKETS	4096

/*
 * #599: how many of the oldest entries eviction looks at for a clean one
 * before it writes a dirty one back; the self-test builds its case around it.
 */
#define PAGE_CACHE_VICTIM_SCAN	64

/*
 * #599: page_cache_install's answers besides 0 and a kern_return_t: the key
 * is held (cached or being read), or the bytes may be older than the disk.
 */
#define PAGE_CACHE_PRESENT	(-1)
#define PAGE_CACHE_STALE	(-2)

/*
 * Writeback callback: called when a dirty block must be flushed to disk.
 * Arguments: opaque context, block number, data pointer, data size,
 * physical address (non-zero for DMA-backed entries).
 * Returns 0 on success, non-zero on failure.
 */
typedef int (*page_cache_writeback_fn)(void *ctx, daddr_t block,
				       vm_offset_t data, vm_size_t size,
				       vm_offset_t phys);

/*
 * #599: the fill a page_cache_get runs for a block it does not hold, with no
 * lock of the cache's held.  It must read all `size' bytes of `block' into
 * `data' (or through `phys', the slot's physical address, 0 for a slab slot)
 * and answer 0, or answer why not.  It takes no lock, waits for nothing but
 * its device, and never calls into a page cache.
 */
typedef int (*page_cache_fill_fn)(void *ctx, daddr_t block,
				  vm_offset_t data, vm_size_t size,
				  vm_offset_t phys);

/*
 * #599: an entry's state.  FREE: on the free list.  FILLING: keyed, being
 * read by the page_cache_get that keyed it; not data yet -- nobody else may
 * see its bytes, and every lookup of its key waits.  VALID: a cached block.
 * ORPHAN: pinned when its block was discarded; no key, freed at the last put.
 */
#define PC_FREE		0
#define PC_FILLING	1
#define PC_VALID	2
#define PC_ORPHAN	3

struct page_cache_entry {
	daddr_t			pc_block;	/* disk block number (key) */
	vm_offset_t		pc_data;	/* cached block data */
	vm_size_t		pc_size;	/* size of cached data */
	vm_offset_t		pc_phys;	/* physical addr (0 = vm_allocate'd); full
					   width: a page can be above 4 GiB (#599) */
	int			pc_dirty;	/* block has been modified */
	int			pc_wfail;	/* #599: its writeback failed,
						   and it was said once */
	int			pc_state;	/* #599: PC_FREE .. PC_ORPHAN */
	/*
	 * #599: pins -- pointers into pc_data held outside pc_lock (a
	 * page_cache_get's caller, until page_cache_put).  A pinned entry is
	 * never re-keyed or reused: a pin guards identity and lifetime, not
	 * content.
	 */
	unsigned int		pc_refs;
	/*
	 * #599: pc_wgen counts the writes into the slot; a sync marks the
	 * block clean only if it is the count it copied when it collected the
	 * block, so a write that lands during the writeback keeps it dirty.
	 * pc_dirty_seq is the cache's pc_seq when it went dirty (the sync's
	 * horizon), pc_tried the sync call that last collected it.
	 */
	unsigned int		pc_wgen;
	uint64_t		pc_dirty_seq;
	unsigned int		pc_tried;
	/*
	 * #599: the cache's pc_seq when a writeback last made this block
	 * clean, 0 if it never was dirty.  Its copy leaving the cache raises
	 * pc_forget to it (page_cache_install).
	 */
	uint64_t		pc_clean_seq;
	int			pc_busy;	/* #384: writeback in flight —
						   its data is being written
						   outside pc_lock, so eviction
						   must skip this entry */
	struct page_cache_entry	*pc_hash_next;	/* hash chain */
	struct page_cache_entry	*pc_lru_next;	/* toward MRU */
	struct page_cache_entry	*pc_lru_prev;	/* toward LRU */
};

struct page_cache {
	pthread_mutex_t		pc_lock;	/* protects all fields below */
	/*
	 * #599: one page_cache_sync at a time; taken before pc_lock, and
	 * never with an ext2 lock held.  Two syncs -- the writeback thread's
	 * and ds_ext2_sync's -- used to overwrite each other's marks.
	 */
	pthread_mutex_t		pc_sync_lock;
	/*
	 * #599: broadcast when a fill ends; every wait for a FILLING key is
	 * on it, and the key is looked up again after the wait -- never the
	 * old pointer.  pc_nwaiters counts the threads inside the wait (the
	 * self-test watches it).
	 */
	pthread_cond_t		pc_cond;
	unsigned int		pc_nwaiters;
	uint64_t		pc_seq;		/* ticks at every clean->dirty
						   and dirty->clean */
	/*
	 * #599: the newest pc_seq at which a copy that had been dirty left
	 * the cache -- evicted once written back.  A readahead ticket older
	 * than it may hold that block's old bytes, read from the disk before
	 * the writeback landed.  64 bits: it never wraps.
	 */
	uint64_t		pc_forget;
	unsigned int		pc_sync_calls;
	unsigned int		pc_max_entries;
	/*
	 * #599: every slot is this size, fixed at creation: pc_data, pc_size
	 * and pc_phys never change after it.  A non-DMA cache owns one slab
	 * (pc_slab) cut into the slots; a DMA cache's slots are the pool its
	 * creator allocated, which is not the cache's to free.
	 */
	vm_size_t		pc_block_size;
	vm_offset_t		pc_slab;	/* non-DMA slots, owned (0 = DMA) */
	vm_size_t		pc_slab_size;
	unsigned int		pc_count;
	unsigned int		pc_hits;
	unsigned int		pc_misses;
	unsigned int		pc_evictions;
	unsigned int		pc_writebacks;
	page_cache_writeback_fn	pc_writeback;	/* dirty block flush callback */
	void			*pc_writeback_ctx; /* opaque context for callback */
	struct page_cache_entry	*pc_pool;	/* pre-allocated entry array */
	struct page_cache_entry	*pc_free;	/* singly-linked free list */
	/* DMA pool: pre-allocated wired pages with known physical addrs */
	vm_offset_t		pc_dma_pool;	  /* base VA (0 = no DMA) */
	vm_size_t		pc_dma_pool_size; /* total pool bytes */
	struct page_cache_entry	*pc_hash[PAGE_CACHE_HASH_BUCKETS];
	/* LRU sentinels: head.pc_lru_next = MRU, tail.pc_lru_prev = LRU */
	struct page_cache_entry	pc_lru_head;
	struct page_cache_entry	pc_lru_tail;
};

/*
 * Create a page cache with the given maximum number of cached blocks.
 * 'writeback' is called when a dirty block must be flushed (eviction or sync).
 * 'ctx' is passed as-is to the callback (typically the device port).
 *
 * 🔴 'writeback' MUST NOT BE NULL, and a cache asked for without one is
 * refused: NULL comes back, as for an allocation failure (#573).  This used
 * to say that dirty blocks were then "silently discarded", and that is what
 * happened to every write on a mount configured before its block size was
 * known: the data was dropped, counted as written, and the sync succeeded
 * over a file of zeros.  A cache that holds a filesystem's blocks cannot
 * have dirty data it is allowed to lose.
 *
 * Returns NULL on allocation failure or without a writeback.
 */
struct page_cache *page_cache_create(unsigned int max_entries,
				     vm_size_t block_size,
				     page_cache_writeback_fn writeback,
				     void *ctx);

/*
 * Destroy a page cache and free what it owns: the slab of a non-DMA cache,
 * the entries, the cache.  Never a DMA pool -- that belongs to whoever
 * allocated it (device_dma_alloc_sg), and is theirs to free (#599).
 *
 * Refuses, freeing nothing, while any block is dirty: a cache that holds a
 * filesystem's blocks cannot drop data it was given.  Returns 0 when
 * destroyed, non-zero when refused.
 */
int page_cache_destroy(struct page_cache *pc);

/*
 * #599: the page cache's self-test, run at ext_server's start; *ran counts
 * the cases, *wrong the wrong answers, *failed which ones.
 */
void page_cache_selftest(unsigned int *ran, unsigned int *wrong,
			 unsigned int *failed);	/* bit n-1: case n failed */

/*
 * Set only by page_cache_selftest, while it runs at ext_server's start
 * before any other thread: the failures it provokes on purpose are not
 * printed, so that the boot log does not report a failing disk that is a
 * passing test.
 */
extern int page_cache_quiet;

/*
 * #599: the block, cached and pinned -- or read into the cache by `fill' and
 * then pinned.
 *
 *  - held VALID: pinned, one hit;
 *  - being read by another get: waits for that fill, then looks again;
 *  - not held: a slot is keyed FILLING (one miss), `fill' runs with no lock
 *    held, and the entry is published VALID and pinned -- or, if the fill
 *    failed, withdrawn: an unread entry never leaves this function.
 *
 * Answers 0 and a pinned entry in *ep; 0 and NULL when there is no slot to
 * give (fill not called: read uncached); or the fill's non-zero answer, with
 * nothing cached.  Every pinned entry is given back with page_cache_put.
 */
int page_cache_get(struct page_cache *pc, daddr_t block,
		   page_cache_fill_fn fill, void *ctx,
		   struct page_cache_entry **ep)
	__attribute__((warn_unused_result));

/* #599: give a pin back. */
void page_cache_put(struct page_cache *pc, struct page_cache_entry *e);

/*
 * #599: is `block' held -- cached or being read?  A hint for readahead: no
 * pin, no statistics.
 */
int page_cache_contains(struct page_cache *pc, daddr_t block);

/*
 * Look up a disk block in the cache.
 * On hit: sets *data_out and *size_out, moves entry to MRU, returns 0.
 * On miss: returns -1.
 * The returned pointer is owned by the cache — caller must copy if needed.
 */
int page_cache_lookup(struct page_cache *pc, daddr_t block,
		      vm_offset_t *data_out, vm_size_t *size_out);

/*
 * #599: readahead's way in.  Take a ticket before reading the disk, then
 * offer each block read with it.  page_cache_install copies `data' into a
 * free or clean slot and caches it clean -- it never waits, never writes a
 * block back to make room, and never overwrites: a key already held, cached
 * or being read, answers PAGE_CACHE_PRESENT and is left as it is.
 *
 * The ticket is what keeps old bytes out.  A block dirty in the cache when
 * the ticket was taken has newer bytes than the disk; readahead may have
 * read the old ones.  While that copy stays cached the install finds it
 * present.  Once it has been written back and evicted, pc_forget is past
 * the ticket, and every install with that ticket answers PAGE_CACHE_STALE.
 * Every way a copy leaves the cache raises pc_forget past any older ticket.
 *
 * Answers 0, PAGE_CACHE_PRESENT, PAGE_CACHE_STALE, KERN_RESOURCE_SHORTAGE
 * (no free or clean slot) or KERN_INVALID_ARGUMENT (not a whole block).
 * Caching a clean block is optional: readahead ignores the answer.
 */
uint64_t page_cache_ticket(struct page_cache *pc);
int page_cache_install(struct page_cache *pc, daddr_t block,
		       vm_offset_t data, vm_size_t size, uint64_t ticket);

/*
 * Write a whole block into the cache (#599): the bytes are copied and the
 * block marked dirty in one hold of the cache's lock, into the slot that
 * holds it or into one taken for it -- or the write is refused and nothing
 * changes.  'size' must be the cache's block size (KERN_INVALID_ARGUMENT
 * otherwise).  KERN_RESOURCE_SHORTAGE when there is no slot to give.
 *
 * It replaces page_cache_update, which on a miss dropped the lock, inserted,
 * then marked dirty: a readahead insert of the same block in between made
 * the insert a no-op, and a full cache made it return in silence -- the
 * write lost, and reported as done.
 */
int page_cache_write(struct page_cache *pc, daddr_t block,
		     vm_offset_t data, vm_size_t size)
	__attribute__((warn_unused_result));

/*
 * Synchronize all dirty blocks to disk via the writeback callback.
 * Dirty blocks remain cached (clean) after successful writeback.
 * Returns the number of blocks that failed to write back.
 */
int page_cache_sync(struct page_cache *pc);

/*
 * Print cache statistics (hits, misses, evictions, writebacks, hit rate).
 */
void page_cache_print_stats(struct page_cache *pc);

/*
 * Create a DMA-backed page cache.  The caller pre-allocates a pool of
 * wired pages (e.g. via device_dma_alloc_sg) and passes the base VA,
 * per-page physical addresses, and page count.  Each cache entry gets
 * a fixed-size slot with a known physical address, enabling zero-copy
 * I/O: the caller can pass pc_phys directly to a device driver PRDT.
 *
 * block_size: filesystem block size (e.g. 1024 or 4096).
 * pool_va:    base userspace VA of the DMA pool.
 * pa_list:    array of physical addresses, one per 4 KB page.
 * n_pages:    number of pages in the pool.
 * max_entries is capped to n_pages * (4096 / block_size).
 * Returns NULL on failure.
 */
struct page_cache *page_cache_create_dma(unsigned int max_entries,
					 vm_size_t block_size,
					 vm_offset_t pool_va,
					 vm_address_t *pa_list,
					 unsigned int n_pages,
					 page_cache_writeback_fn writeback,
					 void *ctx);

#endif /* _PAGE_CACHE_H_ */
