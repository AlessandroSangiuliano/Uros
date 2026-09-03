/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What a task that dies leaves behind, and whether anybody gets it back (#513).
 *
 * ── The defect ────────────────────────────────────────────────────────
 *
 * device_master recorded a driver's DMA regions and its device claims, and
 * released neither when the driver task died.  The comment beside the claim
 * table said so and called it "the safe direction".  It was not:
 *
 *	the claim's task_t is stored WITHOUT a reference, and is never
 *	dereferenced -- only compared -- so a recycled zone block makes a
 *	later, unrelated task compare EQUAL and inherit the device;
 *
 *	a region holds a task reference, so the task struct could never be
 *	freed at all, and its wired pages never came back.
 *
 * 🔑 Neither is a use-after-free.  Nothing invalid is ever accessed, which is
 * why both sat behind a comment asserting the arrangement was safe.
 *
 * ── Why this program is run TWICE ─────────────────────────────────────
 *
 * 🔴 A RECLAIM CANNOT BE SHOWN BY AN ABSENCE.  "The dead task's regions are
 * gone" and "the dead task never had any" are the same observation, so the
 * only evidence that discriminates is somebody else being GIVEN what it held.
 * That needs two tasks: one that dies holding the table, and one that comes
 * afterwards and finds room.
 *
 * The same binary is both, chosen by argv[1]:
 *
 *	hog    fill the region table, say so, and die still holding it
 *	check  wait for room, then take the table and report
 *
 * 🔑 It is ONE binary because the two roles must fill the table by exactly
 * the same code.  A checker that allocated differently from the hog would be
 * comparing two measurements instead of taking one twice.
 *
 * ── The synchronisation, which is the hard part ───────────────────────
 *
 * 🔴 THE ORDER TASKS ARE CREATED IS NOT THE ORDER THEY RUN.  bootstrap creates
 * and resumes each entry in turn, but the first instruction of entry N can
 * follow the first instruction of entry N+1 -- see the note on SERVER_SERIALIZE_F
 * in servers/bootstrap/bootstrap.c, where netname_test lost exactly that race
 * in five boots out of five.  A checker that simply ran second could therefore
 * take the table BEFORE the hog and invert the whole experiment.
 *
 * So the two are joined at both ends:
 *
 *  1. the hog's line carries `-w', and the hog calls bootstrap_completed()
 *     AFTER it has filled the table.  bootstrap does not create the checker
 *     until that message arrives, so the checker is guaranteed to start with
 *     the table already full.  ⚠️ It is the message and not the death that is
 *     waited on: a `-w' entry that only dies wedges bootstrap, which is a
 *     defect of its own and not this program's to work around.
 *
 *  2. the checker then waits for ROOM.  It cannot appear except by the hog
 *     dying and the kernel taking its regions back, because nothing else in
 *     the bundle is running at that point and the hog frees nothing.
 *
 * ⚠️ The wait is BOUNDED.  On a kernel with no reclaim the room never comes,
 * and a test that waited for ever would report a hang instead of a failure.
 *
 * ── Reading the result ────────────────────────────────────────────────
 *
 * With the hook in place the checker takes the whole table.  With it removed
 * the checker takes NOTHING -- the hog still holds all sixteen slots -- so
 * arm [1] is not a threshold that has to be argued about: the two hypotheses
 * predict the full table and zero.
 *
 * ⚠️ Which also means a failure here is loud downstream: on a kernel without
 * the reclaim, block_device_server gets no DMA buffer either and the disk does
 * not come up.  That is deliberate.  A mechanism this one is measuring, that
 * everything else quietly works around, is one nobody would notice losing.
 */

#include <stdio.h>
#include <string.h>
#include <mach.h>
#include <mach/bootstrap.h>
#include <mach/thread_switch.h>
#include <sa_mach.h>			/* printf_init -- the header, not an
					 * extern beside the call: see the note
					 * in <device/device_master.h> */
#include <device/device_types.h>	/* DEVICE_DMA_NO_BDF */
#include "device_master.h"		/* the user half of device_master.defs */

/*
 * ⚠️ A CEILING AND NOT THE KERNEL'S NUMBER.  DEVICE_MAX_DMA_REGIONS is private
 * to device_master.c and this program must not learn it: the arms below are
 * written about how many slots came BACK compared with how many were taken,
 * and a test that hardcoded sixteen would start failing for the wrong reason
 * the day the table is resized.  This only bounds the arrays here.
 */
#define MAX_TRIES	64

#define REGION_BYTES	4096

/* How long the checker waits for the hog's slots: 500 x 10 ms. */
#define RECLAIM_TRIES	500
#define RECLAIM_WAIT_MS	10

static mach_port_t	device_port;

static int		n_pass;
static int		n_fail;

static void
arm(int n, const char *what, int ok)
{
	printf("dma_reclaim: [%d] %-56s %s\n", n, what, ok ? "PASS" : "FAIL");
	if (ok)
		n_pass++;
	else
		n_fail++;
}

/*
 * 🔴 END THE TASK, RATHER THAN RETURNING FROM main() AND HOPING.
 *
 * ⚠️ MEASURED, not assumed: with `return' here neither role's task ever
 * terminated.  The kernel hook this program exists to exercise was never
 * reached, the sixteen regions were still held at the end of the boot, and
 * bootstrap never printed `task terminated' for either instance.
 *
 * crt0 calls _threadlib_exit_routine() before exit(), which is pthread_exit(),
 * and a JOINABLE thread waits there on its own death futex for a joiner that,
 * for the main thread of a program nobody joins, does not exist.  _exit() is
 * task_terminate() and nothing else.
 *
 * 🔑 Which is the right dependency for this test in any case: what is being
 * measured is what the KERNEL does when a task dies, and routing that through
 * the thread library's opinion of how a C program ends would put libpthreads
 * inside the experiment.
 */
static void
die(void)
{
	_exit(0);
}

/* Take one slot.  Answers zero and leaves nothing behind on a refusal. */
static int
take_one(vm_address_t *kva, uint64_t *id)
{
	vm_address_t	v = 0, dma = 0;
	uint64_t	rid = 0;

	if (device_dma_alloc(device_port, DEVICE_DMA_NO_BDF, REGION_BYTES,
			     &v, &dma, &rid) != KERN_SUCCESS)
		return 0;

	*kva = v;
	*id = rid;
	return 1;
}

/*
 * Take slots until the kernel refuses.
 *
 * ⚠️ The refusal is the stop condition and not an error: the table is finite by
 * design, and running into its end is the only way to know it was full.
 */
static unsigned
fill_table(vm_address_t *kva, uint64_t *id, unsigned max)
{
	unsigned n = 0;

	while (n < max && take_one(&kva[n], &id[n]))
		n++;

	return n;
}

static void
free_table(const vm_address_t *kva, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++)
		(void) device_dma_free(device_port, DEVICE_DMA_NO_BDF,
				       kva[i], REGION_BYTES);
}

/*
 * Wait for the hog's slots to come back, and answer how many tries it took.
 * RECLAIM_TRIES means they never did.
 *
 * 🔑 The slot taken to find out is given straight back, so the count the
 * checker reports afterwards is the whole table and not the whole table minus
 * the one this probe was holding.
 */
static unsigned
wait_for_room(void)
{
	unsigned t;

	for (t = 0; t < RECLAIM_TRIES; t++) {
		vm_address_t	kva = 0;
		uint64_t	id = 0;

		if (take_one(&kva, &id)) {
			(void) device_dma_free(device_port, DEVICE_DMA_NO_BDF,
					       kva, REGION_BYTES);
			return t;
		}

		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT,
				     RECLAIM_WAIT_MS);
	}

	return RECLAIM_TRIES;
}

/* Strictly increasing, and therefore also all distinct. */
static int
ids_are_monotonic(const uint64_t *id, unsigned n)
{
	unsigned i;

	for (i = 1; i < n; i++)
		if (id[i] <= id[i - 1])
			return 0;

	return 1;
}

int
main(int argc, char **argv)
{
	vm_address_t	kva[MAX_TRIES];
	uint64_t	id[MAX_TRIES];
	unsigned	n, n2, waited;
	const char	*role;
	kern_return_t	kr;
	mach_port_t	host_port, wired, paged, security;

	kr = bootstrap_ports(bootstrap_port, &host_port, &device_port,
			     &wired, &paged, &security);
	if (kr != KERN_SUCCESS)
		_exit(1);	/* no console yet -- nothing to say it with */

	printf_init(device_port);

	role = (argc > 1 && argv[1] != 0) ? argv[1] : "hog";

	if (strcmp(role, "check") != 0) {
		/*
		 * The fixture, not a test.  It reports what it took so the
		 * checker's number can be read against a real one.
		 */
		n = fill_table(kva, id, MAX_TRIES);

		printf("\n=== DMA reclaim (#513): the task that dies ===\n");
		printf("dma_reclaim: the hog took %u region%s (ids %llu..%llu) "
		       "and is dying WITHOUT freeing them\n",
		       n, n == 1 ? "" : "s",
		       (unsigned long long)(n ? id[0] : 0),
		       (unsigned long long)(n ? id[n - 1] : 0));

		/*
		 * 🔴 RELEASE bootstrap ONLY NOW.  Its `-w' wait is what keeps
		 * the checker from being created while this table is still
		 * being filled -- which would let the checker take slots the
		 * hog was supposed to be holding, and measure the two tasks
		 * racing instead of the kernel reclaiming.
		 */
		(void) bootstrap_completed(bootstrap_port, mach_task_self());

		die();
	}

	printf("\n=== DMA reclaim (#513): the task that comes after ===\n");
	/*
	 * ⚠️ `started' and `N of M arms passed' are what run-x86_64.sh pairs to
	 * decide this program answered at all.  🔑 Only the CHECKER says them:
	 * the hog has no arms, and a `started' with nothing to finish it would
	 * make every boot look like a run that ended early.
	 */
	printf("dma_reclaim: started\n");

	/*
	 * 🔴 THE ARM.  This task was not created until the hog had filled the
	 * table and said so, and the hog frees nothing.  So room appearing here
	 * has exactly one possible cause: the hog died and the kernel took its
	 * regions back.
	 */
	waited = wait_for_room();
	arm(1, "a dead task's DMA regions are given back",
	    waited < RECLAIM_TRIES);

	if (waited >= RECLAIM_TRIES) {
		/*
		 * ⚠️ Said rather than skipped.  The remaining arms need a
		 * region to work with, so on a kernel without the reclaim they
		 * cannot run -- and a suite that silently printed fewer lines
		 * would be reporting a smaller failure than it found.
		 */
		printf("dma_reclaim:     waited %u ms and the table is still "
		       "the dead task's\n",
		       (unsigned)(RECLAIM_TRIES * RECLAIM_WAIT_MS));
		printf("dma_reclaim: [2] and [3] cannot run\n");
		n_fail += 2;
		printf("dma_reclaim: %d of %d arms passed\n",
		       n_pass, n_pass + n_fail);
		die();
	}

	n = fill_table(kva, id, MAX_TRIES);
	printf("dma_reclaim:     room after %u ms, and the table holds %u "
	       "region%s\n", waited * RECLAIM_WAIT_MS, n, n == 1 ? "" : "s");

	/*
	 * 🔑 THE SLOTS ARE REUSED AND THE NAMES ARE NOT, which is the property
	 * device_master.c calls "the whole of its safety": a region id that
	 * came round again would inherit every capability ever issued against
	 * the buffer that used to hold it.  The reclaim is exactly the event
	 * that could have got this wrong, because it is the one that frees
	 * slots without anybody asking.
	 *
	 * ⚠️ `id[0] > n' is the cross-task half and it does not need to know
	 * what the hog took: the hog took a whole table, so the counter has
	 * advanced by at least that many before the first id seen here.
	 */
	arm(2, "the recycled slots did not recycle their ids",
	    ids_are_monotonic(id, n) && id[0] > (uint64_t)n);
	printf("dma_reclaim:     ids %llu..%llu\n",
	       (unsigned long long)id[0], (unsigned long long)id[n - 1]);

	/*
	 * The control group.  If this is green and [1] is red, the table and
	 * its free path are working and the defect is in the death hook alone
	 * -- which is a different finding from "DMA allocation is broken", and
	 * the two are otherwise indistinguishable from one refused call.
	 */
	free_table(kva, n);
	n2 = fill_table(kva, id, MAX_TRIES);
	arm(3, "control: an explicit free returns the same slots", n2 == n);
	free_table(kva, n2);

	printf("dma_reclaim: %d of %d arms passed\n", n_pass, n_pass + n_fail);
	die();
	return 0;			/* not reached */
}
