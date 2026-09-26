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
#include <mach.h>
#include "page_cache.h"

#define ST_BLOCK	1024u

/* Writeback doubles: count the calls, and answer ok or a fixed error. */
static unsigned int	st_wb_calls;
static int		st_wb_answer;

static int
st_writeback(void *ctx, daddr_t block, vm_offset_t data, vm_size_t size,
	     vm_offset_t phys)
{
	(void)ctx;
	(void)block;
	(void)data;
	(void)size;
	(void)phys;
	st_wb_calls++;
	return st_wb_answer;
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
	page_cache_update(pc, 7, (vm_offset_t)blk, sizeof(blk));
	refused = page_cache_destroy(pc) != 0;
	if (!refused)
		return 0;		/* freed: the cache is gone */
	st_wb_answer = 0;
	(void) page_cache_sync(pc);
	destroyed = page_cache_destroy(pc) == 0;
	return destroyed;
}

void
page_cache_selftest(unsigned int *ran, unsigned int *wrong)
{
	*ran = 0;
	*wrong = 0;
	st_wb_calls = 0;
	st_wb_answer = 0;

	(*ran)++;
	if (!st_slots_are_fixed())
		(*wrong)++;
	(*ran)++;
	if (!st_destroy_refuses_dirty())
		(*wrong)++;
}
