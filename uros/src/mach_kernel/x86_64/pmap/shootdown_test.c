/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -J: a shootdown whose thread is moved half-way (#638).
 *
 * ── What can go wrong ─────────────────────────────────────────────────
 *
 * A shootdown asks which processor it is on and acts on the answer twice:
 * tlb_flush_range() flushes "this processor" itself and cross-calls the
 * others, and ipi_call_mask() strikes "this processor" from the set it sends
 * to.  At level zero a thread can be preempted at any instruction and resume
 * on any processor, so the answer can go stale between asking and acting:
 *
 *   [1] the local flush runs on the processor the thread leaves, and the
 *	 cross-call goes out from the one it arrives at -- which leaves itself
 *	 out.  Nobody flushes the processor arrived at, and it can hold the
 *	 translation: a switch between two threads with the same map does not
 *	 reload CR3.  The entry is gone from the table and alive in that TLB,
 *	 and nothing says so.
 *   [2] the bit struck is the processor left behind, and the one arrived at
 *	 stays in the set: it sends the call to itself, holding call_lock with
 *	 interrupts masked, and waits for an answer it cannot give.  That is
 *	 the panic #638 was opened on.
 *
 * ── How each is asked ─────────────────────────────────────────────────
 *
 * [1] Each remapper owns a kernel page and points it, in turn, at one of
 *     three frames: it writes the next generation's number into the frame,
 *     repoints the entry in place -- never through an empty entry, which a
 *     reader would fault on -- and calls tlb_flush_range().  Then it reads
 *     the page, on whatever processor it is on now: anything but the number
 *     it wrote is a translation that outlived the shootdown.  Readers bound
 *     one to each processor read every page without pause, so that whichever
 *     processor a remapper arrives at holds the old translation, and check
 *     it themselves whenever no remap of that page is in flight.
 * [2] Threads call ipi_call_mask() with every processor in the set, their
 *     own included, for the call to strike.  A call sent to its own sender
 *     ends the boot with #638's panic, which names it -- so [2] runs last.
 *
 * Both run on threads free to move, two for every processor, and count the
 * operations during which the thread changed processor: without a move
 * neither defect can show, and a clean run means nothing (#563).  Each arm
 * runs until twenty have moved, or thirty seconds.
 *
 * ── The widened window ────────────────────────────────────────────────
 *
 * Each window is a few instructions, and #638 was seen once in 120 boots.
 * UROS_WIDEN_638_WINDOW makes a shootdown spin for 50 us between deciding
 * which processor it is on and acting on it, while an arm runs (<cpu/ipi.h>,
 * which also says why it is one spin and not two).  Without the option the
 * arms still run, and say what they saw.
 */

#include <stdint.h>

#include <kern/cpu_number.h>
#include <kern/lock.h>			/* mutex_pause */
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched.h>			/* BASEPRI_USER */
#include <kern/sched_prim.h>		/* thread_bind_locked */
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_swap.h>		/* thread_swappable */
#include <vm/vm_kern.h>			/* kmem_alloc_wired, kernel_map */

#include <cpu/ipi.h>
#include <cpu/regs.h>			/* cpu_pause */
#include <cpu/smp.h>			/* smp_answering_set */
#include <pmap/pmap.h>			/* pmap_kernel, kvtophys */
#include <pmap/pte.h>
#include <pmap/shootdown_test.h>
#include <pmap/tlb.h>
#include <pmap/walk.h>
#include <sync/atomic.h>
#include <time/tsc.h>

#define	SHOOT_TEST_PER_CPU	2	/* threads a processor that may move */
#define	SHOOT_TEST_FRAMES	3	/* the frames a page is pointed at */

/*
 * A count, not a duration, for spl_test.c's reason: moves come when a quantum
 * runs out and another processor takes the thread, at a rate the accelerator
 * and the clock decide.  With the window widened about half the moves land
 * inside the harmful spin, and twenty leave a defect that strikes half of
 * them less than one chance in a million of going unseen.
 */
#define	SHOOT_TEST_MIN_MOVED	20
#define	SHOOT_TEST_MAX_SECONDS	30	/* and NOT ASKED if it takes longer */

#define	SHOOT_TEST_MAX_PAGES	(SHOOT_TEST_PER_CPU * NCPUS)
#define	SHOOT_TEST_SPAN		((1 + SHOOT_TEST_FRAMES) * PAGE_SIZE_4K)

/*
 * One remapper's page.  `base' is what kmem_alloc_wired() gave: the page the
 * readers read, then one page for each frame, whose own mapping never changes
 * -- which is how a remapper writes a frame it has not mapped yet.
 */
struct shoot_page {
	vm_offset_t		base;
	pt_entry_t		*entry;		/* base's entry, kernel tables */
	uint64_t		home;		/* base's own frame, put back */
	uint64_t		frame[SHOOT_TEST_FRAMES];
	volatile uint64_t	gen_begin;	/* the remap in flight, or last */
	volatile uint64_t	gen_end;	/* the last remap shot down */
};

static struct shoot_page	shoot_pages[SHOOT_TEST_MAX_PAGES];
static uint32_t			shoot_npages;

static volatile int		shoot_go;
static volatile int		shoot_readers_stop;
static volatile uint32_t	shoot_started;
static volatile uint32_t	shoot_finished;
static volatile uint32_t	shoot_readers_finished;
static volatile uint32_t	shoot_claimed;		/* the next page to own */
static volatile uint64_t	shoot_deadline;
static uint64_t			shoot_everyone;		/* [2]'s set */

static volatile uint64_t	shoot_ops;
static volatile uint64_t	shoot_moved;		/* as they happen */
static volatile uint64_t	shoot_stale;		/* seen by a remapper */
static volatile uint64_t	shoot_reader_stale;	/* seen by a reader */

/* The first of each, as found, for the line that says what it was. */
static volatile uint32_t	shoot_stale_said;
static int			shoot_stale_from, shoot_stale_to;
static uint64_t			shoot_stale_seen, shoot_stale_wanted;
static volatile uint32_t	shoot_reader_said;
static int			shoot_reader_on;
static uint64_t			shoot_reader_seen, shoot_reader_wanted;

static void
shoot_reset(void)
{
	shoot_go = 0;
	shoot_readers_stop = 0;
	shoot_started = 0;
	shoot_finished = 0;
	shoot_readers_finished = 0;
	shoot_claimed = 0;
	shoot_ops = 0;
	shoot_moved = 0;
	shoot_stale = 0;
	shoot_reader_stale = 0;
	shoot_stale_said = 0;
	shoot_reader_said = 0;
}

/*
 * The remapper: repoint, shoot down, read back where it now is.  The entry is
 * written whole in one store, so a reader finds either frame and never an
 * empty entry; the generation brackets the remap for the readers' sake.
 */
static void
shoot_remap_body(void)
{
	struct shoot_page	*p = &shoot_pages[atomic_add32(&shoot_claimed, 1)];
	uint64_t		ops = 0, stale = 0;

	atomic_add32(&shoot_started, 1);
	while (!shoot_go)
		cpu_pause();

	for (;;) {
		uint64_t	n = p->gen_end + 1;
		unsigned	i = (unsigned) (n % SHOOT_TEST_FRAMES);
		pt_entry_t	e = *p->entry;
		int		before, after;
		uint64_t	seen;

		*(volatile uint64_t *) (p->base + (i + 1) * PAGE_SIZE_4K) = n;
		p->gen_begin = n;
		atomic_store64((volatile uint64_t *) p->entry,
			       pa_to_pte(p->frame[i]) | (e & ~INTEL_PTE_PFN));

		before = cpu_number();
		tlb_flush_range(pmap_kernel(), p->base, PAGE_SIZE_4K);
		after = cpu_number();
		seen = *(volatile uint64_t *) p->base;
		p->gen_end = n;
		ops++;

		if (after != before)
			atomic_add64(&shoot_moved, 1);
		if (seen != n) {
			stale++;
			if (atomic_cmpxchg32(&shoot_stale_said, 0, 1) == 0) {
				shoot_stale_from = before;
				shoot_stale_to = after;
				shoot_stale_seen = seen;
				shoot_stale_wanted = n;
			}
		}

		if ((ops & 15) == 0
		    && (shoot_moved >= SHOOT_TEST_MIN_MOVED
			|| rdtsc() >= shoot_deadline))
			break;
	}

	atomic_add64(&shoot_ops, ops);
	atomic_add64(&shoot_stale, stale);
	atomic_add32(&shoot_finished, 1);
	thread_terminate_self();
}

/*
 * The reader: every page, without pause, on its own processor.
 *
 * A read counts only between remaps -- `end' read before the page, `begin'
 * after it, and the two equal -- and only an OLDER number counts against it:
 * that is the frame of a generation already shot down, through a translation
 * that should not exist.  Of the three frames, the two not mapped are
 * rewritten only for generations later than `begin', so between remaps a
 * stale translation can only show a number below `end'.
 */
static void
shoot_read_body(void)
{
	uint64_t	stale = 0;
	uint32_t	k;

	while (!shoot_readers_stop) {
		for (k = 0; k < shoot_npages; k++) {
			struct shoot_page *p = &shoot_pages[k];
			uint64_t end = p->gen_end;
			uint64_t seen = *(volatile uint64_t *) p->base;
			uint64_t begin = p->gen_begin;

			if (begin != end || seen >= end)
				continue;
			stale++;
			if (atomic_cmpxchg32(&shoot_reader_said, 0, 1) == 0) {
				shoot_reader_on = cpu_number();
				shoot_reader_seen = seen;
				shoot_reader_wanted = end;
			}
		}
	}

	atomic_add64(&shoot_reader_stale, stale);
	atomic_add32(&shoot_readers_finished, 1);
	thread_terminate_self();
}

static void
shoot_noop(void *arg)
{
	(void) arg;
}

/* [2]: a cross-call to everyone, this processor included, and nothing else. */
static void
shoot_call_body(void)
{
	uint64_t	ops = 0;

	atomic_add32(&shoot_started, 1);
	while (!shoot_go)
		cpu_pause();

	for (;;) {
		int	before = cpu_number();

		ipi_call_mask(shoot_everyone, shoot_noop, 0);
		ops++;
		if (cpu_number() != before)
			atomic_add64(&shoot_moved, 1);

		if ((ops & 15) == 0
		    && (shoot_moved >= SHOOT_TEST_MIN_MOVED
			|| rdtsc() >= shoot_deadline))
			break;
	}

	atomic_add64(&shoot_ops, ops);
	atomic_add32(&shoot_finished, 1);
	thread_terminate_self();
}

/*
 * spl_test.c's thread at a user thread's priority, bound to `where' unless it
 * is PROCESSOR_NULL -- and bound before it can run, for the reason
 * preempt_test.c gives: a bind after thread_setrun() comes after the race.
 */
static thread_t
shoot_thread(void (*fn)(void), processor_t where)
{
	thread_t	th;
	thread_act_t	act;
	spl_t		s;

	if (thread_create_at(kernel_task, &th, fn) != KERN_SUCCESS)
		return THREAD_NULL;

	thread_swappable(th->top_act, FALSE);

	s = splsched();
	thread_lock(th);

	act = th->top_act;
	th->max_priority = BASEPRI_USER;
	th->priority = BASEPRI_USER;
	th->sched_pri = BASEPRI_USER;
	if (where != PROCESSOR_NULL)
		thread_bind_locked(th, where);

	th->state |= TH_RUN;
	thread_setrun(th, TRUE, TAIL_Q);
	thread_unlock(th);
	splx(s);

	act_deallocate(act);
	thread_resume(act);

	return th;
}

/* spl_test.c's wait: a tick asleep at a time, so as not to take a processor. */
static boolean_t
shoot_wait(volatile uint32_t *count, uint32_t want, uint64_t hz,
	   unsigned seconds)
{
	uint64_t	t0 = rdtsc();

	while (*count < want) {
		if (rdtsc() - t0 >= hz * seconds)
			return FALSE;
		mutex_pause();
	}
	return TRUE;
}

/*
 * A page and its frames, wired.  The entry is looked up once and kept: a
 * kernel address needs no read section to walk, and its tables are never
 * collected (<pmap/pmap.h>, pmap_read_enter_for).
 */
static boolean_t
shoot_page_init(struct shoot_page *p)
{
	uint64_t	size = 0;
	unsigned	i;

	if (kmem_alloc_wired(kernel_map, &p->base, SHOOT_TEST_SPAN)
	    != KERN_SUCCESS)
		return FALSE;

	p->entry = pmap_walk(pmap_kernel()->root_pa, p->base, &size);
	if (p->entry == PT_ENTRY_NULL || size != PAGE_SIZE_4K) {
		kmem_free(kernel_map, p->base, SHOOT_TEST_SPAN);
		return FALSE;
	}

	p->home = pte_to_pa(*p->entry);
	for (i = 0; i < SHOOT_TEST_FRAMES; i++)
		p->frame[i] = kvtophys(p->base + (i + 1) * PAGE_SIZE_4K);

	*(volatile uint64_t *) p->base = 0;	/* generation 0, at home */
	p->gen_begin = 0;
	p->gen_end = 0;
	return TRUE;
}

/*
 * Each page back on its own frame before it goes: the VM recorded that one,
 * and kmem_free() takes down what the VM recorded.
 */
static void
shoot_pages_fini(uint32_t n)
{
	uint32_t	k;

	for (k = 0; k < n; k++) {
		struct shoot_page *p = &shoot_pages[k];
		pt_entry_t e = *p->entry;

		atomic_store64((volatile uint64_t *) p->entry,
			       pa_to_pte(p->home) | (e & ~INTEL_PTE_PFN));
		tlb_flush_range(pmap_kernel(), p->base, PAGE_SIZE_4K);
		kmem_free(kernel_map, p->base, SHOOT_TEST_SPAN);
	}
}

static void
shoot_arm_window(int on)
{
#if	WIDEN_638_WINDOW
	shootdown_widen_armed = on;
#else
	(void) on;
#endif
}

static void
shoot_stale_arm(processor_t *online, uint32_t ncpu, uint64_t hz)
{
	uint32_t	want = 0, readers = 0, k;
	boolean_t	back;
	uint64_t	t0, ms;

	shoot_reset();
	shoot_npages = 0;
	for (k = 0; k < ncpu * SHOOT_TEST_PER_CPU; k++) {
		if (!shoot_page_init(&shoot_pages[k]))
			break;
		shoot_npages++;
	}
	if (shoot_npages < ncpu * SHOOT_TEST_PER_CPU) {
		printf("shootdown_test: [1] NOT ASKED — %u of %u pages could "
		       "be set up (#563)\n", (unsigned) shoot_npages,
		       (unsigned) (ncpu * SHOOT_TEST_PER_CPU));
		shoot_pages_fini(shoot_npages);
		return;
	}

	for (k = 0; k < ncpu; k++)
		if (shoot_thread(shoot_read_body, online[k]) != THREAD_NULL)
			readers++;
	for (k = 0; k < shoot_npages; k++)
		if (shoot_thread(shoot_remap_body, PROCESSOR_NULL)
		    != THREAD_NULL)
			want++;

	if (!shoot_wait(&shoot_started, want, hz, 5) || want < shoot_npages
	    || readers < ncpu) {
		printf("shootdown_test: [1] NOT ASKED — %u of %u remappers "
		       "started in 5 s, %u of %u readers created (#563)\n",
		       (unsigned) shoot_started, (unsigned) shoot_npages,
		       (unsigned) readers, (unsigned) ncpu);
		shoot_deadline = 0;
		shoot_go = 1;
		back = shoot_wait(&shoot_finished, want, hz, 5);
		shoot_readers_stop = 1;
		if (shoot_wait(&shoot_readers_finished, readers, hz, 5) && back)
			shoot_pages_fini(shoot_npages);
		return;
	}

	printf("shootdown_test: [1] starting: %u remappers free to move "
	       "repoint a page each and shoot it down, while %u readers, one "
	       "bound to each processor, read every page; until %u remaps have "
	       "moved, for %u s at most (#638)\n", (unsigned) want,
	       (unsigned) readers, SHOOT_TEST_MIN_MOVED,
	       SHOOT_TEST_MAX_SECONDS);

	t0 = rdtsc();
	shoot_deadline = t0 + hz * SHOOT_TEST_MAX_SECONDS;
	shoot_arm_window(1);
	shoot_go = 1;

	back = shoot_wait(&shoot_finished, want, hz, SHOOT_TEST_MAX_SECONDS + 5);
	shoot_arm_window(0);
	ms = (rdtsc() - t0) * 1000 / hz;
	shoot_readers_stop = 1;

	/*
	 * ⚠️ Nothing is freed while anyone may still touch it: a page taken
	 * away under a reader is a fault in the kernel, and the report would
	 * be about the test instead of the shootdown.
	 */
	if (!back) {
		printf("shootdown_test: [1] WRONG — %u of %u remappers came back "
		       "in %u s: a shootdown never finished; the pages stay "
		       "(#638)\n", (unsigned) shoot_finished, (unsigned) want,
		       SHOOT_TEST_MAX_SECONDS + 5);
		return;
	}
	if (!shoot_wait(&shoot_readers_finished, readers, hz, 5)) {
		printf("shootdown_test: [1] WRONG — %u of %u readers came back "
		       "in 5 s; the pages stay (#638)\n",
		       (unsigned) shoot_readers_finished, (unsigned) readers);
		return;
	}
	shoot_pages_fini(shoot_npages);

	printf("shootdown_test: [1] %u remappers on %u processors for %u ms: "
	       "%lu remaps shot down, %lu of them with the remapper on another "
	       "processor by the time the shootdown returned\n",
	       (unsigned) want, (unsigned) ncpu, (unsigned) ms,
	       (unsigned long) shoot_ops, (unsigned long) shoot_moved);

	if (shoot_stale != 0 || shoot_reader_stale != 0) {
		if (shoot_stale != 0)
			printf("shootdown_test:   %lu remaps were read back "
			       "through the old translation; the first began "
			       "on processor %d and read generation %lu instead "
			       "of %lu on processor %d\n",
			       (unsigned long) shoot_stale, shoot_stale_from,
			       (unsigned long) shoot_stale_seen,
			       (unsigned long) shoot_stale_wanted,
			       shoot_stale_to);
		if (shoot_reader_stale != 0)
			printf("shootdown_test:   %lu reads between remaps found "
			       "an old generation; the first on processor %d, "
			       "%lu instead of %lu\n",
			       (unsigned long) shoot_reader_stale,
			       shoot_reader_on,
			       (unsigned long) shoot_reader_seen,
			       (unsigned long) shoot_reader_wanted);
		printf("shootdown_test: [1] WRONG — a translation outlived the "
		       "shootdown that removed it (#638)\n");
	} else if (shoot_moved < SHOOT_TEST_MIN_MOVED)
		printf("shootdown_test: [1] NOT ASKED — %lu remaps moved to "
		       "another processor in %u s, fewer than %u: a shootdown "
		       "split by a move had too little chance to show (#563)\n",
		       (unsigned long) shoot_moved, SHOOT_TEST_MAX_SECONDS,
		       SHOOT_TEST_MIN_MOVED);
	else
		printf("shootdown_test: [1] PASS — every remap was read back "
		       "through its own frame, on every processor, %lu of them "
		       "across a move to another processor (#638)\n",
		       (unsigned long) shoot_moved);
}

static void
shoot_self_arm(uint32_t ncpu, uint64_t hz)
{
	uint32_t	want = 0, k;
	uint64_t	t0, ms;

	shoot_reset();
	shoot_everyone = smp_answering_set();

	for (k = 0; k < ncpu * SHOOT_TEST_PER_CPU; k++)
		if (shoot_thread(shoot_call_body, PROCESSOR_NULL) != THREAD_NULL)
			want++;

	if (!shoot_wait(&shoot_started, want, hz, 5)
	    || want < ncpu * SHOOT_TEST_PER_CPU) {
		printf("shootdown_test: [2] NOT ASKED — %u of %u threads "
		       "started in 5 s (#563)\n", (unsigned) shoot_started,
		       (unsigned) (ncpu * SHOOT_TEST_PER_CPU));
		shoot_deadline = 0;
		shoot_go = 1;
		(void) shoot_wait(&shoot_finished, want, hz, 5);
		return;
	}

	printf("shootdown_test: [2] starting: %u threads on %u processors send "
	       "cross-calls to every processor, their own included, until %u "
	       "calls have moved, for %u s at most (#638)\n", (unsigned) want,
	       (unsigned) ncpu, SHOOT_TEST_MIN_MOVED, SHOOT_TEST_MAX_SECONDS);

	t0 = rdtsc();
	shoot_deadline = t0 + hz * SHOOT_TEST_MAX_SECONDS;
	shoot_arm_window(1);
	shoot_go = 1;

	if (!shoot_wait(&shoot_finished, want, hz, SHOOT_TEST_MAX_SECONDS + 5)) {
		shoot_arm_window(0);
		printf("shootdown_test: [2] WRONG — %u of %u threads came back "
		       "in %u s (#638)\n", (unsigned) shoot_finished,
		       (unsigned) want, SHOOT_TEST_MAX_SECONDS + 5);
		return;
	}
	shoot_arm_window(0);
	ms = (rdtsc() - t0) * 1000 / hz;

	printf("shootdown_test: [2] %u threads on %u processors for %u ms: %lu "
	       "cross-calls, %lu of them with the thread on another processor "
	       "by the time the call returned\n", (unsigned) want,
	       (unsigned) ncpu, (unsigned) ms, (unsigned long) shoot_ops,
	       (unsigned long) shoot_moved);

	if (shoot_moved < SHOOT_TEST_MIN_MOVED)
		printf("shootdown_test: [2] NOT ASKED — %lu cross-calls moved "
		       "to another processor in %u s, fewer than %u: a call "
		       "split by a move had too little chance to show (#563)\n",
		       (unsigned long) shoot_moved, SHOOT_TEST_MAX_SECONDS,
		       SHOOT_TEST_MIN_MOVED);
	else
		printf("shootdown_test: [2] PASS — every cross-call left its "
		       "own sender out, %lu of them across a move to another "
		       "processor (#638)\n", (unsigned long) shoot_moved);
}

void
shootdown_moved_test(void)
{
	processor_t	online[NCPUS];
	uint64_t	hz = tsc_hz();
	uint32_t	ncpu = 0;
	int		i;

	if (hz == 0) {
		printf("shootdown_test: NOT ASKED — no calibrated TSC to time "
		       "the arms with (#563)\n");
		return;
	}

	for (i = 0; i < NCPUS; i++) {
		processor_t p = cpu_to_processor(i);

		if (p != PROCESSOR_NULL && p->state != PROCESSOR_OFF_LINE)
			online[ncpu++] = p;
	}

	/*
	 * 🔑 NOT ASKED, and true: on one processor a preempted thread resumes
	 * where it was, so the processor it asked about is the one it is on.
	 */
	if (ncpu < 2) {
		printf("shootdown_test: NOT ASKED — %u processor: a thread "
		       "resumes where it was, so a shootdown cannot be split "
		       "between two (#638)\n", (unsigned) ncpu);
		return;
	}

	shoot_stale_arm(online, ncpu, hz);
	shoot_self_arm(ncpu, hz);
}
