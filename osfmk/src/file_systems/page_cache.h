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

#define PAGE_CACHE_HASH_BUCKETS	128

/*
 * Writeback callback: called when a dirty block must be flushed to disk.
 * Arguments: opaque context, block number, data pointer, data size.
 * Returns 0 on success, non-zero on failure.
 */
typedef int (*page_cache_writeback_fn)(void *ctx, daddr_t block,
				       vm_offset_t data, vm_size_t size);

struct page_cache_entry {
	daddr_t			pc_block;	/* disk block number (key) */
	vm_offset_t		pc_data;	/* cached block data */
	vm_size_t		pc_size;	/* size of cached data */
	int			pc_dirty;	/* block has been modified */
	struct page_cache_entry	*pc_hash_next;	/* hash chain */
	struct page_cache_entry	*pc_lru_next;	/* toward MRU */
	struct page_cache_entry	*pc_lru_prev;	/* toward LRU */
};

struct page_cache {
	unsigned int		pc_max_entries;
	unsigned int		pc_count;
	unsigned int		pc_hits;
	unsigned int		pc_misses;
	unsigned int		pc_evictions;
	unsigned int		pc_writebacks;
	page_cache_writeback_fn	pc_writeback;	/* dirty block flush callback */
	void			*pc_writeback_ctx; /* opaque context for callback */
	struct page_cache_entry	*pc_pool;	/* pre-allocated entry array */
	struct page_cache_entry	*pc_free;	/* singly-linked free list */
	struct page_cache_entry	*pc_hash[PAGE_CACHE_HASH_BUCKETS];
	/* LRU sentinels: head.pc_lru_next = MRU, tail.pc_lru_prev = LRU */
	struct page_cache_entry	pc_lru_head;
	struct page_cache_entry	pc_lru_tail;
};

/*
 * Create a page cache with the given maximum number of cached blocks.
 * 'writeback' is called when a dirty block must be flushed (eviction or sync).
 * 'ctx' is passed as-is to the callback (typically the device port).
 * If writeback is NULL, dirty blocks are silently discarded on eviction.
 * Returns NULL on allocation failure.
 */
struct page_cache *page_cache_create(unsigned int max_entries,
				     page_cache_writeback_fn writeback,
				     void *ctx);

/*
 * Destroy a page cache, freeing all cached data and the cache itself.
 */
void page_cache_destroy(struct page_cache *pc);

/*
 * Look up a disk block in the cache.
 * On hit: sets *data_out and *size_out, moves entry to MRU, returns 0.
 * On miss: returns -1.
 * The returned pointer is owned by the cache — caller must copy if needed.
 */
int page_cache_lookup(struct page_cache *pc, daddr_t block,
		      vm_offset_t *data_out, vm_size_t *size_out);

/*
 * Insert a block into the cache.  The cache allocates its own buffer
 * and copies 'size' bytes from 'data'.  Caller retains ownership of 'data'.
 * If the cache is full, the LRU entry is evicted (its buffer is freed).
 */
void page_cache_insert(struct page_cache *pc, daddr_t block,
		       vm_offset_t data, vm_size_t size);

/*
 * Invalidate a specific block, freeing its cached data.
 */
void page_cache_invalidate(struct page_cache *pc, daddr_t block);

/*
 * Flush the entire cache, freeing all cached data.
 */
void page_cache_flush(struct page_cache *pc);

/*
 * Mark a cached block as dirty.  Returns 0 on success, -1 if the
 * block is not in the cache.
 */
int page_cache_mark_dirty(struct page_cache *pc, daddr_t block);

/*
 * Update data in a cached block (or insert it) and mark it dirty.
 * Used for write operations: the caller provides new data, the cache
 * copies it, and the block is marked dirty for later writeback.
 */
void page_cache_update(struct page_cache *pc, daddr_t block,
		       vm_offset_t data, vm_size_t size);

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

#endif /* _PAGE_CACHE_H_ */
