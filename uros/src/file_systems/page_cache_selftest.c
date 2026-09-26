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
 * page_cache_selftest.c -- the page cache asked about itself, at every start
 * of ext_server (#599).
 *
 * Each case builds a small cache of its own on a slab -- never the mount's --
 * with writeback test doubles that succeed or fail on demand, and checks one
 * property the #599 page-cache work made true.  Each is written to fail when
 * the fix it covers is taken out.  Running inside ext_server, on the real
 * libpthreads of each target, is the point: a host test would run another
 * library.
 */

#include <string.h>
#include <pthread.h>
#include <mach.h>
#include <mach/thread_switch.h>
#include "page_cache.h"

#define ST_BLOCK	1024u

#define ST_EIO		2500		/* D_IO_ERROR, as a device answers */

/*
 * The writeback double: counts the calls, remembers the last block and its
 * first byte, and answers st_wb_answer for every block, or fails only
 * st_wb_fail_block ((daddr_t)-1: none).
 */
static unsigned int	st_wb_calls;
static int		st_wb_answer;
static daddr_t		st_wb_fail_block = (daddr_t)-1;
static daddr_t		st_wb_last_block;
static unsigned char	st_wb_last_byte;

/*
 * The gate: a writeback of st_gate_block says it has arrived and waits until
 * the case opens it -- the one way to hold a sync in the middle of a write
 * and do something to the cache meanwhile.  Initialised at the self-test's
 * start: libpthreads refuses a zeroed mutex (EINVAL, not locked).
 */
static pthread_mutex_t	st_gate_lock;
static pthread_cond_t	st_gate_cond;
static daddr_t		st_gate_block = (daddr_t)-1;
static volatile int	st_gate_entered, st_gate_open;

static void
st_gate_wait(daddr_t block)
{
	if (block != st_gate_block)
		return;
	pthread_mutex_lock(&st_gate_lock);
	st_gate_entered = 1;
	while (!st_gate_open)
		pthread_cond_wait(&st_gate_cond, &st_gate_lock);
	pthread_mutex_unlock(&st_gate_lock);
}

static void
st_gate_release(void)
{
	pthread_mutex_lock(&st_gate_lock);
	st_gate_open = 1;
	pthread_cond_broadcast(&st_gate_cond);
	pthread_mutex_unlock(&st_gate_lock);
}

static void
st_gate_reset(daddr_t block)
{
	st_gate_block = block;
	st_gate_entered = 0;
	st_gate_open = 0;
}

/* A bounded wait for a flag: 1 ms at a time, 5 s at most. */
static int
st_wait_for(volatile int *flag)
{
	unsigned int i;

	for (i = 0; i < 5000 && !*flag; i++)
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 1);
	return *flag;
}

/* A clean block offered with a ticket taken now, as readahead would. */
static int
st_install(struct page_cache *pc, daddr_t b, const unsigned char *blk)
{
	return page_cache_install(pc, b, (vm_offset_t)blk, ST_BLOCK,
				  page_cache_ticket(pc));
}

static int
st_writeback(void *ctx, daddr_t block, vm_offset_t data, vm_size_t size,
	     vm_offset_t phys)
{
	(void)ctx;
	(void)size;
	(void)phys;
	st_gate_wait(block);
	st_wb_calls++;
	if (st_wb_answer != 0)
		return st_wb_answer;
	if (block == st_wb_fail_block)
		return ST_EIO;
	st_wb_last_block = block;
	st_wb_last_byte = *(const unsigned char *)data;
	return 0;
}

/*
 * P1: slots are fixed.  A two-entry cache re-keyed three times over keeps
 * every entry's slot where creation put it, inside the cache's own slab.
 */
static int
st_slots_are_fixed(void)
{
	struct page_cache	*pc;
	unsigned char		 blk[ST_BLOCK];
	unsigned int		 i, b;
	int			 ok = 1;

	pc = page_cache_create(2, ST_BLOCK, st_writeback, 0);
	if (pc == 0)
		return 0;
	for (b = 1; b <= 6; b++) {
		memset(blk, (int)b, sizeof(blk));
		if (st_install(pc, (daddr_t)b, blk) != 0)
			ok = 0;
		for (i = 0; i < 2; i++)
			if (pc->pc_pool[i].pc_data !=
			    pc->pc_slab + (vm_offset_t)i * ST_BLOCK ||
			    pc->pc_pool[i].pc_size != ST_BLOCK)
				ok = 0;
	}
	if (page_cache_destroy(pc) != 0)
		ok = 0;
	return ok;
}

/*
 * P2: a dirty cache is not destroyed.  With one dirty block, destroy refuses
 * and the cache is still there to sync; once the block is written back,
 * destroy succeeds.
 */
