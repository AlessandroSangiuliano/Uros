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
 * allocation on the hot path).  Single-threaded — no locking.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mach.h>
#include "page_cache.h"

#define PC_HASH(block) ((unsigned int)(block) % PAGE_CACHE_HASH_BUCKETS)

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

/* Free the data buffer of a cache entry */
static void
entry_free_data(struct page_cache_entry *e)
{
	if (e->pc_data) {
		vm_deallocate(mach_task_self(), e->pc_data, e->pc_size);
		e->pc_data = 0;
		e->pc_size = 0;
	}
}

/* Evict the LRU entry, returning it to the free list */
static struct page_cache_entry *
evict_lru(struct page_cache *pc)
{
	struct page_cache_entry *victim = pc->pc_lru_tail.pc_lru_prev;

	/* Don't evict the head sentinel */
	if (victim == &pc->pc_lru_head)
		return NULL;

	lru_remove(victim);
	hash_remove(pc, victim);
	entry_free_data(victim);
	pc->pc_count--;
	pc->pc_evictions++;

	return victim;
}

struct page_cache *
page_cache_create(unsigned int max_entries)
{
	struct page_cache *pc;
	unsigned int i;

	pc = (struct page_cache *)malloc(sizeof(*pc));
	if (!pc)
		return NULL;

	memset(pc, 0, sizeof(*pc));
	pc->pc_max_entries = max_entries;

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
		pc->pc_pool[i].pc_hash_next = pc->pc_free;
		pc->pc_free = &pc->pc_pool[i];
	}

	return pc;
}

void
page_cache_destroy(struct page_cache *pc)
{
	unsigned int i;

	if (!pc)
		return;

	/* Free all cached data buffers */
	for (i = 0; i < pc->pc_max_entries; i++)
		entry_free_data(&pc->pc_pool[i]);

	free(pc->pc_pool);
	free(pc);
}

int
page_cache_lookup(struct page_cache *pc, daddr_t block,
		  vm_offset_t *data_out, vm_size_t *size_out)
{
	unsigned int h = PC_HASH(block);
	struct page_cache_entry *e;

	for (e = pc->pc_hash[h]; e; e = e->pc_hash_next) {
		if (e->pc_block == block) {
			/* Hit — move to MRU */
			lru_remove(e);
			lru_insert_mru(pc, e);
			*data_out = e->pc_data;
			*size_out = e->pc_size;
			pc->pc_hits++;
			return 0;
		}
	}

	pc->pc_misses++;
	return -1;
}

void
page_cache_insert(struct page_cache *pc, daddr_t block,
		  vm_offset_t data, vm_size_t size)
{
	unsigned int h = PC_HASH(block);
	struct page_cache_entry *e;
	vm_offset_t buf;

	/* Check if already cached (update data if so) */
	for (e = pc->pc_hash[h]; e; e = e->pc_hash_next) {
		if (e->pc_block == block) {
			lru_remove(e);
			lru_insert_mru(pc, e);
			return;
		}
	}

	/* Allocate cache buffer and copy data */
	if (vm_allocate(mach_task_self(), &buf, size, TRUE) != KERN_SUCCESS)
		return;
	memcpy((void *)buf, (void *)data, size);

	/* Get a free entry, evicting if necessary */
	if (pc->pc_free) {
		e = pc->pc_free;
		pc->pc_free = e->pc_hash_next;
		e->pc_hash_next = NULL;
	} else {
		e = evict_lru(pc);
		if (!e) {
			vm_deallocate(mach_task_self(), buf, size);
			return;
		}
	}

	/* Fill entry */
	e->pc_block = block;
	e->pc_data = buf;
	e->pc_size = size;

	/* Insert into hash chain */
	e->pc_hash_next = pc->pc_hash[h];
	pc->pc_hash[h] = e;

	/* Insert at MRU */
	lru_insert_mru(pc, e);
	pc->pc_count++;
}

void
page_cache_invalidate(struct page_cache *pc, daddr_t block)
{
	unsigned int h = PC_HASH(block);
	struct page_cache_entry *e;

	for (e = pc->pc_hash[h]; e; e = e->pc_hash_next) {
		if (e->pc_block == block) {
			lru_remove(e);
			hash_remove(pc, e);
			entry_free_data(e);
			e->pc_block = -1;
			/* Return to free list */
			e->pc_hash_next = pc->pc_free;
			pc->pc_free = e;
			pc->pc_count--;
			return;
		}
	}
}

void
page_cache_flush(struct page_cache *pc)
{
	unsigned int i;

	for (i = 0; i < pc->pc_max_entries; i++) {
		struct page_cache_entry *e = &pc->pc_pool[i];
		if (e->pc_data) {
			lru_remove(e);
			hash_remove(pc, e);
			entry_free_data(e);
			e->pc_block = -1;
			e->pc_hash_next = pc->pc_free;
			pc->pc_free = e;
		}
	}
	pc->pc_count = 0;
}

void
page_cache_print_stats(struct page_cache *pc)
{
	unsigned int total = pc->pc_hits + pc->pc_misses;
	unsigned int hit_pct = total ? (pc->pc_hits * 100) / total : 0;

	printf("page cache: %u entries, %u/%u hits/misses (%u%%), "
	       "%u evictions\n",
	       pc->pc_count, pc->pc_hits, pc->pc_misses,
	       hit_pct, pc->pc_evictions);
}
