/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The counters checked against loads whose counts are known (#804).
 *
 * A counter programmed with the wrong event, or the wrong unit mask, counts
 * something: a plausible number about another thing.  So, once a boot that
 * has pmc=N, on the boot processor, each counter is made to count a load
 * whose answer is known before it runs, and the boot says whether it did.
 *
 * The load is a walk through a buffer in a random order, each step's address
 * loaded from the slot before -- a chain no prefetcher can run ahead of, and
 * one access per step:
 *
 *   - lines 64 bytes apart in 16 KiB, which the L1D holds (32 KiB on the
 *     6th-generation core): after a pass to warm it, nothing misses;
 *   - in 128 KiB, four times the L1D and half the L2 (256 KiB): every step
 *     misses the L1D and hits the L2 -- the walk that tells a counter of L2
 *     misses from one of L2 requests, which every other walk here makes
 *     equal;
 *   - in 2 MiB, eight times the L2 (256 KiB) and a third of the last level
 *     (6 MiB on omen): after the warm pass nearly every step misses the L1D
 *     and the L2, and hits the last level;
 *   - in 64 MiB, more than the last level of any machine here: every step
 *     misses the L2, and most miss the last level -- what it keeps of the
 *     buffer is a few in a hundred;
 *   - one slot a page, in 16 KiB and in 64 MiB: walking 16384 pages in a
 *     random order needs a page walk nearly every step, more pages than the
 *     second-level TLB maps (1536); four pages need none.  Once storing into
 *     each slot before loading from it, which makes the walks the stores'.
 *     One line a page is 1 MiB of lines, which the last level holds: these
 *     walks say nothing of its misses.
 *
 * Each walk runs in chunks with interrupts off, so that every chunk is read
 * on one processor (RDPMC reads the processor it runs on), and its counts
 * are said beside the rule each must meet.  The rules are loose -- a fraction
 * of the steps, not an exact count -- because a prefetcher, the page-table
 * walker's own loads and the warm pass are not nothing; a wrong event is not
 * off by a fraction but by everything.  ⚠️ Not checked: a load from another
 * core's modified line, which needs a second core writing, and an instruction
 * TLB walk, which needs code spread over pages: each is said to be unchecked.
 *
 * 🔴 ASKED FOR, AND ITS MEMORY KEPT.  It runs with pmc_test=1 only, not on
 * every boot that counts: it takes a second and 68 MiB, which a measurement
 * boot should not pay.  And it gives the 68 MiB back to no one.  Freeing
 * them unwires 17408 pages with a cross-call each, and before user mode has
 * begun those thousands of IPIs, each waking the idle boot processor, read
 * to the quiet census as thousands of idle passes: on omen it took the boot
 * for over, six boots of six, and stopped the machine under the free's
 * cross-call in three.  Both are defects of their own -- the census's clock
 * is #805, the cross-call a page #806 -- and here a test boot keeps its
 * 68 MiB.
 *
 * The rules' ceilings are loose too: on the walks that miss, a hardware
 * prefetcher fetches the next line with each one, and omen counted two to
 * four L2 and last-level events a step in 64 MiB.
 */

#include <stdint.h>

#include <kern/misc_protos.h>		/* printf, sprintf */
#include <vm/vm_kern.h>			/* kmem_alloc, kernel_map */

#include <cpu/pmc.h>
#include <cpu/regs.h>			/* interrupts_disable() */

#define	PMC_LINE		64
/*
 * The processor's base page, which one TLB entry maps: 4 KiB on x86-64
 * whatever the VM's page_size, which here is a variable and not a constant.
 */
#define	PMC_PAGE		4096
#define	PMC_CHUNK		4096		/* steps with interrupts off at a time */
#define	PMC_BIG			(64 * 1024 * 1024)
#define	PMC_BIG_STEPS		262144

/* The walks, and how many steps each counts. */
#define	W_L1D		0	/* lines in 16 KiB */
#define	W_L2HIT		1	/* lines in 128 KiB */
#define	W_L2		2	/* lines in 2 MiB */
#define	W_BIG		3	/* lines in 64 MiB */
#define	W_PAGES_FEW	4	/* a slot a page, 4 pages */
#define	W_PAGES		5	/* a slot a page, 16384 pages */
#define	W_PAGES_STORE	6	/* the same, storing first */
#define	W_COUNT		7

static const struct pmc_walk {
	const char	*what;
	uint32_t	size, stride, warm, steps, store;
} pmc_walks[W_COUNT] = {
	{ "lines in 16 KiB",		16 * 1024,	PMC_LINE,  1,  4096,		0 },
	{ "lines in 128 KiB",		128 * 1024,	PMC_LINE,  1,  8192,		0 },
	{ "lines in 2 MiB",		2 * 1024 * 1024, PMC_LINE, 1,  32768,		0 },
	{ "lines in 64 MiB",		PMC_BIG,	PMC_LINE,  0,  PMC_BIG_STEPS,	0 },
	{ "pages, 4",			4 * PMC_PAGE,	PMC_PAGE,  1,  4096,		0 },
	{ "pages, 16384",		PMC_BIG,	PMC_PAGE,  0,  16384,		0 },
	{ "pages, 16384, storing",	PMC_BIG,	PMC_PAGE,  0,  16384,		1 },
};

/*
 * What a role must count on a walk, as a fraction of its steps in percent:
 * at least `min', at most `max' (0 for no ceiling); -1 for a walk that says
 * nothing about it.  The fixed counters are checked on every walk: at least
 * one instruction and one cycle a step.  Branches at a fifth of the steps at
 * least: one a step as the loop is written, fewer if the compiler unrolls it.
 */
struct pmc_rule {
	int	min, max;
};

static const struct pmc_rule pmc_rules[PMC_ROLE_COUNT][W_COUNT] = {
	/* lines 16K  lines 128K   lines 2M     lines 64M    pages 4     pages 16K   pages, storing */
	[PMC_ROLE_INST] =
	{ { 100, 0 }, { 100, 0 },  { 100, 0 },  { 100, 0 },  { 100, 0 }, { 100, 0 }, { 100, 0 } },
	[PMC_ROLE_CYCLES] =
	{ { 100, 0 }, { 100, 0 },  { 100, 0 },  { 100, 0 },  { 100, 0 }, { 100, 0 }, { 100, 0 } },
	[PMC_ROLE_L1D_MISS] =
	{ { 0, 5 },   { 80, 300 }, { 80, 600 }, { 80, 600 }, { 0, 5 },   { -1, 0 },  { -1, 0 } },
	[PMC_ROLE_L2_MISS] =
	{ { 0, 5 },   { 0, 5 },    { 80, 600 }, { 80, 600 }, { 0, 5 },   { -1, 0 },  { -1, 0 } },
	[PMC_ROLE_LLC_MISS] =
	{ { 0, 5 },   { 0, 5 },    { 0, 20 },   { 50, 600 }, { 0, 5 },   { -1, 0 },  { -1, 0 } },
	[PMC_ROLE_HITM] =
	{ { -1, 0 },  { -1, 0 },   { -1, 0 },   { -1, 0 },   { -1, 0 },  { -1, 0 },  { -1, 0 } },
	[PMC_ROLE_DTLB_LOAD_WALK] =
	{ { 0, 5 },   { 0, 5 },    { -1, 0 },   { -1, 0 },   { 0, 5 },   { 50, 150 },{ -1, 0 } },
	[PMC_ROLE_DTLB_STORE_WALK] =
	{ { 0, 5 },   { 0, 5 },    { -1, 0 },   { -1, 0 },   { 0, 5 },   { 0, 5 },   { 50, 150 } },
	[PMC_ROLE_ITLB_WALK] =
	{ { -1, 0 },  { -1, 0 },   { -1, 0 },   { -1, 0 },   { -1, 0 },  { -1, 0 },  { -1, 0 } },
	[PMC_ROLE_LLC_REF] =
	{ { -1, 0 },  { 0, 5 },    { 80, 600 }, { 80, 600 }, { -1, 0 },  { -1, 0 },  { -1, 0 } },
	[PMC_ROLE_BRANCH] =
	{ { 20, 300 }, { 20, 300 }, { 20, 300 }, { 20, 300 }, { 20, 300 }, { 20, 300 }, { 20, 300 } },
	[PMC_ROLE_BRANCH_MISS] =
	{ { 0, 5 },   { 0, 5 },    { 0, 5 },    { 0, 5 },    { 0, 5 },   { 0, 5 },   { 0, 5 } },
	/*
	 * AMD's.  Its data page walks are loads' and stores' together, so the
	 * walk that stores counts as many as the one that loads.  2 MiB of
	 * lines is 512 pages, more than the L1 DTLB maps (64 on Zen 2 and Zen
	 * 3) and fewer than the L2 TLB (2048): it misses the one and hits the
	 * other.  And a line filled from the L3 or another core's cache is what
	 * every step of the 2 MiB walk is, and no step of the 128 KiB one.
	 */
	[PMC_ROLE_DTLB_WALK] =
	{ { 0, 5 },   { 0, 5 },    { 0, 5 },    { -1, 0 },   { 0, 5 },   { 50, 150 },{ 50, 150 } },
	[PMC_ROLE_DTLB_L2_HIT] =
	{ { 0, 5 },   { 0, 5 },    { 50, 150 }, { -1, 0 },   { 0, 5 },   { -1, 0 },  { -1, 0 } },
	[PMC_ROLE_L3_OR_PEER] =
	{ { 0, 5 },   { 0, 5 },    { 80, 300 }, { -1, 0 },   { 0, 5 },   { -1, 0 },  { -1, 0 } },
};

static const char *const pmc_role_short[PMC_ROLE_COUNT] = {
	"?", "inst", "cyc", "L1D", "L2", "LLC", "HITM", "dtlb-ld", "dtlb-st",
	"itlb", "LLC-ref", "br", "br-miss", "dtlb-walk", "dtlb-l2hit", "L3/peer",
};

static uint64_t		pmc_seed = 0x8040201008040201ULL;
static volatile uint64_t pmc_sink;	/* where each walk ends, so it is not optimised away */

static uint64_t
pmc_rand(void)
{
	pmc_seed ^= pmc_seed << 13;
	pmc_seed ^= pmc_seed >> 7;
	pmc_seed ^= pmc_seed << 17;
	return pmc_seed;
}

/* A cycle through the walk's slots in a random order: each slot's first word is the next slot's address. */
static uint64_t
pmc_cycle(vm_offset_t buf, uint32_t n, uint32_t stride, uint32_t *idx)
{
	uint32_t	i, j, t;

	for (i = 0; i < n; i++)
		idx[i] = i;
	for (i = n - 1; i > 0; i--) {
		j = (uint32_t) (pmc_rand() % (i + 1));
		t = idx[i];
		idx[i] = idx[j];
		idx[j] = t;
	}
	for (i = 0; i < n; i++)
		*(volatile uint64_t *) (buf + (vm_offset_t) idx[i] * stride) =
		    buf + (vm_offset_t) idx[(i + 1) % n] * stride;
	return buf + (vm_offset_t) idx[0] * stride;
}

/*
 * `steps' along the cycle from `p', in chunks with interrupts off, the
 * counters' advance added into d[]; storing into each slot first if asked.
 * Returns where it stopped.
 */
static uint64_t
pmc_follow(uint64_t p, uint32_t steps, int store, uint64_t *d, int n)
{
	uint64_t	v0[PMC_MAX], v1[PMC_MAX];
	uint32_t	done, k, todo;
	int		i, was;

	for (done = 0; done < steps; done += todo) {
		todo = steps - done < PMC_CHUNK ? steps - done : PMC_CHUNK;
		was = interrupts_enabled();
		interrupts_disable();
		if (d != 0)
			pmc_read(v0);
		if (store)
			for (k = 0; k < todo; k++) {
				((volatile uint64_t *) p)[1] = k;
				p = *(volatile uint64_t *) p;
			}
		else
			for (k = 0; k < todo; k++)
				p = *(volatile uint64_t *) p;
		if (d != 0)
			pmc_read(v1);
		if (was)
			interrupts_enable();
		if (d != 0)
			for (i = 0; i < n; i++)
				d[i] += v1[i] - v0[i];
	}
	return p;
}

/*
 * The median of three, each counter's on its own.  In QEMU a run whose walk
 * the host disturbed -- another of its tasks on the core, emptying the L2
 * between two chunks -- counted 16% of a walk's steps as missing the L2 that
 * holds it, in one boot of three; on metal and in the other two, nothing.
 */
static uint64_t
pmc_median3(uint64_t a, uint64_t b, uint64_t c)
{
	if (a > b) { uint64_t t = a; a = b; b = t; }
	if (b > c) { uint64_t t = b; b = c; c = t; }
	return a > b ? a : b;
}

/* Each line built here and printed whole: one printf cannot be cut by another thread's (#757). */
static char	pmc_test_line[512];

void
pmc_test(void)
{
	vm_offset_t	buf = 0, idx_buf = 0;
	uint32_t	idx_size = (PMC_BIG / PMC_LINE) * sizeof (uint32_t);
	uint64_t	d[3][PMC_MAX], m[PMC_MAX], p;
	int		n = pmc_count(), w, i, run, role, checks = 0, wrong = 0;
	const struct pmc_walk *wk;
	char		*q;

	if (n == 0)
		return;
	if (kmem_alloc(kernel_map, &buf, PMC_BIG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &idx_buf, idx_size) != KERN_SUCCESS) {
		printf("pmc test (#804): not run -- no %u MiB to walk\n",
		       PMC_BIG >> 20);
		if (buf != 0)
			kmem_free(kernel_map, buf, PMC_BIG);
		return;
	}

	for (w = 0; w < W_COUNT; w++) {
		wk = &pmc_walks[w];
		p = pmc_cycle(buf, wk->size / wk->stride, wk->stride,
			      (uint32_t *) idx_buf);
		if (wk->warm)
			p = pmc_follow(p, wk->size / wk->stride, wk->store,
				       0, n);
		for (run = 0; run < 3; run++) {
			for (i = 0; i < n; i++)
				d[run][i] = 0;
			p = pmc_follow(p, wk->steps, wk->store, d[run], n);
		}
		pmc_sink = p;
		for (i = 0; i < n; i++)
			m[i] = pmc_median3(d[0][i], d[1][i], d[2][i]);

		q = pmc_test_line;
		q += sprintf(q, "pmc test (#804): %s, %u steps:", wk->what,
			     wk->steps);
		for (i = 0; i < n; i++) {
			const struct pmc_rule *r;
			uint64_t		pct;

			role = pmc_role(i);
			r = &pmc_rules[role][w];
			pct = m[i] * 100 / wk->steps;
			q += sprintf(q, " %s %llu", pmc_role_short[role],
				     (unsigned long long) m[i]);
			if (r->min < 0)
				continue;
			checks++;
			if (pct < (uint64_t) r->min ||
			    (r->max != 0 && pct > (uint64_t) r->max)) {
				wrong++;
				q += sprintf(q, " (WRONG: %llu%% of the steps, not "
					     "%d..%d)", (unsigned long long) pct,
					     r->min, r->max);
			}
		}
		printf("%s\n", pmc_test_line);
	}

	/* Kept: see the head of this file. */

	q = pmc_test_line;
	q += sprintf(q, "pmc test (#804): %d checks against loads of known shape, "
		     "the median of three runs each, %d WRONG%s; not checked "
		     "here:", checks, wrong, wrong != 0 ?
		     " -- a counter that counts something else" : "");
	for (i = 0; i < n; i++) {
		role = pmc_role(i);
		if (role == PMC_ROLE_HITM)
			q += sprintf(q, " %s (needs a second core writing)",
				     pmc_role_short[role]);
		else if (role == PMC_ROLE_ITLB_WALK)
			q += sprintf(q, " %s (needs code over many pages)",
				     pmc_role_short[role]);
	}
	printf("%s\n", pmc_test_line);
}
