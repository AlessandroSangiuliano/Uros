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
 * page_cache.c — Generic block-level page cache for filesystem servers
 *
 * Hash table with chaining for O(1) lookup, doubly-linked LRU list for
 * eviction.  All entries are pre-allocated at creation time (no dynamic
 * allocation on the hot path).  Thread-safe via pc_lock mutex.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mach.h>
#include "page_cache.h"

#define PC_HASH(block) ((unsigned int)(block) % PAGE_CACHE_HASH_BUCKETS)

int	page_cache_quiet;	/* see page_cache.h */

/* Remove entry from LRU list */
static void
lru_remove(struct page_cache_entry *e)
{
	e->pc_lru_prev->pc_lru_next = e->pc_lru_next;
	e->pc_lru_next->pc_lru_prev = e->pc_lru_prev;
}

/* Insert entry at MRU end (just after head sentinel) */
static void
lru_insert_mru(struct page_cache *pc, struct page_cache_entry *e)
{
	e->pc_lru_next = pc->pc_lru_head.pc_lru_next;
	e->pc_lru_prev = &pc->pc_lru_head;
	pc->pc_lru_head.pc_lru_next->pc_lru_prev = e;
	pc->pc_lru_head.pc_lru_next = e;
}

/* Remove entry from hash chain */
static void
hash_remove(struct page_cache *pc, struct page_cache_entry *e)
{
	unsigned int h = PC_HASH(e->pc_block);
	struct page_cache_entry **pp = &pc->pc_hash[h];

	while (*pp) {
		if (*pp == e) {
			*pp = e->pc_hash_next;
			e->pc_hash_next = NULL;
			return;
		}
		pp = &(*pp)->pc_hash_next;
	}
}

/*
 * #599: how take_victim may make room.  PC_TAKE_CLEAN never writes a block
 * back for it -- a readahead insert has no business costing a disk write --
 * and PC_TAKE_ANY may write back up to PC_EVICT_TRIES dirty victims.
 */
#define PC_TAKE_CLEAN	0
#define PC_TAKE_ANY	1
#define PC_VICTIM_SCAN	PAGE_CACHE_VICTIM_SCAN
#define PC_EVICT_TRIES	4

/* Can this entry's slot be taken?  #599: the one predicate eviction asks. */
static int
evictable(const struct page_cache_entry *e)
{
	/* #599: a cached block nobody holds a pointer into, not being written */
	return e->pc_state == PC_VALID && e->pc_refs == 0 && !e->pc_busy;
}

/*
 * The entry keyed `block' once no fill of it is in flight, or NULL; with
 * pc_lock held, which the wait gives up and takes back.  #599: every lookup
 * of a key waits out a FILLING entry -- its slot is not data yet -- and looks
 * the key up again afterwards, since the fill may have failed and the entry
 * gone.
 */
static struct page_cache_entry *
find_ready(struct page_cache *pc, daddr_t block)
{
	struct page_cache_entry *e;

	for (;;) {
		for (e = pc->pc_hash[PC_HASH(block)]; e; e = e->pc_hash_next)
			if (e->pc_block == block)
				break;
		if (e == NULL || e->pc_state != PC_FILLING)
			return e;
		pc->pc_nwaiters++;
		pthread_cond_wait(&pc->pc_cond, &pc->pc_lock);
		pc->pc_nwaiters--;
	}
}

/* Key a slot for `block', in `state', at MRU. */
static void
key_entry(struct page_cache *pc, struct page_cache_entry *e, daddr_t block,
	  int state)
{
	unsigned int h = PC_HASH(block);

	e->pc_block = block;
	e->pc_state = state;
	e->pc_busy = 0;
	e->pc_dirty = 0;
	e->pc_wfail = 0;
	e->pc_hash_next = pc->pc_hash[h];
	pc->pc_hash[h] = e;
	lru_insert_mru(pc, e);
	pc->pc_count++;
}

