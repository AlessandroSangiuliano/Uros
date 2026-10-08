/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A function's MSI-X enable, raced from two processors (#598's C10, -V).
 *
 * The enable is one bit for all of a function's entries, so detaching a slot
 * asks whether it was the function's last before clearing it.  Two sides, each
 * on its own processor, each with a slot and an entry of the same function,
 * attach and detach in turn.  While its own entry is attached a side reads the
 * function's control word, and the enable must be set every time.  Without the
 * machine layer's lock (msi_attach(), msi_detach()), one side's question can be
 * answered before the other's record and its clear land after the other's
 * enable: an interrupt nobody asked to silence.
 *
 * 🔑 Only a device nothing has claimed: the first function on bus 0 whose MSI-X
 * table has three entries or more, at boot, before bootstrap starts the
 * drivers.  Its entries 1 and 2 are used, and both slots are detached after,
 * so the function is left with its MSI-X enable clear, as a detach leaves it.
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
#include <cpu/msi_race_test.h>
#include <cpu/pci_cfg.h>
#include <cpu/pci_msix.h>
#include <cpu/regs.h>
#include <cpu/spl.h>
#include <device/device_machdep.h>
#include <device/pci.h>
#include <sync/atomic.h>
#include <sync/barrier.h>

/*
 * How many rounds each side makes, and how many times at most it reads the
 * enable while attached.  A round costs a few configuration accesses, each a
 * few microseconds of emulated MMIO; enough rounds for an unlocked kernel with
 * its window held open to be caught many times, few enough that a boot does
 * not notice.
 *
 * ⚠️ HOW MANY varies from round to round, and differently on each side.  With
 * a fixed count the two sides ran rounds of one length, so whatever phase
 * they started in they kept: an unlocked kernel was caught in one boot of two,
 * three rounds of 4000, and passed the other.  A count that varies sweeps the
 * phase, so each side's attach lands across the whole of the other's detach.
 */
#define RACE_ROUNDS	2000
#define RACE_LOOKS	256

/*
 * The rendezvous, in one word, as in ioapic_race_test.c: the second side moves
 * it from WAITING to READY, the first from READY to GO -- or from WAITING to
 * GIVEN_UP, when the second did not arrive in time.
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
static volatile uint64_t	race_lost[2];	/* rounds the enable was clear */
static int			race_parked;

static struct pci_msix		race_m;
static unsigned int		race_slot[2];
static unsigned long long	race_addr[2];
static unsigned int		race_data[2];

static void
race_handler(int irq)
{
	(void)irq;
}

static int
race_enabled(void)
{
	uint16_t control = pci_cfg_read16(race_m.segment, race_m.bus,
					  race_m.dev, race_m.func,
					  (uint16_t)(race_m.cap
						     + PCI_MSIX_CONTROL));

	return (control & PCI_MSIX_CTL_ENABLE) != 0;
}