static int
st_destroy_refuses_dirty(void)
{
	struct page_cache	*pc;
	unsigned char		 blk[ST_BLOCK];
	int			 refused, destroyed;

	pc = page_cache_create(2, ST_BLOCK, st_writeback, 0);
	if (pc == 0)
		return 0;
	memset(blk, 0x5A, sizeof(blk));
	if (page_cache_write(pc, 7, (vm_offset_t)blk, sizeof(blk)) != 0)
		return 0;
	refused = page_cache_destroy(pc) != 0;
	if (!refused)
		return 0;		/* freed: the cache is gone */
	st_wb_answer = 0;
	(void) page_cache_sync(pc);
	destroyed = page_cache_destroy(pc) == 0;
	return destroyed;
}

/* The entry that holds `b', or 0. */
static struct page_cache_entry *
st_entry(struct page_cache *pc, daddr_t b)
{
	unsigned int i;

	for (i = 0; i < pc->pc_max_entries; i++)
		if (pc->pc_pool[i].pc_block == b)
			return &pc->pc_pool[i];
	return 0;
}

static int
st_holds(struct page_cache *pc, daddr_t b, unsigned char v, int dirty)
{
	struct page_cache_entry *e = st_entry(pc, b);
	unsigned int i;

	if (e == 0 || (e->pc_dirty != 0) != (dirty != 0))
		return 0;
	for (i = 0; i < ST_BLOCK; i++)
		if (((const unsigned char *)e->pc_data)[i] != v)
			return 0;
	return 1;
}

/* Written back and destroyed, so a case leaves nothing behind. */
static int
st_done(struct page_cache *pc)
{
	st_wb_answer = 0;
	st_wb_fail_block = (daddr_t)-1;
	(void) page_cache_sync(pc);
	return page_cache_destroy(pc) == 0;
}

static int
st_write_byte(struct page_cache *pc, daddr_t b, unsigned char v)
{
	unsigned char blk[ST_BLOCK];

	memset(blk, v, sizeof(blk));
	return page_cache_write(pc, b, (vm_offset_t)blk, sizeof(blk));
}

/*
 * P6: eviction keeps what it cannot write.  Two dirty blocks, the oldest
 * unwritable: a write of a third takes the other one's slot, and the
 * unwritable one stays cached and dirty -- and a later good sync writes
 * exactly its bytes.
 */