/* Back to the free list: unkeyed, unpinned, clean. */
static void
free_entry(struct page_cache *pc, struct page_cache_entry *e)
{
	e->pc_block = -1;
	e->pc_state = PC_FREE;
	e->pc_refs = 0;
	e->pc_dirty = 0;
	e->pc_hash_next = pc->pc_free;
	pc->pc_free = e;
}

/* Detach an entry that is being taken, and count it. */
static struct page_cache_entry *
take_detach(struct page_cache *pc, struct page_cache_entry *e)
{
	lru_remove(e);
	hash_remove(pc, e);
	pc->pc_count--;
	pc->pc_evictions++;
	e->pc_block = -1;
	e->pc_state = PC_FREE;
	e->pc_refs = 0;
	e->pc_dirty = 0;
	e->pc_wfail = 0;
	return e;
}

/*
 * A slot for a new block, under pc_lock; never waits.
 *
 * #599: the eviction this replaces wrote a dirty victim back, IGNORED THE
 * ANSWER and took the slot anyway -- a block the device refused was dropped,
 * with only a printf in ext_server to say so.  A dirty entry now leaves only
 * once the disk has it: a failed writeback leaves it cached and dirty, moved
 * to MRU so the next search does not start from it again, and said once
 * (pc_wfail); its answer is kept in *wb_rc for a caller that ends up with no
 * slot.
 *
 * The writeback runs under pc_lock, as it did.  It cannot deadlock: the
 * callback takes no lock of the cache's, and the block server it calls is
 * single-threaded and never calls back into ext_server.
 *
 * Order: the free list; a clean entry among the PC_VICTIM_SCAN oldest; (ANY)
 * up to PC_EVICT_TRIES dirty victims written back; any clean entry at all;
 * none.
 */
static struct page_cache_entry *
take_victim(struct page_cache *pc, int mode, int *wb_rc)
{
	struct page_cache_entry *e, *prev;
	unsigned int n;
	int tries = 0, rc;

	if (pc->pc_free) {
		e = pc->pc_free;
		pc->pc_free = e->pc_hash_next;
		e->pc_hash_next = NULL;
		return e;
	}

	for (e = pc->pc_lru_tail.pc_lru_prev, n = 0;
	     e != &pc->pc_lru_head && n < PC_VICTIM_SCAN;
	     e = e->pc_lru_prev, n++)
		if (evictable(e) && !e->pc_dirty)
			return take_detach(pc, e);

	for (e = pc->pc_lru_tail.pc_lru_prev;
	     mode == PC_TAKE_ANY && e != &pc->pc_lru_head &&
	     tries < PC_EVICT_TRIES;
	     e = prev) {
		prev = e->pc_lru_prev;
		if (!evictable(e) || !e->pc_dirty)
			continue;
		tries++;
		rc = pc->pc_writeback(pc->pc_writeback_ctx, e->pc_block,
				      e->pc_data, e->pc_size, e->pc_phys);
		if (rc == 0) {
			pc->pc_writebacks++;
			return take_detach(pc, e);
		}
		if (wb_rc != NULL)
			*wb_rc = rc;
		if (!e->pc_wfail && !page_cache_quiet)
			printf("page cache: block %lu could not be written back "
			       "(%d) — it stays cached and dirty, and is tried "
			       "again at the next sync\n",
			       (unsigned long)e->pc_block, rc);
		e->pc_wfail = 1;
		lru_remove(e);
		lru_insert_mru(pc, e);
	}

	for (e = pc->pc_lru_tail.pc_lru_prev; e != &pc->pc_lru_head;
	     e = e->pc_lru_prev)
		if (evictable(e) && !e->pc_dirty)
			return take_detach(pc, e);

	return NULL;
}

/*
 * The cache and its entries, with no slots yet: page_cache_create gives it a
 * slab, page_cache_create_dma the caller's pool.
 */