static void
race_side(int side)
{
	struct pci_msix	gone;
	uint32_t	looks;

	for (uint32_t n = 0; n < RACE_ROUNDS && !race_stop; n++) {
		/*
		 * Inside, then a look at the other: the store must be visible
		 * before the load, the one order x86-TSO does not keep.
		 */
		race_inside[side] = 1;
		smp_mb();
		if (race_inside[!side])
			race_overlap[side]++;

		msi_attach(race_slot[side], &race_m, 1u + (unsigned int)side,
			   race_addr[side], race_data[side]);
		looks = 1u + ((n + 1u) * 2654435761u
			      >> (side ? 19 : 23)) % RACE_LOOKS;
		for (unsigned int k = 0; k < looks; k++)
			if (!race_enabled()) {
				race_lost[side]++;
				break;
			}
		(void) msi_detach(race_slot[side], &gone);
		race_inside[side] = 0;
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

	/* Parked for ever, as ioapic_race_test.c leaves its second side. */
	for (;;) {
		spl_t s = splsched();

		assert_wait((event_t) &race_parked, FALSE);
		splx(s);
		thread_block((void (*)(void)) 0);
	}
}

/* The first function on bus 0 with an MSI-X table of three entries or more. */
static int
race_find(void)
{
	for (unsigned int dev = 0; dev < 32; dev++)
		if (pci_cfg_find_cap(0, 0, (uint8_t)dev, 0, PCI_CAP_ID_MSIX)
		    && pci_msix_probe(0, 0, (uint8_t)dev, 0, &race_m)
		    && race_m.vectors >= 3)
			return 1;
	return 0;
}

/* As msix_table_selftest() gives its slot back: the entry, then the slot. */
static void
race_give_back(unsigned int claimed)
{
	for (unsigned int s = 0; s < claimed; s++) {
		msi_unremap_vector(race_slot[s], race_m.bus, race_m.dev,
				   race_m.func);
		device_md_msi_unregister(race_slot[s]);
	}
}

static void msi_function_race_test_body(int me);

/* The driver stays on the processor it chose from (#646). */
void
msi_function_race_test(void)
{
	processor_t	was = current_thread()->bound_processor;

	msi_function_race_test_body(thread_bind_here());
	thread_bind(current_thread(), was);
}

static void
msi_function_race_test_body(int me)
{
	thread_t	th;
	thread_act_t	act;
	processor_t	target = PROCESSOR_NULL;
	unsigned int	claimed = 0;
	uint64_t	spins, lost, overlap;
	spl_t		s;

	if (!race_find()) {
		printf("msi_race: NOT ASKED — no function on bus 0 has an MSI-X "
		       "table of three entries (#598)\n");
		return;
	}

	/* A processor that is not this one, as ioapic_race_test.c chooses. */
	for (int i = 0; i < NCPUS; i++) {
		if (i == me || !machine_slot[i].is_cpu || !machine_slot[i].running)
			continue;
		target = cpu_to_processor(i);
		break;
	}
	if (target == PROCESSOR_NULL) {
		printf("msi_race: NOT ASKED — alone, no processor other than "
		       "this one is running (#598)\n");
		return;
	}

	/*
	 * Two slots, claimed once: a slot is spent and not freed, so the race
	 * attaches and detaches the same two rather than registering anew.
	 */
	for (; claimed < 2; claimed++)
		if (!msi_claim_vector(race_handler, &race_slot[claimed],
				      &race_addr[claimed], &race_data[claimed])
		    || !msi_remap_vector(race_slot[claimed], race_m.bus,
					 race_m.dev, race_m.func,
					 &race_addr[claimed],
					 &race_data[claimed])) {
			printf("msi_race: WRONG — slot %u could not be claimed "
			       "and given an entry (#598)\n", claimed);
			race_give_back(claimed);
			return;
		}

	if (thread_create_at(kernel_task, &th, race_probe_body) != KERN_SUCCESS) {
		printf("msi_race: WRONG — could not create the second side's "
		       "thread (#598)\n");
		race_give_back(claimed);
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

	printf("msi_race: racing entries 1 and 2 of %02x:%02x.%u from "
	       "processors %d and %d, %u attaches and detaches each, the "
	       "function's enable read while attached (#598)\n", race_m.bus,
	       race_m.dev, race_m.func, me, target->slot_num, RACE_ROUNDS);

	for (spins = 0; spins < CPU_SPIN_BUDGET && race_state == RACE_WAITING;
	     spins++)
		cpu_pause();
	if (atomic_cmpxchg32(&race_state, RACE_WAITING, RACE_GIVEN_UP) ==
	    RACE_WAITING) {
		printf("msi_race: WRONG — the second side never ran on "
		       "processor %d; if it runs now it parks without touching "
		       "its slot (#598)\n", target->slot_num);
		race_give_back(claimed);
		return;
	}

	race_state = RACE_GO;
	race_side(0);
	race_done[0] = 1;

	for (spins = 0; spins < CPU_SPIN_BUDGET && !race_done[1]; spins++)
		cpu_pause();
	if (!race_done[1]) {
		race_stop = 1;
		for (spins = 0; spins < CPU_SPIN_BUDGET && !race_done[1]; spins++)
			cpu_pause();
		printf("msi_race: WRONG — the second side did not finish its "
		       "%u rounds in time on processor %d%s (#598)\n",
		       RACE_ROUNDS, target->slot_num, race_done[1] ? "" :
		       ", nor stop when told: its slots are not given back");
		if (race_done[1])
			race_give_back(claimed);
		return;
	}

	race_give_back(claimed);

	lost = race_lost[0] + race_lost[1];
	overlap = race_overlap[0] + race_overlap[1];

	if (lost != 0) {
		printf("msi_race: WRONG — in %llu of %u rounds the function's "
		       "MSI-X enable was found clear under an entry still "
		       "attached; the other side was mid-round for %llu rounds "
		       "(#598)\n", (unsigned long long) lost, 2 * RACE_ROUNDS,
		       (unsigned long long) overlap);
		return;
	}
	if (overlap == 0) {
		printf("msi_race: NOT ASKED — the two sides never overlapped, "
		       "so the enable was not raced (#598)\n");
		return;
	}
	printf("msi_race: PASS — %u attaches and detaches on each of entries 1 "
	       "and 2, the function's enable set every time it was read under "
	       "an attached entry; the other side was mid-round for %llu of "
	       "them (#598)\n", RACE_ROUNDS, (unsigned long long) overlap);
}
