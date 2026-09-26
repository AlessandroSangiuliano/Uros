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

/*
 * Hash buckets.  Must scale with the entry count or lookups degrade into
 * long linear chain walks.  The DMA page cache holds up to 4096 entries
 * (#265); with the old 128 buckets that was a load factor of 32 and made
 * cached reads measurably slower.  4096 buckets keeps the load factor at
 * ~1 (array is 16 KB per cache — negligible).
 */
#define PAGE_CACHE_HASH_BUCKETS	4096

/*
 * Writeback callback: called when a dirty block must be flushed to disk.
 * Arguments: opaque context, block number, data pointer, data size,
 * physical address (non-zero for DMA-backed entries).
 * Returns 0 on success, non-zero on failure.
 */
typedef int (*page_cache_writeback_fn)(void *ctx, daddr_t block,
				       vm_offset_t data, vm_size_t size,
				       vm_offset_t phys);

struct page_cache_entry {
	daddr_t			pc_block;	/* disk block number (key) */
	vm_offset_t		pc_data;	/* cached block data */
	vm_size_t		pc_size;	/* size of cached data */
	vm_offset_t		pc_phys;	/* physical addr (0 = vm_allocate'd); full
					   width: a page can be above 4 GiB (#599) */
	int			pc_dirty;	/* block has been modified */
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
 * the cases, *wrong the wrong answers.
 */
void page_cache_selftest(unsigned int *ran, unsigned int *wrong);

/*
 * Look up a disk block in the cache.
 * On hit: sets *data_out and *size_out, moves entry to MRU, returns 0.
 * On miss: returns -1.
 * The returned pointer is owned by the cache — caller must copy if needed.
 */
int page_cache_lookup(struct page_cache *pc, daddr_t block,
		      vm_offset_t *data_out, vm_size_t *size_out);

/*
 * Insert a CLEAN block into the cache: 'size' bytes (at most a slot's) are
 * copied from 'data' into a slot.  Caller retains ownership of 'data'.  If
 * the cache is full, the LRU entry is evicted and its slot reused.
 *
 * A block already cached is left as it is -- it may be dirty, and newer
 * than what the caller read from the disk (#599: this said "update data if
 * so", and never did).
 */
void page_cache_insert(struct page_cache *pc, daddr_t block,
		       vm_offset_t data, vm_size_t size);

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

/*
 * Allocate a cache entry for 'block' without populating data.
 * The entry is inserted into the hash/LRU and its pre-allocated
 * DMA buffer (pc_data/pc_phys) is ready for direct device I/O.
 * Returns NULL if the cache has no DMA pool or allocation fails.
 * Caller must hold NO locks — this function locks internally.
 */
struct page_cache_entry *page_cache_alloc_entry(struct page_cache *pc,
						daddr_t block);

#endif /* _PAGE_CACHE_H_ */
