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
		page_cache_insert(pc, (daddr_t)b, (vm_offset_t)blk, sizeof(blk));
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
	page_cache_insert(pc, 1000, (vm_offset_t)blk, sizeof(blk));
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
 * and an insert of a block already written leaves the write alone.
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
	page_cache_insert(pc, 1, (vm_offset_t)blk, sizeof(blk));
	memset(blk, 0x22, sizeof(blk));
	if (page_cache_write(pc, 1, (vm_offset_t)blk, sizeof(blk)) != 0 ||
	    !st_holds(pc, 1, 0x22, 1))
		ok = 0;
	memset(blk, 0x33, sizeof(blk));
	if (page_cache_write(pc, 2, (vm_offset_t)blk, sizeof(blk)) != 0)
		ok = 0;
	memset(blk, 0x44, sizeof(blk));
	page_cache_insert(pc, 2, (vm_offset_t)blk, sizeof(blk));
	if (!st_holds(pc, 2, 0x33, 1))
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
 * P11: a sync ends while blocks keep being dirtied behind it.  A writer
 * dirties new blocks without pause; the sync must still return within the
 * bounded wait -- it takes only what was dirty when it began.
 */
struct st_writer {
	struct page_cache	*pc;
	volatile int		 stop;
	pthread_t		 th;
};

static void *
st_writer_thread(void *arg)
{
	struct st_writer *w = (struct st_writer *)arg;
	daddr_t b = 5000;

	while (!w->stop)
		(void) st_write_byte(w->pc, b++, 0xB0);
	return 0;
}

static int
st_sync_ends_under_writes(void)
{
	struct page_cache	*pc = page_cache_create(16, ST_BLOCK,
							st_writeback, 0);
	struct st_syncer	 sy;
	struct st_writer	 w;
	int			 ok = 1, ended;

	if (pc == 0)
		return 0;
	memset(&w, 0, sizeof(w));
	w.pc = pc;
	if (pthread_create(&w.th, 0, st_writer_thread, &w) != 0) {
		(void) st_done(pc);
		return 0;
	}
	(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 5);
	if (!st_sync_start(&sy, pc))
		ok = 0;
	ended = st_wait_for(&sy.done);
	w.stop = 1;
	(void) pthread_join(w.th, 0);
	(void) pthread_join(sy.th, 0);
	if (!ended)
		ok = 0;
	if (!st_done(pc))
		ok = 0;
	return ok;
}

void
page_cache_selftest(unsigned int *ran, unsigned int *wrong)
{
	*ran = 0;
	*wrong = 0;
	st_wb_calls = 0;
	st_wb_answer = 0;
	page_cache_quiet = 1;
	pthread_mutex_init(&st_gate_lock, 0);
	pthread_cond_init(&st_gate_cond, 0);

	(*ran)++;
	if (!st_slots_are_fixed())
		(*wrong)++;
	(*ran)++;
	if (!st_destroy_refuses_dirty())
		(*wrong)++;
	(*ran)++;
	if (!st_write_lands())
		(*wrong)++;
	(*ran)++;
	if (!st_write_refused_when_full())
		(*wrong)++;
	(*ran)++;
	if (!st_write_refuses_a_part())
		(*wrong)++;
	(*ran)++;
	if (!st_evict_keeps_unwritable())
		(*wrong)++;
	(*ran)++;
	if (!st_write_fails_when_nothing_writes())
		(*wrong)++;
	(*ran)++;
	if (!st_clean_found_past_the_window())
		(*wrong)++;
	(*ran)++;
	if (!st_write_during_writeback_stays_dirty())
		(*wrong)++;
	(*ran)++;
	if (!st_one_sync_at_a_time())
		(*wrong)++;
	(*ran)++;
	if (!st_sync_ends_under_writes())
		(*wrong)++;
	page_cache_quiet = 0;
}