static int
st_evict_keeps_unwritable(void)
{
	struct page_cache	*pc = page_cache_create(2, ST_BLOCK,
							st_writeback, 0);
	int			 ok = 1;

	if (pc == 0)
		return 0;
	if (st_write_byte(pc, 1, 0xA1) != 0 || st_write_byte(pc, 2, 0xA2) != 0)
		ok = 0;
	st_wb_fail_block = 1;
	if (st_write_byte(pc, 3, 0xA3) != 0 || !st_holds(pc, 1, 0xA1, 1) ||
	    st_entry(pc, 2) != 0 || !st_holds(pc, 3, 0xA3, 1))
		ok = 0;
	st_wb_fail_block = (daddr_t)-1;
	st_wb_last_block = (daddr_t)-1;
	(void) page_cache_sync(pc);
	if (!st_holds(pc, 1, 0xA1, 0))
		ok = 0;
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/*
 * P7: with every block dirty and unwritable, a write of another fails with
 * the device's answer after at most four writebacks -- and every block is
 * still there, dirty, with its bytes.
 */
static int
st_write_fails_when_nothing_writes(void)
{
	struct page_cache	*pc = page_cache_create(2, ST_BLOCK,
							st_writeback, 0);
	unsigned int		 calls;
	int			 ok = 1, rc;

	if (pc == 0)
		return 0;
	if (st_write_byte(pc, 1, 0xB1) != 0 || st_write_byte(pc, 2, 0xB2) != 0)
		ok = 0;
	st_wb_answer = ST_EIO;
	calls = st_wb_calls;
	rc = st_write_byte(pc, 3, 0xB3);
	if (rc != ST_EIO || st_wb_calls - calls > 4 || st_entry(pc, 3) != 0 ||
	    !st_holds(pc, 1, 0xB1, 1) || !st_holds(pc, 2, 0xB2, 1))
		ok = 0;
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/*
 * P8: a clean block beyond the scan window is still found before a write is
 * refused.  All but the newest entry dirty and unwritable; the newest clean:
 * a write of another block takes its slot.
 */
static int
st_clean_found_past_the_window(void)
{
	unsigned int		 n = PAGE_CACHE_VICTIM_SCAN + 2, b;
	struct page_cache	*pc = page_cache_create(n, ST_BLOCK,
							st_writeback, 0);
	unsigned char		 blk[ST_BLOCK];
	int			 ok = 1;

	if (pc == 0)
		return 0;
	for (b = 1; b < n; b++)
		if (st_write_byte(pc, (daddr_t)b, 0xC1) != 0)
			ok = 0;
	memset(blk, 0xC2, sizeof(blk));
	if (st_install(pc, 1000, blk) != 0)
		ok = 0;
	st_wb_answer = ST_EIO;
	if (st_write_byte(pc, 2000, 0xC3) != 0 || st_entry(pc, 1000) != 0 ||
	    !st_holds(pc, 2000, 0xC3, 1))
		ok = 0;
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/*
 * P3: a write lands, copied and dirty, whether the block was cached or not;
 * and an install of a block already written leaves the write alone.
 */
static int
st_write_lands(void)
{
	struct page_cache	*pc = page_cache_create(4, ST_BLOCK,
							st_writeback, 0);
	unsigned char		 blk[ST_BLOCK];
	int			 ok = 1;

	if (pc == 0)
		return 0;
	memset(blk, 0x11, sizeof(blk));
	if (st_install(pc, 1, blk) != 0)
		ok = 0;
	memset(blk, 0x22, sizeof(blk));
	if (page_cache_write(pc, 1, (vm_offset_t)blk, sizeof(blk)) != 0 ||
	    !st_holds(pc, 1, 0x22, 1))
		ok = 0;
	memset(blk, 0x33, sizeof(blk));
	if (page_cache_write(pc, 2, (vm_offset_t)blk, sizeof(blk)) != 0)
		ok = 0;
	memset(blk, 0x44, sizeof(blk));
	if (st_install(pc, 2, blk) != PAGE_CACHE_PRESENT ||
	    !st_holds(pc, 2, 0x33, 1))
		ok = 0;
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/*
 * P4: with every slot being written back (busy), a write of another block
 * is refused -- and the blocks being written are exactly as they were.
 */
static int
st_write_refused_when_full(void)
{
	struct page_cache	*pc = page_cache_create(2, ST_BLOCK,
							st_writeback, 0);
	unsigned char		 blk[ST_BLOCK];
	int			 ok = 1, rc;

	if (pc == 0)
		return 0;
	memset(blk, 0x55, sizeof(blk));
	if (page_cache_write(pc, 1, (vm_offset_t)blk, sizeof(blk)) != 0 ||
	    page_cache_write(pc, 2, (vm_offset_t)blk, sizeof(blk)) != 0)
		ok = 0;
	pc->pc_pool[0].pc_busy = 1;
	pc->pc_pool[1].pc_busy = 1;
	memset(blk, 0x66, sizeof(blk));
	rc = page_cache_write(pc, 3, (vm_offset_t)blk, sizeof(blk));
	if (rc == 0 || st_entry(pc, 3) != 0 ||
	    !st_holds(pc, 1, 0x55, 1) || !st_holds(pc, 2, 0x55, 1))
		ok = 0;
	pc->pc_pool[0].pc_busy = 0;
	pc->pc_pool[1].pc_busy = 0;
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/* P5: a write that is not a whole block is refused, and changes nothing. */
static int
st_write_refuses_a_part(void)
{
	struct page_cache	*pc = page_cache_create(2, ST_BLOCK,
							st_writeback, 0);
	unsigned char		 blk[ST_BLOCK];
	int			 ok = 1;

	if (pc == 0)
		return 0;
	memset(blk, 0x77, sizeof(blk));
	if (page_cache_write(pc, 1, (vm_offset_t)blk, sizeof(blk)) != 0)
		ok = 0;
	memset(blk, 0x88, sizeof(blk));
	if (page_cache_write(pc, 1, (vm_offset_t)blk, ST_BLOCK / 2) !=
	    KERN_INVALID_ARGUMENT || !st_holds(pc, 1, 0x77, 1))
		ok = 0;
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/*
 * The fill double: waits at the gate for its block, counts the calls, and
 * answers st_fill_answer, or fills the slot with st_fill_byte.
 */
static unsigned int	st_fill_calls;
static int		st_fill_answer;
static unsigned char	st_fill_byte = 0xF0;

static int
st_fill(void *ctx, daddr_t block, vm_offset_t data, vm_size_t size,
	vm_offset_t phys)
{
	(void)ctx;
	(void)phys;
	st_gate_wait(block);
	st_fill_calls++;
	if (st_fill_answer != 0)
		return st_fill_answer;
	memset((void *)data, st_fill_byte, size);
	return 0;
}

/* A get on a thread of its own. */
struct st_getter {
	struct page_cache	*pc;
	daddr_t			 block;
	struct page_cache_entry	*e;
	int			 rc;
	pthread_t		 th;
};

static void *
st_get_thread(void *arg)
{
	struct st_getter *g = (struct st_getter *)arg;

	g->rc = page_cache_get(g->pc, g->block, st_fill, 0, &g->e);
	return 0;
}

static int
st_get_start(struct st_getter *g, struct page_cache *pc, daddr_t block)
{
	memset(g, 0, sizeof(*g));
	g->pc = pc;
	g->block = block;
	return pthread_create(&g->th, 0, st_get_thread, g) == 0;
}

/* Until `n' threads wait on the cache's condition: 1 ms at a time, 5 s. */
static int
st_wait_waiters(struct page_cache *pc, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < 5000 && *(volatile unsigned int *)&pc->pc_nwaiters < n;
	     i++)
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 1);
	return *(volatile unsigned int *)&pc->pc_nwaiters >= n;
}

static int
st_bytes(const struct page_cache_entry *e, unsigned char v)
{
	unsigned int i;

	for (i = 0; i < ST_BLOCK; i++)
		if (((const unsigned char *)e->pc_data)[i] != v)
			return 0;
	return 1;
}

/* P12: a failed fill leaves nothing cached, and the next get reads again. */
static int
st_failed_fill_leaves_nothing(void)
{
	struct page_cache	*pc = page_cache_create(4, ST_BLOCK,
							st_writeback, 0);
	struct page_cache_entry	*e = 0;
	unsigned int		 calls = st_fill_calls;
	int			 ok = 1;

	if (pc == 0)
		return 0;
	st_fill_answer = ST_EIO;
	if (page_cache_get(pc, 7, st_fill, 0, &e) != ST_EIO || e != 0 ||
	    page_cache_contains(pc, 7))
		ok = 0;
	st_fill_answer = 0;
	if (page_cache_get(pc, 7, st_fill, 0, &e) != 0 || e == 0 ||
	    !st_bytes(e, st_fill_byte) || st_fill_calls - calls != 2)
		ok = 0;
	if (e != 0)
		page_cache_put(pc, e);
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/*
 * P13: a get of a block being read waits for that read and takes its bytes;
 * its own fill is never called.
 */
static int
st_get_waits_for_the_fill(void)
{
	struct page_cache	*pc = page_cache_create(4, ST_BLOCK,
							st_writeback, 0);
	struct st_getter	 g1, g2;
	unsigned int		 calls = st_fill_calls;
	int			 ok = 1;

	if (pc == 0)
		return 0;
	st_gate_reset(8);
	if (!st_get_start(&g1, pc, 8)) {
		st_gate_reset((daddr_t)-1);
		(void) st_done(pc);
		return 0;
	}
	if (!st_wait_for(&st_gate_entered) || !st_get_start(&g2, pc, 8) ||
	    !st_wait_waiters(pc, 1))
		ok = 0;
	st_gate_release();
	(void) pthread_join(g1.th, 0);
	(void) pthread_join(g2.th, 0);
	st_gate_reset((daddr_t)-1);
	if (g1.rc != 0 || g2.rc != 0 || g1.e == 0 || g2.e != g1.e ||
	    !st_bytes(g1.e, st_fill_byte) || st_fill_calls - calls != 1)
		ok = 0;
	if (g1.e != 0)
		page_cache_put(pc, g1.e);
	if (g2.e != 0)
		page_cache_put(pc, g2.e);
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/*
 * P14: neither a pinned entry nor one being read is ever a victim: with both
 * slots so held, a get of a third block answers 0 with no entry, and its
 * fill is not called.
 */
static int
st_held_entries_are_not_victims(void)
{
	struct page_cache	*pc = page_cache_create(2, ST_BLOCK,
							st_writeback, 0);
	struct page_cache_entry	*e1 = 0, *e3 = (struct page_cache_entry *)1;
	struct st_getter	 g2;
	unsigned int		 calls;
	int			 ok = 1, rc3;

	if (pc == 0)
		return 0;
	if (page_cache_get(pc, 1, st_fill, 0, &e1) != 0 || e1 == 0)
		ok = 0;
	st_gate_reset(2);
	if (!st_get_start(&g2, pc, 2)) {
		st_gate_reset((daddr_t)-1);
		if (e1 != 0)
			page_cache_put(pc, e1);
		(void) st_done(pc);
		return 0;
	}
	if (!st_wait_for(&st_gate_entered))
		ok = 0;
	calls = st_fill_calls;
	rc3 = page_cache_get(pc, 3, st_fill, 0, &e3);
	if (rc3 != 0 || e3 != 0 || st_fill_calls != calls ||
	    !page_cache_contains(pc, 1) || !page_cache_contains(pc, 2))
		ok = 0;
	st_gate_release();
	(void) pthread_join(g2.th, 0);
	st_gate_reset((daddr_t)-1);
	if (e1 != 0)
		page_cache_put(pc, e1);
	if (g2.e != 0)
		page_cache_put(pc, g2.e);
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/* P15: a get of a cached block never reads it, so dirty bytes survive. */
static int
st_hit_never_fills(void)
{
	struct page_cache	*pc = page_cache_create(4, ST_BLOCK,
							st_writeback, 0);
	struct page_cache_entry	*e = 0;
	unsigned int		 calls;
	int			 ok = 1;

	if (pc == 0)
		return 0;
	if (st_write_byte(pc, 9, 0x99) != 0)
		ok = 0;
	calls = st_fill_calls;
	if (page_cache_get(pc, 9, st_fill, 0, &e) != 0 || e == 0 ||
	    !st_bytes(e, 0x99) || !e->pc_dirty || st_fill_calls != calls)
		ok = 0;
	if (e != 0)
		page_cache_put(pc, e);
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/* P16: a get counts one miss or one hit; contains counts nothing. */
static int
st_counts_are_one(void)
{
	struct page_cache	*pc = page_cache_create(4, ST_BLOCK,
							st_writeback, 0);
	struct page_cache_entry	*e = 0;
	unsigned int		 h, m;
	int			 ok = 1;

	if (pc == 0)
		return 0;
	h = pc->pc_hits;
	m = pc->pc_misses;
	if (page_cache_get(pc, 10, st_fill, 0, &e) != 0 || e == 0 ||
	    pc->pc_misses != m + 1 || pc->pc_hits != h)
		ok = 0;
	if (e != 0)
		page_cache_put(pc, e);
	e = 0;
	if (page_cache_get(pc, 10, st_fill, 0, &e) != 0 || e == 0 ||
	    pc->pc_hits != h + 1 || pc->pc_misses != m + 1)
		ok = 0;
	if (e != 0)
		page_cache_put(pc, e);
	(void) page_cache_contains(pc, 10);
	(void) page_cache_contains(pc, 11);
	if (pc->pc_hits != h + 1 || pc->pc_misses != m + 1)
		ok = 0;
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/*
 * P17: a write of a block being read lands after the read: the block ends
 * with the write's bytes, dirty -- not the disk's, over them.
 */
struct st_wr {
	struct page_cache	*pc;
	int			 rc;
	pthread_t		 th;
};

static void *
st_wr_thread(void *arg)
{
	struct st_wr *w = (struct st_wr *)arg;

	w->rc = st_write_byte(w->pc, 12, 0x17);
	return 0;
}

static int
st_write_lands_after_fill(void)
{
	struct page_cache	*pc = page_cache_create(4, ST_BLOCK,
							st_writeback, 0);
	struct st_getter	 g;
	struct st_wr		 w;
	int			 ok = 1;

	if (pc == 0)
		return 0;
	memset(&w, 0, sizeof(w));
	w.pc = pc;
	st_gate_reset(12);
	if (!st_get_start(&g, pc, 12)) {
		st_gate_reset((daddr_t)-1);
		(void) st_done(pc);
		return 0;
	}
	if (!st_wait_for(&st_gate_entered) ||
	    pthread_create(&w.th, 0, st_wr_thread, &w) != 0 ||
	    !st_wait_waiters(pc, 1))
		ok = 0;
	st_gate_release();
	(void) pthread_join(g.th, 0);
	(void) pthread_join(w.th, 0);
	st_gate_reset((daddr_t)-1);
	if (g.rc != 0 || w.rc != 0 || !st_holds(pc, 12, 0x17, 1))
		ok = 0;
	if (g.e != 0)
		page_cache_put(pc, g.e);
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/* A sync on a thread of its own; its answer, and when it is done. */
struct st_syncer {
	struct page_cache	*pc;
	volatile int		 started, done;
	int			 failures;
	pthread_t		 th;
};

static void *
st_sync_thread(void *arg)
{
	struct st_syncer *sy = (struct st_syncer *)arg;

	sy->started = 1;
	sy->failures = page_cache_sync(sy->pc);
	sy->done = 1;
	return 0;
}

static int
st_sync_start(struct st_syncer *sy, struct page_cache *pc)
{
	memset(sy, 0, sizeof(*sy));
	sy->pc = pc;
	return pthread_create(&sy->th, 0, st_sync_thread, sy) == 0;
}

/*
 * P9: a write that lands while its block is being written back keeps the
 * block dirty, and the next sync writes the new bytes.
 */
static int
st_write_during_writeback_stays_dirty(void)
{
	struct page_cache	*pc = page_cache_create(4, ST_BLOCK,
							st_writeback, 0);
	struct st_syncer	 sy;
	int			 ok = 1;

	if (pc == 0)
		return 0;
	if (st_write_byte(pc, 10, 0x91) != 0)
		ok = 0;
	st_gate_reset(10);
	if (!st_sync_start(&sy, pc)) {
		st_gate_reset((daddr_t)-1);
		return 0;
	}
	if (!st_wait_for(&st_gate_entered))
		ok = 0;
	if (st_write_byte(pc, 10, 0x92) != 0)
		ok = 0;
	st_gate_release();
	(void) pthread_join(sy.th, 0);
	st_gate_reset((daddr_t)-1);
	if (!st_holds(pc, 10, 0x92, 1))
		ok = 0;
	st_wb_last_byte = 0;
	(void) page_cache_sync(pc);
	if (!st_holds(pc, 10, 0x92, 0) || st_wb_last_byte != 0x92)
		ok = 0;
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/*
 * P10: one sync at a time.  A sync started while another's write of a block
 * is failing waits for it, then tries the block itself and says it failed
 * (>= 1).  Two at once, the second skipped the busy block and said 0.
 */
static int
st_one_sync_at_a_time(void)
{
	struct page_cache	*pc = page_cache_create(4, ST_BLOCK,
							st_writeback, 0);
	struct st_syncer	 a, b;
	int			 ok = 1;

	if (pc == 0)
		return 0;
	if (st_write_byte(pc, 30, 0xA0) != 0)
		ok = 0;
	st_wb_fail_block = 30;
	st_gate_reset(30);
	if (!st_sync_start(&a, pc)) {
		st_gate_reset((daddr_t)-1);
		st_wb_fail_block = (daddr_t)-1;
		return 0;
	}
	if (!st_wait_for(&st_gate_entered) || !st_sync_start(&b, pc))
		ok = 0;
	(void) st_wait_for(&b.started);
	(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 20);
	st_gate_release();
	(void) pthread_join(a.th, 0);
	(void) pthread_join(b.th, 0);
	st_gate_reset((daddr_t)-1);
	if (a.failures < 1 || b.failures < 1)
		ok = 0;
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/*
 * P11: a sync takes only what was dirty when it began -- which is what makes
 * it end while blocks keep being dirtied behind it.  Deterministic, on the
 * gate: the sync collects three dirty blocks and is held in the middle of
 * writing the first; ten new blocks are dirtied; the gate opens.  When the
 * sync returns, the three are clean and the ten are still dirty.  A sync
 * that took every dirty block it found would have written the ten too, and
 * against a writer that keeps up it would never have returned.
 */
static int
st_sync_takes_what_was_dirty(void)
{
	struct page_cache	*pc = page_cache_create(32, ST_BLOCK,
							st_writeback, 0);
	struct st_syncer	 sy;
	unsigned int		 b;
	int			 ok = 1;

	if (pc == 0)
		return 0;
	for (b = 1; b <= 5; b += 2)
		if (st_write_byte(pc, (daddr_t)b, 0xB1) != 0)
			ok = 0;
	st_gate_reset(1);
	if (!st_sync_start(&sy, pc)) {
		st_gate_reset((daddr_t)-1);
		(void) st_done(pc);
		return 0;
	}
	if (!st_wait_for(&st_gate_entered))
		ok = 0;
	for (b = 101; b <= 119; b += 2)
		if (st_write_byte(pc, (daddr_t)b, 0xB2) != 0)
			ok = 0;
	st_gate_release();
	(void) pthread_join(sy.th, 0);
	st_gate_reset((daddr_t)-1);
	for (b = 1; b <= 5; b += 2)
		if (!st_holds(pc, (daddr_t)b, 0xB1, 0))
			ok = 0;
	for (b = 101; b <= 119; b += 2)
		if (!st_holds(pc, (daddr_t)b, 0xB2, 1))
			ok = 0;
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/* An install on a thread of its own, with a ticket taken there. */
struct st_installer {
	struct page_cache	*pc;
	daddr_t			 block;
	int			 rc;
	volatile int		 done;
	pthread_t		 th;
};

static void *
st_install_thread(void *arg)
{
	struct st_installer *in = (struct st_installer *)arg;
	unsigned char blk[ST_BLOCK];

	memset(blk, 0xE7, sizeof(blk));
	in->rc = st_install(in->pc, in->block, blk);
	in->done = 1;
	return 0;
}

/*
 * P18: readahead publishes only bytes the disk cannot have changed since its
 * ticket.  A ticket; then a block written, synced clean and evicted: an
 * install of it with that ticket is STALE and leaves it absent, one with a
 * fresh ticket caches it.  An install never touches a dirty block (PRESENT,
 * the write's bytes kept) nor one being read: PRESENT at once, while the
 * fill is held at the gate, and the fill's bytes are what the get sees.
 */
static int
st_install_takes_no_stale_bytes(void)
{
	struct page_cache	*pc = page_cache_create(2, ST_BLOCK,
							st_writeback, 0);
	struct st_getter	 g;
	struct st_installer	 in;
	unsigned char		 blk[ST_BLOCK];
	uint64_t		 t;
	int			 ok = 1, started = 0;

	if (pc == 0)
		return 0;
	t = page_cache_ticket(pc);
	st_wb_answer = 0;
	if (st_write_byte(pc, 5, 0xD1) != 0 || page_cache_sync(pc) != 0 ||
	    !st_holds(pc, 5, 0xD1, 0))
		ok = 0;
	memset(blk, 0xD2, sizeof(blk));
	if (st_install(pc, 6, blk) != 0 || st_install(pc, 8, blk) != 0 ||
	    page_cache_contains(pc, 5))
		ok = 0;			/* 5, the oldest, was evicted */
	memset(blk, 0xD3, sizeof(blk));
	if (page_cache_install(pc, 5, (vm_offset_t)blk, ST_BLOCK, t) !=
	    PAGE_CACHE_STALE || page_cache_contains(pc, 5))
		ok = 0;
	if (st_install(pc, 5, blk) != 0 || !st_holds(pc, 5, 0xD3, 0))
		ok = 0;
	if (st_write_byte(pc, 5, 0xD4) != 0)
		ok = 0;
	memset(blk, 0xD5, sizeof(blk));
	if (st_install(pc, 5, blk) != PAGE_CACHE_PRESENT ||
	    !st_holds(pc, 5, 0xD4, 1))
		ok = 0;
	if (!st_done(pc))
		ok = 0;

	pc = page_cache_create(4, ST_BLOCK, st_writeback, 0);
	if (pc == 0)
		return 0;
	st_gate_reset(9);
	if (!st_get_start(&g, pc, 9)) {
		st_gate_reset((daddr_t)-1);
		(void) st_done(pc);
		return 0;
	}
	memset(&in, 0, sizeof(in));
	in.pc = pc;
	in.block = 9;
	if (st_wait_for(&st_gate_entered) &&
	    pthread_create(&in.th, 0, st_install_thread, &in) == 0)
		started = 1;
	if (!started || !st_wait_for(&in.done) ||
	    in.rc != PAGE_CACHE_PRESENT)
		ok = 0;			/* it waited for the fill, or took it */
	st_gate_release();
	(void) pthread_join(g.th, 0);
	if (started)
		(void) pthread_join(in.th, 0);
	st_gate_reset((daddr_t)-1);
	if (g.rc != 0 || g.e == 0 || !st_bytes(g.e, st_fill_byte))
		ok = 0;
	if (g.e != 0)
		page_cache_put(pc, g.e);
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/* A discard on a thread of its own, and when it is done. */
struct st_discarder {
	struct page_cache	*pc;
	daddr_t			 block;
	volatile int		 done;
	pthread_t		 th;
};

static void *
st_discard_thread(void *arg)
{
	struct st_discarder *d = (struct st_discarder *)arg;

	page_cache_discard(d->pc, d->block);
	d->done = 1;
	return 0;
}

/*
 * P19: a discarded block leaves the cache and is never written.  A dirty
 * block discarded is gone, a sync writes nothing, and a ticket taken before
 * the discard is stale after it.  A pinned block discarded loses its key and
 * keeps its bytes for its holder, and its slot is freed at the put.  A
 * discard of a block being written back waits for the write to land.
 */
static int
st_discard_leaves_nothing(void)
{
	struct page_cache	*pc = page_cache_create(4, ST_BLOCK,
							st_writeback, 0);
	struct page_cache_entry	*e = 0;
	struct st_syncer	 sy;
	struct st_discarder	 d;
	unsigned char		 blk[ST_BLOCK];
	unsigned int		 calls;
	uint64_t		 t;
	int			 ok = 1, started = 0;

	if (pc == 0)
		return 0;
	st_wb_answer = 0;
	t = page_cache_ticket(pc);
	if (st_write_byte(pc, 5, 0xB1) != 0)
		ok = 0;
	page_cache_discard(pc, 5);
	calls = st_wb_calls;
	if (page_cache_contains(pc, 5) || page_cache_sync(pc) != 0 ||
	    st_wb_calls != calls)
		ok = 0;
	memset(blk, 0xB2, sizeof(blk));
	if (page_cache_install(pc, 5, (vm_offset_t)blk, ST_BLOCK, t) !=
	    PAGE_CACHE_STALE)
		ok = 0;

	st_fill_answer = 0;
	if (page_cache_get(pc, 6, st_fill, 0, &e) != 0 || e == 0) {
		ok = 0;
	} else {
		page_cache_discard(pc, 6);
		if (page_cache_contains(pc, 6) || e->pc_state != PC_ORPHAN ||
		    !st_bytes(e, st_fill_byte))
			ok = 0;
		page_cache_put(pc, e);
		if (e->pc_state != PC_FREE)
			ok = 0;
	}

	if (st_write_byte(pc, 7, 0xB3) != 0)
		ok = 0;
	st_gate_reset(7);
	if (!st_sync_start(&sy, pc)) {
		st_gate_reset((daddr_t)-1);
		(void) st_done(pc);
		return 0;
	}
	memset(&d, 0, sizeof(d));
	d.pc = pc;
	d.block = 7;
	if (st_wait_for(&st_gate_entered) &&
	    pthread_create(&d.th, 0, st_discard_thread, &d) == 0)
		started = 1;
	if (!started || !st_wait_waiters(pc, 1) || d.done)
		ok = 0;			/* it did not wait for the write */
	st_gate_release();
	(void) pthread_join(sy.th, 0);
	if (started)
		(void) pthread_join(d.th, 0);
	st_gate_reset((daddr_t)-1);
	if (!d.done || page_cache_contains(pc, 7) || st_wb_last_block != 7)
		ok = 0;
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/* A wrote on a thread of its own, and when it is done. */
struct st_wroter {
	struct page_cache	*pc;
	daddr_t			 block;
	int			 rc;
	volatile int		 done;
	pthread_t		 th;
};

static void *
st_wrote_thread(void *arg)
{
	struct st_wroter *w = (struct st_wroter *)arg;
	unsigned char blk[ST_BLOCK];

	memset(blk, 0xA9, sizeof(blk));
	w->rc = page_cache_wrote(w->pc, w->block, (vm_offset_t)blk, ST_BLOCK);
	w->done = 1;
	return 0;
}

/*
 * P20: page_cache_wrote gives the cache what the disk now has.  A clean
 * cached block takes the bytes and stays clean, a dirty one takes them and
 * stays dirty, a part of a block is refused and changes nothing, and an
 * absent block stays absent -- with a ticket taken before now stale.  A
 * wrote of a block being read waits for the read, and its bytes are the
 * ones left.
 */
static int
st_wrote_follows_the_disk(void)
{
	struct page_cache	*pc = page_cache_create(4, ST_BLOCK,
							st_writeback, 0);
	struct st_getter	 g;
	struct st_wroter	 w;
	unsigned char		 blk[ST_BLOCK];
	uint64_t		 t;
	int			 ok = 1, started = 0;

	if (pc == 0)
		return 0;
	memset(blk, 0x91, sizeof(blk));
	if (st_install(pc, 5, blk) != 0)
		ok = 0;
	memset(blk, 0x92, sizeof(blk));
	if (page_cache_wrote(pc, 5, (vm_offset_t)blk, ST_BLOCK) != 0 ||
	    !st_holds(pc, 5, 0x92, 0))
		ok = 0;
	if (st_write_byte(pc, 6, 0x93) != 0)
		ok = 0;
	memset(blk, 0x94, sizeof(blk));
	if (page_cache_wrote(pc, 6, (vm_offset_t)blk, ST_BLOCK) != 0 ||
	    !st_holds(pc, 6, 0x94, 1))
		ok = 0;
	memset(blk, 0x95, sizeof(blk));
	if (page_cache_wrote(pc, 5, (vm_offset_t)blk, ST_BLOCK / 2) !=
	    KERN_INVALID_ARGUMENT || !st_holds(pc, 5, 0x92, 0))
		ok = 0;
	t = page_cache_ticket(pc);
	if (page_cache_wrote(pc, 9, (vm_offset_t)blk, ST_BLOCK) != 0 ||
	    page_cache_contains(pc, 9) ||
	    page_cache_install(pc, 9, (vm_offset_t)blk, ST_BLOCK, t) !=
	    PAGE_CACHE_STALE)
		ok = 0;

	st_fill_answer = 0;
	st_gate_reset(10);
	if (!st_get_start(&g, pc, 10)) {
		st_gate_reset((daddr_t)-1);
		(void) st_done(pc);
		return 0;
	}
	memset(&w, 0, sizeof(w));
	w.pc = pc;
	w.block = 10;
	if (st_wait_for(&st_gate_entered) &&
	    pthread_create(&w.th, 0, st_wrote_thread, &w) == 0)
		started = 1;
	if (!started || !st_wait_waiters(pc, 1) || w.done)
		ok = 0;			/* it did not wait for the read */
	st_gate_release();
	(void) pthread_join(g.th, 0);
	if (started)
		(void) pthread_join(w.th, 0);
	st_gate_reset((daddr_t)-1);
	if (g.rc != 0 || g.e == 0 || w.rc != 0 || !st_holds(pc, 10, 0xA9, 0))
		ok = 0;
	if (g.e != 0)
		page_cache_put(pc, g.e);
	if (!st_done(pc))
		ok = 0;
	return ok;
}

/* Count a case, and mark it failed by its number. */
static void
st_case(unsigned int *ran, unsigned int *wrong, unsigned int *failed, int ok)
{
	(*ran)++;
	if (!ok) {
		(*wrong)++;
		*failed |= 1u << (*ran - 1);
	}
}

void
page_cache_selftest(unsigned int *ran, unsigned int *wrong,
		    unsigned int *failed)
{
	*ran = 0;
	*wrong = 0;
	*failed = 0;
	st_wb_calls = 0;
	st_wb_answer = 0;
	page_cache_quiet = 1;
	pthread_mutex_init(&st_gate_lock, 0);
	pthread_cond_init(&st_gate_cond, 0);

	st_case(ran, wrong, failed, st_slots_are_fixed());
	st_case(ran, wrong, failed, st_destroy_refuses_dirty());
	st_case(ran, wrong, failed, st_write_lands());
	st_case(ran, wrong, failed, st_write_refused_when_full());
	st_case(ran, wrong, failed, st_write_refuses_a_part());
	st_case(ran, wrong, failed, st_evict_keeps_unwritable());
	st_case(ran, wrong, failed, st_write_fails_when_nothing_writes());
	st_case(ran, wrong, failed, st_clean_found_past_the_window());
	st_case(ran, wrong, failed, st_write_during_writeback_stays_dirty());
	st_case(ran, wrong, failed, st_one_sync_at_a_time());
	st_case(ran, wrong, failed, st_sync_takes_what_was_dirty());
	st_case(ran, wrong, failed, st_failed_fill_leaves_nothing());
	st_case(ran, wrong, failed, st_get_waits_for_the_fill());
	st_case(ran, wrong, failed, st_held_entries_are_not_victims());
	st_case(ran, wrong, failed, st_hit_never_fills());
	st_case(ran, wrong, failed, st_counts_are_one());
	st_case(ran, wrong, failed, st_write_lands_after_fill());
	st_case(ran, wrong, failed, st_install_takes_no_stale_bytes());
	st_case(ran, wrong, failed, st_discard_leaves_nothing());
	st_case(ran, wrong, failed, st_wrote_follows_the_disk());
	page_cache_quiet = 0;
}
