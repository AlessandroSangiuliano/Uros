/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The I/O APIC's window, raced from two processors (#599, -Y).
 *
 * A redirection entry is reached by writing its register number to one
 * address and then reading or writing the other, so a second processor that
 * selects between the two makes the first one read or write a DIFFERENT
 * register.  On i386 that put pin 11's entry on pin 2 and stopped processor 0's
 * clock for good (#599).  x86-64 takes the window in turns since #593
 * (cpu/ioapic.c); this is the test that shows the lock is what keeps the two
 * apart, by taking it away (UROS_ABLATE_599_WINDOW_OPEN, _WINDOW_WIDEN).
 *
 * Two sides, each on its own processor, each with a pin of its own.  A round
 * changes the pin's vector by the read-modify-write that masks and unmasks
 * pins -- the sequence the lock exists for -- and reads the low half back.
 * Each side writes vectors from a range of its own, so a read-back that is
 * not what the side wrote also says whose it was.
 *
 * 🔑 Only pins nobody uses.  A pin whose low half is still what ioapic_init()
 * wrote has never been routed; the pins stay masked for the whole test, so no
 * vector written here is ever delivered, and each low half is put back after.
 *
 * 🔴 A race test that never raced passes for nothing (#604).  Each side counts
 * the rounds in which the other was inside a round of its own; with none, the
 * question was not posed, and that is NOT ASKED, not PASS.
 */

#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_act.h>
#include <kern/thread_swap.h>
#include <mach/machine.h>

#include <cpu/ioapic.h>
#include <cpu/ioapic_race_test.h>
#include <cpu/regs.h>
#include <cpu/spl.h>
#include <sync/atomic.h>
#include <sync/barrier.h>

/*
 * How many rounds each side makes.  Enough for the ablated kernel to be caught
 * many times over (a round under the window's lock costs a few microseconds of
 * emulated MMIO), few enough that the unablated boot does not notice.
 */
#define RACE_ROUNDS	20000

/* Each side's vectors: a read-back in the other's range is the other's. */
#define RACE_TAG_0	0x20u		/* 0x20..0x7f */
#define RACE_TAG_1	0x80u		/* 0x80..0xdf */
#define RACE_SPAN	0x60u

static uint32_t			race_gsi[2];
static uint32_t			race_low[2];	/* what each pin held */
/*
 * The rendezvous, in one word: the second side moves it from WAITING to READY,
 * the first from READY to GO -- or from WAITING to GIVEN_UP, when the second
 * did not arrive in time.  One word, so that "it arrived" and "I gave up"
 * cannot both win: a second side that comes late finds GIVEN_UP and parks
 * without touching its pin.
 */
#define RACE_WAITING	0u
#define RACE_READY	1u
#define RACE_GO		2u
#define RACE_GIVEN_UP	3u
static volatile uint32_t	race_state;
static volatile int		race_stop;	/* the first side's word to quit */
static volatile int		race_done[2];
static volatile uint32_t	race_inside[2];
static volatile uint64_t	race_overlap[2];
static volatile uint64_t	race_wrong[2];
static volatile uint64_t	race_theirs[2];
static int			race_parked;

static void
race_side(int side)
{
	uint32_t	gsi = race_gsi[side];
	uint32_t	keep = race_low[side] & ~0xFFu;
	uint32_t	mine = side ? RACE_TAG_1 : RACE_TAG_0;
	uint32_t	theirs = side ? RACE_TAG_0 : RACE_TAG_1;

	for (uint32_t n = 0; n < RACE_ROUNDS && !race_stop; n++) {
		uint32_t want = mine + n % RACE_SPAN;
		uint32_t got;

		/*
		 * Inside, then a look at the other: the store must be visible
		 * before the load, the one order x86-TSO does not keep.
		 */
		race_inside[side] = 1;
		smp_mb();
		if (race_inside[!side])
			race_overlap[side]++;

		ioapic_set_vector(gsi, (uint8_t) want);
		got = ioapic_low_half(gsi);
		race_inside[side] = 0;

		if (got != (keep | want)) {
			race_wrong[side]++;
			if ((got & 0xFFu) - theirs < RACE_SPAN)
				race_theirs[side]++;
		}
	}
}

static void
race_probe_body(void)
{
	if (atomic_cmpxchg32(&race_state, RACE_WAITING, RACE_READY) ==
	    RACE_WAITING) {
		while (race_state == RACE_READY)
			cpu_pause();
		race_side(1);
		race_done[1] = 1;
	}

	/*
	 * Parked for ever in a wait nobody signals, as -S and -G leave theirs:
	 * the boot goes on, and a kernel thread has nowhere to return to.
	 */
	for (;;) {
		spl_t s = splsched();

		assert_wait((event_t) &race_parked, FALSE);
		splx(s);
		thread_block((void (*)(void)) 0);
	}
}

static void ioapic_window_race_test_body(int me);

/*
 * The driver stays on the processor it chose from (#646).  It read
 * cpu_number() bare, on an unbound thread, and then counted on running
 * there; a move in between left `me' naming a processor it had left.
 */
void
ioapic_window_race_test(void)
{
	processor_t	was = current_thread()->bound_processor;

	ioapic_window_race_test_body(thread_bind_here());
	thread_bind(current_thread(), was);
}

static void
ioapic_window_race_test_body(int me)
{
	thread_t	th;
	thread_act_t	act;
	processor_t	target = PROCESSOR_NULL;
	uint32_t	first = ioapic_first_gsi();
	unsigned	found = 0;
	uint64_t	spins, wrong, theirs, overlap;
	spl_t		s;

	/* The two highest pins nobody has routed. */
	for (unsigned i = ioapic_pin_count(); i > 0 && found < 2; i--) {
		if (ioapic_pin_untouched(first + i - 1))
			race_gsi[found++] = first + i - 1;
	}
	if (found < 2) {
		printf("ioapic_race: NOT ASKED — %u pins untouched since boot, "
		       "two are needed (#599)\n", found);
		return;
	}

	/* A processor that is not this one, as wait_preempt_test chooses. */
	for (int i = 0; i < NCPUS; i++) {
		if (i == me || !machine_slot[i].is_cpu || !machine_slot[i].running)
			continue;
		target = cpu_to_processor(i);
		break;
	}
	if (target == PROCESSOR_NULL) {
		printf("ioapic_race: NOT ASKED — alone, no processor other than "
		       "this one is running (#599)\n");
		return;
	}

	race_low[0] = ioapic_low_half(race_gsi[0]);
	race_low[1] = ioapic_low_half(race_gsi[1]);

	if (thread_create_at(kernel_task, &th, race_probe_body) != KERN_SUCCESS) {
		printf("ioapic_race: WRONG — could not create the second side's "
		       "thread (#599)\n");
		return;
	}
	act = th->top_act;
	thread_swappable(act, FALSE);

	s = splsched();
	thread_lock(th);
	th->max_priority = BASEPRI_SYSTEM;
	th->priority = BASEPRI_SYSTEM;
	th->sched_pri = BASEPRI_SYSTEM;
	thread_bind_locked(th, target);
	th->state |= TH_RUN;
	thread_setrun(th, TRUE, TAIL_Q);
	thread_unlock(th);
	splx(s);

	act_deallocate(act);
	thread_resume(act);

	printf("ioapic_race: racing pins %u and %u from processors %d and %d, "
	       "%u read-modify-writes of each pin's vector, each read back "
	       "(#599)\n", race_gsi[0], race_gsi[1], me, target->slot_num,
	       RACE_ROUNDS);

	/*
	 * Bounded: a side that never starts or never finishes is a verdict, and
	 * a test that waited for ever would report it as silence.
	 */
	for (spins = 0; spins < CPU_SPIN_BUDGET && race_state == RACE_WAITING;
	     spins++)
		cpu_pause();
	if (atomic_cmpxchg32(&race_state, RACE_WAITING, RACE_GIVEN_UP) ==
	    RACE_WAITING) {
		printf("ioapic_race: WRONG — the second side never ran on "
		       "processor %d; if it runs now it parks without touching "
		       "its pin (#599)\n", target->slot_num);
		return;
	}

	race_state = RACE_GO;
	race_side(0);
	race_done[0] = 1;

	/*
	 * Told to stop if it has not finished in time, and waited for again:
	 * the pins are put back only once neither side can touch them, as far
	 * as that can be known.
	 */
	for (spins = 0; spins < CPU_SPIN_BUDGET && !race_done[1]; spins++)
		cpu_pause();
	if (!race_done[1]) {
		race_stop = 1;
		for (spins = 0; spins < CPU_SPIN_BUDGET && !race_done[1]; spins++)
			cpu_pause();
		ioapic_set_low_half(race_gsi[0], race_low[0]);
		ioapic_set_low_half(race_gsi[1], race_low[1]);
		printf("ioapic_race: WRONG — the second side did not finish its "
		       "%u rounds in time on processor %d%s (#599)\n",
		       RACE_ROUNDS, target->slot_num, race_done[1] ? "" :
		       ", nor stop when told: its pin may change once more");
		return;
	}

	ioapic_set_low_half(race_gsi[0], race_low[0]);
	ioapic_set_low_half(race_gsi[1], race_low[1]);

	wrong = race_wrong[0] + race_wrong[1];
	theirs = race_theirs[0] + race_theirs[1];
	overlap = race_overlap[0] + race_overlap[1];

	if (wrong != 0) {
		printf("ioapic_race: WRONG — %llu of %u read-backs were not what "
		       "their side wrote, %llu of them the other pin's vector; "
		       "the other side was mid-round for %llu rounds (#599)\n",
		       (unsigned long long) wrong, 2 * RACE_ROUNDS,
		       (unsigned long long) theirs,
		       (unsigned long long) overlap);
		return;
	}
	if (overlap == 0) {
		printf("ioapic_race: NOT ASKED — the two sides never overlapped, "
		       "so the window was not raced (#599)\n");
		return;
	}
	printf("ioapic_race: PASS — %u read-modify-writes on each of pins %u and "
	       "%u, every read-back what its own side wrote; the other side was "
	       "mid-round for %llu of them (#599)\n", RACE_ROUNDS, race_gsi[0],
	       race_gsi[1], (unsigned long long) overlap);
}