static struct page_cache *
page_cache_alloc(unsigned int max_entries, vm_size_t block_size,
		 page_cache_writeback_fn writeback, void *ctx)
{
	struct page_cache *pc;
	unsigned int i;

	/* #573: see page_cache.h -- a cache that could lose dirty blocks is
	 * not one this function makes. */
	if (writeback == NULL || max_entries == 0 || block_size == 0)
		return NULL;

	pc = (struct page_cache *)malloc(sizeof(*pc));
	if (!pc)
		return NULL;

	memset(pc, 0, sizeof(*pc));
	pthread_mutex_init(&pc->pc_lock, NULL);
	pthread_mutex_init(&pc->pc_sync_lock, NULL);	/* #599 */
	pthread_cond_init(&pc->pc_cond, NULL);		/* #599 */
	pc->pc_max_entries = max_entries;
	pc->pc_block_size = block_size;
	pc->pc_writeback = writeback;
	pc->pc_writeback_ctx = ctx;

	/* Initialize LRU sentinels (empty list: head <-> tail) */
	pc->pc_lru_head.pc_lru_next = &pc->pc_lru_tail;
	pc->pc_lru_head.pc_lru_prev = NULL;
	pc->pc_lru_tail.pc_lru_prev = &pc->pc_lru_head;
	pc->pc_lru_tail.pc_lru_next = NULL;

	/* Pre-allocate entry pool and build free list */
	pc->pc_pool = (struct page_cache_entry *)malloc(
		max_entries * sizeof(struct page_cache_entry));
	if (!pc->pc_pool) {
		free(pc);
		return NULL;
	}
	memset(pc->pc_pool, 0, max_entries * sizeof(struct page_cache_entry));

	pc->pc_free = NULL;
	for (i = 0; i < max_entries; i++) {
		pc->pc_pool[i].pc_block = -1;
		pc->pc_pool[i].pc_size = block_size;
		pc->pc_pool[i].pc_hash_next = pc->pc_free;
		pc->pc_free = &pc->pc_pool[i];
	}

	return pc;
}

/*
 * #599: a non-DMA cache owns one slab, cut into fixed slots at creation.
 * It allocated a buffer per insertion and freed it at eviction, and the
 * update path freed a buffer before an allocation that could fail -- a
 * published entry with no data.  No slot moves or goes away now while the
 * cache exists.
 */
struct page_cache *
page_cache_create(unsigned int max_entries, vm_size_t block_size,
		  page_cache_writeback_fn writeback, void *ctx)
{
	struct page_cache *pc = page_cache_alloc(max_entries, block_size,
						 writeback, ctx);
	unsigned int i;

	if (pc == NULL)
		return NULL;

	pc->pc_slab_size = (vm_size_t)max_entries * block_size;
	if (vm_allocate(mach_task_self(), &pc->pc_slab, pc->pc_slab_size,
			TRUE) != KERN_SUCCESS) {
		free(pc->pc_pool);
		free(pc);
		return NULL;
	}
	for (i = 0; i < max_entries; i++)
		pc->pc_pool[i].pc_data = pc->pc_slab + (vm_offset_t)i *
					 block_size;
	return pc;
}

int
page_cache_destroy(struct page_cache *pc)
{
	unsigned int i;

	if (!pc)
		return 0;

	/*
	 * #599: refused, with nothing freed, while a block is dirty.  It wrote
	 * each dirty block back and ignored the answer, then freed the lot --
	 * the DMA pool included, which was never the cache's.
	 */
	pthread_mutex_lock(&pc->pc_lock);
	for (i = 0; i < pc->pc_max_entries; i++)
		if (pc->pc_pool[i].pc_state != PC_FREE &&
		    (pc->pc_pool[i].pc_dirty || pc->pc_pool[i].pc_busy ||
		     pc->pc_pool[i].pc_refs != 0 ||
		     pc->pc_pool[i].pc_state != PC_VALID)) {
			pthread_mutex_unlock(&pc->pc_lock);
			return -1;
		}
	pthread_mutex_unlock(&pc->pc_lock);

	if (pc->pc_slab != 0)
		(void) vm_deallocate(mach_task_self(), pc->pc_slab,
				     pc->pc_slab_size);
	free(pc->pc_pool);
	free(pc);
	return 0;
}

int
page_cache_lookup(struct page_cache *pc, daddr_t block,
		  vm_offset_t *data_out, vm_size_t *size_out)
{
	struct page_cache_entry *e;

	pthread_mutex_lock(&pc->pc_lock);
	e = find_ready(pc, block);	/* #599: never an entry being read */
	if (e != NULL) {
		/* Hit — move to MRU */
		lru_remove(e);
		lru_insert_mru(pc, e);
		*data_out = e->pc_data;
		*size_out = e->pc_size;
		pc->pc_hits++;
		pthread_mutex_unlock(&pc->pc_lock);
		return 0;
	}

	pc->pc_misses++;
	pthread_mutex_unlock(&pc->pc_lock);
	return -1;
}

void
page_cache_insert(struct page_cache *pc, daddr_t block,
		  vm_offset_t data, vm_size_t size)
{
	struct page_cache_entry *e;

	pthread_mutex_lock(&pc->pc_lock);

	/*
	 * Already cached: left as it is, and only moved to MRU.  It may be
	 * dirty, and newer than what the caller read from the disk (#599: this
	 * comment said "update data if so", and it never did).  A key being
	 * read is waited out first.
	 */
	e = find_ready(pc, block);
	if (e != NULL) {
		lru_remove(e);
		lru_insert_mru(pc, e);
		pthread_mutex_unlock(&pc->pc_lock);
		return;
	}

	/* A free or clean slot: an insert never costs a writeback (#599) */
	e = take_victim(pc, PC_TAKE_CLEAN, NULL);
	if (!e) {
		pthread_mutex_unlock(&pc->pc_lock);
		return;
	}

	/* #599: a fixed slot in either kind of cache */
	memcpy((void *)e->pc_data, (void *)data,
	       size < e->pc_size ? size : e->pc_size);
	key_entry(pc, e, block, PC_VALID);

	pthread_mutex_unlock(&pc->pc_lock);
}

int
page_cache_write(struct page_cache *pc, daddr_t block, vm_offset_t data,
		 vm_size_t size)
{
	struct page_cache_entry *e;

	if (size != pc->pc_block_size)
		return KERN_INVALID_ARGUMENT;

	pthread_mutex_lock(&pc->pc_lock);
	/* #599: a key being read is waited out, so the write lands after it */
	e = find_ready(pc, block);

	if (e != NULL) {
		lru_remove(e);
		lru_insert_mru(pc, e);
	} else {
		int wb_rc = KERN_RESOURCE_SHORTAGE;

		/*
		 * #599: no slot is the last refused writeback's answer, or a
		 * shortage -- the write fails and nothing is lost.
		 */
		e = take_victim(pc, PC_TAKE_ANY, &wb_rc);
		if (!e) {
			pthread_mutex_unlock(&pc->pc_lock);
			return wb_rc;
		}
		key_entry(pc, e, block, PC_VALID);
	}

	/* #599: the copy and the dirty bit in the same hold */
	memcpy((void *)e->pc_data, (void *)data, size);
	if (!e->pc_dirty)
		e->pc_dirty_seq = ++pc->pc_seq;
	e->pc_wgen++;
	e->pc_dirty = 1;
	pthread_mutex_unlock(&pc->pc_lock);
	return KERN_SUCCESS;
}

/*
 * #599: see page_cache.h.  The fill runs outside pc_lock on a slot keyed
 * FILLING and pinned by this call: every other lookup of the key waits
 * (find_ready), eviction skips it (evictable), and it is published VALID or
 * withdrawn to the free list in one hold, with a broadcast either way.
 */
int
page_cache_get(struct page_cache *pc, daddr_t block, page_cache_fill_fn fill,
	       void *ctx, struct page_cache_entry **ep)
{
	struct page_cache_entry *e;
	int rc;

	*ep = NULL;
	pthread_mutex_lock(&pc->pc_lock);
	e = find_ready(pc, block);
	if (e != NULL) {
		e->pc_refs++;
		lru_remove(e);
		lru_insert_mru(pc, e);
		pc->pc_hits++;
		pthread_mutex_unlock(&pc->pc_lock);
		*ep = e;
		return 0;
	}

	pc->pc_misses++;
	e = take_victim(pc, PC_TAKE_ANY, NULL);
	if (e == NULL) {
		pthread_mutex_unlock(&pc->pc_lock);
		return 0;		/* no slot: the caller reads uncached */
	}
	key_entry(pc, e, block, PC_FILLING);
	e->pc_refs = 1;
	pthread_mutex_unlock(&pc->pc_lock);

	rc = fill(ctx, block, e->pc_data, e->pc_size, e->pc_phys);

	pthread_mutex_lock(&pc->pc_lock);
	if (rc == 0) {
		e->pc_state = PC_VALID;
		*ep = e;
	} else {
		lru_remove(e);
		hash_remove(pc, e);
		pc->pc_count--;
		free_entry(pc, e);
	}
	pthread_cond_broadcast(&pc->pc_cond);
	pthread_mutex_unlock(&pc->pc_lock);
	return rc;
}

void
page_cache_put(struct page_cache *pc, struct page_cache_entry *e)
{
	pthread_mutex_lock(&pc->pc_lock);
	if (e->pc_refs > 0)
		e->pc_refs--;
	if (e->pc_state == PC_ORPHAN && e->pc_refs == 0)
		free_entry(pc, e);
	pthread_mutex_unlock(&pc->pc_lock);
}

int
page_cache_contains(struct page_cache *pc, daddr_t block)
{
	struct page_cache_entry *e;

	pthread_mutex_lock(&pc->pc_lock);
	for (e = pc->pc_hash[PC_HASH(block)]; e; e = e->pc_hash_next)
		if (e->pc_block == block)
			break;
	pthread_mutex_unlock(&pc->pc_lock);
	return e != NULL;
}

/*
 * Maximum dirty entries collected per sync pass.
 * Kept small to limit stack usage (each slot = 12 bytes on i386).
 */
#define SYNC_BATCH	64

struct sync_entry {
	struct page_cache_entry *e;	/* #599: the entry itself, pinned busy */
	unsigned int	wgen;		/* its write count when collected */
	daddr_t		block;
	vm_offset_t	data;
	vm_size_t	size;
	vm_offset_t	phys;
};

/* Sort batch by block number (insertion sort, N <= 64) */
static void
batch_sort(struct sync_entry *b, int n)
{
	int i, j;
	struct sync_entry tmp;

	for (i = 1; i < n; i++) {
		tmp = b[i];
		j = i - 1;
		while (j >= 0 && b[j].block > tmp.block) {
			b[j + 1] = b[j];
			j--;
		}
		b[j + 1] = tmp;
	}
}

/*
 * Post-writeback bookkeeping for `count' collected entries, under lock:
 * always drop the busy pin; mark an entry clean only if the write landed and
 * nothing wrote into it since it was collected (#599: by the entry pointer
 * and its write count -- by block number, a write that landed during the
 * writeback was marked clean without ever reaching the disk).
 */
static void
mark_range_done(struct page_cache *pc, const struct sync_entry *b, int count,
		int success)
{
	int i;

	pthread_mutex_lock(&pc->pc_lock);
	for (i = 0; i < count; i++) {
		struct page_cache_entry *ce = b[i].e;

		ce->pc_busy = 0;
		if (success && ce->pc_wgen == b[i].wgen) {
			ce->pc_dirty = 0;
			ce->pc_wfail = 0;
			pc->pc_writebacks++;
		} else if (!success && !ce->pc_wfail) {
			/* #599: said once, not every 5 s */
			if (!page_cache_quiet)
				printf("page cache: block %lu could not be "
				       "written back — it stays dirty, and "
				       "is tried again at the next sync\n",
				       (unsigned long)ce->pc_block);
			ce->pc_wfail = 1;
		}
	}
	pthread_mutex_unlock(&pc->pc_lock);
}

int
page_cache_sync(struct page_cache *pc)
{
	struct sync_entry batch[SYNC_BATCH];
	int n, i, failures = 0;
	uint64_t horizon;
	unsigned int call;
	struct page_cache_entry *e;

	/*
	 * #599: one sync at a time (pc_sync_lock), and each dirty block that
	 * was dirty when this call began is tried once by it: the horizon is
	 * pc_seq now, and pc_tried is this call's number.  That is what ends
	 * the loop -- a block that fails, or one re-dirtied behind the sync,
	 * is not collected again by the same call.  The old skip-by-count
	 * guessed at the same thing and was wrong when another sync ran.
	 *
	 * #384: batch entries are pinned busy, so eviction leaves their slots
	 * alone until the write lands, and every batch restarts from the LRU
	 * tail.
	 */
	pthread_mutex_lock(&pc->pc_sync_lock);
	pthread_mutex_lock(&pc->pc_lock);
	horizon = pc->pc_seq;
	call = ++pc->pc_sync_calls;
	pthread_mutex_unlock(&pc->pc_lock);

	for (;;) {
		/* Phase 1: collect dirty entries under lock, pin them */
		n = 0;
		pthread_mutex_lock(&pc->pc_lock);
		e = pc->pc_lru_tail.pc_lru_prev;
		while (e != &pc->pc_lru_head && n < SYNC_BATCH) {
			if (e->pc_state == PC_VALID && e->pc_dirty &&
			    !e->pc_busy &&
			    e->pc_dirty_seq <= horizon && e->pc_tried != call) {
				e->pc_busy = 1;
				e->pc_tried = call;
				batch[n].e     = e;
				batch[n].wgen  = e->pc_wgen;
				batch[n].block = e->pc_block;
				batch[n].data  = e->pc_data;
				batch[n].size  = e->pc_size;
				batch[n].phys  = e->pc_phys;
				n++;
			}
			e = e->pc_lru_prev;
		}
		pthread_mutex_unlock(&pc->pc_lock);

		if (n == 0)
			break;

		/* Phase 2: sort by block number to find contiguous runs */
		batch_sort(batch, n);

		/* Phase 3: merge contiguous runs and write back */
		i = 0;
		while (i < n) {
			daddr_t run_start = batch[i].block;
			vm_size_t blksz = batch[i].size;
			int run_len = 1;

			/* Extend run while blocks are contiguous and
			 * same size */
			while (i + run_len < n &&
			       batch[i + run_len].block ==
				   run_start + run_len &&
			       batch[i + run_len].size == blksz)
				run_len++;

			if (run_len == 1) {
				/* Single block — write directly */
				int ret = pc->pc_writeback(
					pc->pc_writeback_ctx,
					run_start,
					batch[i].data, blksz,
					batch[i].phys);
				if (ret != 0)
					failures++;
				mark_range_done(pc, &batch[i], 1, ret == 0);
				i++;
			} else {
				/* Merged write: copy into contiguous
				 * buffer */
				vm_size_t total = (vm_size_t)run_len *
						  blksz;
				vm_offset_t mbuf;
				int j, ret;

				if (vm_allocate(mach_task_self(), &mbuf,
						total, TRUE)
				    != KERN_SUCCESS) {
					/* Fallback: write one by one */
					for (j = 0; j < run_len; j++) {
						ret = pc->pc_writeback(
						    pc->pc_writeback_ctx,
						    batch[i + j].block,
						    batch[i + j].data,
						    blksz,
						    batch[i + j].phys);
						if (ret != 0)
							failures++;
						mark_range_done(pc,
						    &batch[i + j], 1,
						    ret == 0);
					}
					i += run_len;
					continue;
				}

				for (j = 0; j < run_len; j++)
					memcpy((void *)(mbuf + j * blksz),
					       (void *)batch[i + j].data,
					       blksz);

				ret = pc->pc_writeback(
					pc->pc_writeback_ctx,
					run_start, mbuf, total, 0);

				vm_deallocate(mach_task_self(), mbuf,
					      total);

				if (ret != 0)
					failures += run_len;
				mark_range_done(pc, &batch[i], run_len,
						ret == 0);
				i += run_len;
			}
		}
	}

	pthread_mutex_unlock(&pc->pc_sync_lock);
	return failures;
}

struct page_cache *
page_cache_create_dma(unsigned int max_entries, vm_size_t block_size,
		      vm_offset_t pool_va, vm_address_t *pa_list,
		      unsigned int n_pages,
		      page_cache_writeback_fn writeback, void *ctx)
{
	struct page_cache *pc;
	unsigned int entries_per_page, max_possible, i;

	if (block_size == 0 || block_size > 4096 || n_pages == 0)
		return NULL;

	entries_per_page = 4096 / (unsigned int)block_size;
	max_possible = n_pages * entries_per_page;
	if (max_entries > max_possible)
		max_entries = max_possible;

	pc = page_cache_alloc(max_entries, block_size, writeback, ctx);
	if (!pc)
		return NULL;

	/* Set up DMA pool metadata */
	pc->pc_dma_pool = pool_va;
	pc->pc_dma_pool_size = (vm_size_t)n_pages * 4096;

	/*
	 * Pre-assign data buffers and physical addresses to each entry.
	 *
	 * #599: pa_list is read here and nowhere after.  A copy of it, kept in
	 * the cache beside two sizes, was written at half its width (a
	 * sizeof(unsigned int) left over from before #520) and read by nothing;
	 * it went, with the sizes.  pc_phys is the only record of a page's
	 * address, and it is as wide as the list it comes from.
	 */
	for (i = 0; i < max_entries; i++) {
		unsigned int page_idx = i / entries_per_page;
		unsigned int offset = (i % entries_per_page) *
				      (unsigned int)block_size;

		pc->pc_pool[i].pc_data = pool_va + page_idx * 4096 + offset;
		pc->pc_pool[i].pc_phys = pa_list[page_idx] + offset;
		pc->pc_pool[i].pc_size = block_size;
	}

	return pc;
}

struct page_cache_entry *
page_cache_alloc_entry(struct page_cache *pc, daddr_t block)
{
	struct page_cache_entry *e;

	if (!pc->pc_dma_pool)
		return NULL;

	pthread_mutex_lock(&pc->pc_lock);

	/* Check if already cached (#599: a key being read is waited out) */
	e = find_ready(pc, block);
	if (e != NULL) {
		lru_remove(e);
		lru_insert_mru(pc, e);
		pc->pc_hits++;
		pthread_mutex_unlock(&pc->pc_lock);
		return e;
	}

	pc->pc_misses++;

	/* Get a free entry or evict (#599: keeping what cannot be written) */
	e = take_victim(pc, PC_TAKE_ANY, NULL);
	if (!e) {
		pthread_mutex_unlock(&pc->pc_lock);
		return NULL;
	}

	/* Set up the entry (data/phys already assigned from pool) */
	key_entry(pc, e, block, PC_VALID);

	pthread_mutex_unlock(&pc->pc_lock);
	return e;
}

void
page_cache_print_stats(struct page_cache *pc)
{
	unsigned int count, hits, misses, evictions, writebacks;
	unsigned int total, hit_pct;

	pthread_mutex_lock(&pc->pc_lock);
	count = pc->pc_count;
	hits = pc->pc_hits;
	misses = pc->pc_misses;
	evictions = pc->pc_evictions;
	writebacks = pc->pc_writebacks;
	pthread_mutex_unlock(&pc->pc_lock);

	total = hits + misses;
	hit_pct = total ? (hits * 100) / total : 0;

	printf("page cache: %u entries, %u/%u hits/misses (%u%%), "
	       "%u evictions, %u writebacks\n",
	       count, hits, misses, hit_pct, evictions, writebacks);
}
