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
 *	holder  take the region table AND a device, say so, and die still
 *	        holding both
 *	check   wait for them to come back, take them, and report
 *
 * 🔑 It is ONE binary because the two roles must fill the table by exactly
 * the same code.  A checker that allocated differently from the holder would be
 * comparing two measurements instead of taking one twice.
 *
 * ── The synchronisation, which is the hard part ───────────────────────
 *
 * 🔴 THE ORDER TASKS ARE CREATED IS NOT THE ORDER THEY RUN.  bootstrap creates
 * and resumes each entry in turn, but the first instruction of entry N can
 * follow the first instruction of entry N+1 -- see the note on SERVER_SERIALIZE_F
 * in servers/bootstrap/bootstrap.c, where netname_test lost exactly that race
 * in five boots out of five.  A checker that simply ran second could therefore
 * take the table BEFORE the holder and invert the whole experiment.
 *
 * So the two are joined at both ends:
 *
 *  1. the holder's line carries `-w', and the holder calls bootstrap_completed()
 *     AFTER it has filled the table.  bootstrap does not create the checker
 *     until that message arrives, so the checker is guaranteed to start with
 *     the table already full.  ⚠️ It is the message and not the death that is
 *     waited on: a `-w' entry that only dies wedges bootstrap, which is a
 *     defect of its own and not this program's to work around.
 *
 *  2. the checker then waits for ROOM.  It cannot appear except by the holder
 *     dying and the kernel taking its regions back, because nothing else in
 *     the bundle is running at that point and the holder frees nothing.
 *
 * ⚠️ The wait is BOUNDED.  On a kernel with no reclaim the room never comes,
 * and a test that waited for ever would report a hang instead of a failure.
 *
 * ── Reading the result ────────────────────────────────────────────────
 *
 * With the hook in place the checker takes the whole table.  With it removed
 * the checker takes NOTHING -- the holder still holds all sixteen slots -- so
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
#include <mach_init.h>			/* name_server_port */
#include <mach/cap_types.h>
#include <libcap.h>			/* cap_request */
#include <hal_state.h>			/* HAL_DEV_* */
#include "device_master.h"		/* the user half of device_master.defs */
#include "hal.h"			/* the user half of hal.defs */
#include <servers/netname.h>

/*
 * ⚠️ A CEILING AND NOT THE KERNEL'S NUMBER.  DEVICE_MAX_DMA_REGIONS is private
 * to device_master.c and this program must not learn it: the arms below are
 * written about how many slots came BACK compared with how many were taken,
 * and a test that hardcoded sixteen would start failing for the wrong reason
 * the day the table is resized.  This only bounds the arrays here.
 */
#define MAX_TRIES	64

#define REGION_BYTES	4096

/* How long the checker waits for the holder's slots: 500 x 10 ms. */
#define RECLAIM_TRIES	500
#define RECLAIM_WAIT_MS	10

/*
 * ── The device this program takes and abandons (#513) ────────────────
 *
 * 🔴 THE HOST BRIDGE, and that is a safety decision rather than a convenience.
 * What is being measured is a claim SURVIVING its owner's death, so the program
 * has to walk away from a device it holds.  Pick the disk controller and a
 * release that did not work takes the boot disk with it -- and a test that
 * breaks the mature suite when it fails is a test nobody keeps reading.
 *
 * 🔑 0:0.0 is on every board either target runs on, because that is where a
 * host bridge goes, and no driver will ever want it.  Its class is what the
 * manifest names; the kernel reads the same value out of the device's own
 * configuration space and requires the two to agree, so naming the wrong one
 * here fails loudly rather than claiming something else.
 */
#define BRIDGE_BDF	0u
#define BRIDGE_CLASS	0x060000ULL

/* Matches base class 6 -- bridges -- which is what the HAL notifies us about. */
#define BRIDGE_MASK	0xFF000000u
#define BRIDGE_MATCH	0x06000000u

/* Waiting for hal to check in: 100 x 100 ms, as block_device_server does. */
#define HAL_WAIT_TRIES	100
#define HAL_WAIT_MS	100

static mach_port_t	hal_port;
static mach_port_t	driver_port;

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
 * 🔴 END THE TASK, RATHER THAN RETURNING FROM main().
 *
 * ⚠️ This is how the defect in libpthreads was found, and the reason survives
 * it.  With `return' here neither role's task ever terminated: crt0 calls
 * _threadlib_exit_routine() before exit(), that is pthread_exit(), and a
 * JOINABLE thread waited there on its own death futex for a joiner that the
 * main thread of a program nobody joins does not have.  The kernel hook this
 * program exists to exercise was never reached and the sixteen regions were
 * still held at the end of the boot.
 *
 * pthread_exit() ends the process when it is the last thread now, so `return'
 * would work.  This stays because it is the right dependency for THIS test:
 * what is being measured is what the KERNEL does when a task dies, and routing
 * that through the thread library's opinion of how a C program ends would put
 * libpthreads inside the experiment -- and this program is the only thing that
 * would notice if it broke again.
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
 * Wait for the holder's slots to come back, and answer how many tries it took.
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


/*
 * ── Becoming a driver for long enough to die as one (#513) ───────────
 *
 * 🔑 THE HAL RELEASES A DEVICE WHEN THE PORT THAT BOUND IT DIES, so a witness
 * for that has to be a task that registers, binds, and goes.  block_device_server
 * binds two controllers on every boot and never dies, which is why the release
 * path had no arm before this.
 *
 * ⚠️ Bounded, like block_device_server's own wait: hal is STARTED before this
 * program but starting is not registering, and a lookup that loses that race
 * must report rather than spin (#460).
 */
static int
become_a_driver(void)
{
	kern_return_t	kr;
	int		tries;

	/*
	 * 🔴 EVERY STEP SAYS IT IS ABOUT TO HAPPEN, and that is not decoration.
	 * This program's line carries `-w', so bootstrap does not go on until it
	 * says it is done -- which means any call here that does not return stops
	 * the whole boot.  Three of them are RPCs to other servers, and a MIG RPC
	 * has no timeout.
	 *
	 * ⚠️ A boot was caught stopping exactly here, with the header printed and
	 * nothing after it, and the log could not say which call it was in.  A
	 * fixture whose failure is fatal has to narrate.
	 */
	printf("dma_reclaim: looking the HAL up\n");

	for (tries = 0; ; tries++) {
		kr = netname_look_up(name_server_port, "", "hal", &hal_port);
		if (kr == NETNAME_SUCCESS)
			break;
		if (tries >= HAL_WAIT_TRIES) {
			printf("dma_reclaim: the HAL never checked in\n");
			return 0;
		}
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT,
				     HAL_WAIT_MS);
	}

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			       &driver_port) != KERN_SUCCESS)
		return 0;

	if (mach_port_insert_right(mach_task_self(), driver_port, driver_port,
				   MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS)
		return 0;

	printf("dma_reclaim: registering with the HAL\n");

	kr = hal_register_driver(hal_port, BRIDGE_MASK, BRIDGE_MATCH,
				 driver_port);
	if (kr != KERN_SUCCESS) {
		printf("dma_reclaim: hal_register_driver failed (kr=%d)\n",
		       (int)kr);
		return 0;
	}

	return 1;
}

/*
 * Take the host bridge from the kernel, the way a driver does: a capability for
 * the CLASS out of this program's manifest, and a claim on the instance.
 */
static int
claim_the_bridge(void)
{
	struct uros_cap	tok;
	kern_return_t	kr;

	memset(&tok, 0, sizeof(tok));

	printf("dma_reclaim: asking cap_server for class 0x%06lx\n",
	       (unsigned long)BRIDGE_CLASS);

	kr = cap_request(RESOURCE_PCI_DEVICE, BRIDGE_CLASS,
			 CAP_OP_PCI_DMA_MAP | CAP_OP_PCI_MMIO_MAP
			 | CAP_OP_PCI_IRQ, 0, &tok);
	if (kr != KERN_SUCCESS) {
		printf("dma_reclaim: cap_server would not issue a capability "
		       "for class 0x%06lx (kr=%d)\n",
		       (unsigned long)BRIDGE_CLASS, (int)kr);
		return 0;
	}

	printf("dma_reclaim: claiming 0:0.0 from the kernel\n");

	kr = device_claim(device_port, BRIDGE_BDF, (char *)&tok, sizeof(tok));
	if (kr != KERN_SUCCESS) {
		printf("dma_reclaim: the kernel refused the claim on 0:0.0 "
		       "(kr=%d)\n", (int)kr);
		return 0;
	}

	return 1;
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

	role = (argc > 1 && argv[1] != 0) ? argv[1] : "holder";

	if (strcmp(role, "check") != 0) {
		/*
		 * The fixture, not a test.  It reports what it took so the
		 * checker's number can be read against a real one.
		 */
		n = fill_table(kva, id, MAX_TRIES);

		printf("\n=== DMA reclaim (#513): the task that dies ===\n");

		/*
		 * The device half.  A failure at any step is reported and the
		 * program carries on: the buffer arms below do not depend on
		 * it, and half a fixture is better than a boot that stops.
		 */
		if (become_a_driver() && claim_the_bridge()) {
			kern_return_t rkr;

			printf("dma_reclaim: reporting the bind to the HAL\n");

			rkr = hal_report_probe(hal_port, 0, 0, 0, driver_port,
					       HAL_DEV_BOUND);
			printf("dma_reclaim: the holder holds 0:0.0 and told the "
			       "HAL so (kr=%d) — it is about to die still "
			       "holding it\n", (int)rkr);
		}

		printf("dma_reclaim: the holder took %u region%s (ids %llu..%llu) "
		       "and is dying WITHOUT freeing them\n",
		       n, n == 1 ? "" : "s",
		       (unsigned long long)(n ? id[0] : 0),
		       (unsigned long long)(n ? id[n - 1] : 0));

		/*
		 * 🔴 RELEASE bootstrap ONLY NOW.  Its `-w' wait is what keeps
		 * the checker from being created while this table is still
		 * being filled -- which would let the checker take slots the
		 * holder was supposed to be holding, and measure the two tasks
		 * racing instead of the kernel reclaiming.
		 */
		printf("dma_reclaim: releasing bootstrap\n");
		(void) bootstrap_completed(bootstrap_port, mach_task_self());

		die();
	}

	printf("\n=== DMA reclaim (#513): the task that comes after ===\n");
	/*
	 * ⚠️ `started' and `N of M arms passed' are what run-x86_64.sh pairs to
	 * decide this program answered at all.  🔑 Only the CHECKER says them:
	 * the holder has no arms, and a `started' with nothing to finish it would
	 * make every boot look like a run that ended early.
	 */
	printf("dma_reclaim: started\n");

	/*
	 * 🔴 THE ARM.  This task was not created until the holder had filled the
	 * table and said so, and the holder frees nothing.  So room appearing here
	 * has exactly one possible cause: the holder died and the kernel took its
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
	 * what the holder took: it took a whole table, so the counter has
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

	/*
	 * ── The device half, which is the other thing a dead driver held ──
	 *
	 * 🔴 ARM [5] IS THE PRESENCE AND ARM [4] LEANS ON IT.  "The HAL says
	 * 0:0.0 is unclaimed" is also what it says about a device nobody ever
	 * bound, so on its own that reading proves nothing -- what makes it
	 * evidence is that the HAL printed `0:0.0 is bound to a driver' earlier
	 * in this same boot, when the holder reported it.  A field that read BOUND
	 * and now reads UNCLAIMED has been released.
	 *
	 * Arm [5] needs no such argument: claiming a device is refused while
	 * anybody else holds it, so succeeding here is only possible because
	 * the kernel let go of a claim whose owner died.  That is the box this
	 * issue could not tick from the buffer arms alone.
	 */
	if (become_a_driver()) {
		unsigned int state = (unsigned int)-1;
		unsigned int t;

		/*
		 * ⚠️ WAITED FOR, not read once.  The HAL learns of the death
		 * from a dead-name notification, which is a message it handles
		 * when it gets to it -- so a single read here is a race with
		 * the HAL's own message loop, and it passed by luck the first
		 * time it was run.  Bounded, so a HAL that never releases the
		 * device fails the arm instead of hanging the boot.
		 */
		for (t = 0; t < RECLAIM_TRIES; t++) {
			if (hal_get_device_state(hal_port, 0, 0, 0, &state)
			    == KERN_SUCCESS && state == HAL_DEV_UNCLAIMED)
				break;
			(void) thread_switch(MACH_PORT_NULL,
					     SWITCH_OPTION_WAIT,
					     RECLAIM_WAIT_MS);
		}

		arm(4, "the HAL released the dead driver's device",
		    state == HAL_DEV_UNCLAIMED);

		/*
		 * 🔴 [5] IS THREE OBSERVATIONS AND THE FIRST ONE IS THE POINT.
		 *
		 * ⚠️ "Claiming it works" does NOT discriminate, and the
		 * ablation said so: with the release removed the arm still
		 * passed.  ds_master_device_claim answers SUCCESS when the
		 * existing entry's task is the caller -- and the dead holder's
		 * task_t was recycled into this one, so the stale entry
		 * compared EQUAL.  The arm was passing by demonstrating the
		 * defect it was written to catch.
		 *
		 * So ask the question a false match cannot answer the same
		 * way.  check_claim() refuses a DMA buffer for a device this
		 * task does not hold, and refuses one for an UNCLAIMED device
		 * too, so before claiming:
		 *
		 *	released    no entry     -> refused
		 *	inherited   entry == me  -> granted
		 *
		 * Then the claim, and then a buffer the claim makes allowed.
		 * A refusal that turns into a grant across one claim is a
		 * transition this task caused, and neither half alone is.
		 */
		vm_address_t	kva = 0, dma = 0;
		uint64_t	rid = 0;
		int		refused_before, claimed, allowed_after;

		refused_before = device_dma_alloc(device_port, BRIDGE_BDF,
						  REGION_BYTES, &kva, &dma,
						  &rid) != KERN_SUCCESS;
		if (!refused_before)
			(void) device_dma_free(device_port, BRIDGE_BDF, kva,
					       REGION_BYTES);

		claimed = claim_the_bridge();

		allowed_after = device_dma_alloc(device_port, BRIDGE_BDF,
						 REGION_BYTES, &kva, &dma,
						 &rid) == KERN_SUCCESS;
		if (allowed_after)
			(void) device_dma_free(device_port, BRIDGE_BDF, kva,
					       REGION_BYTES);

		arm(5, "the kernel let go, and this task had not inherited it",
		    refused_before && claimed && allowed_after);
		printf("dma_reclaim:     0:0.0 before the claim: %s; claim: "
		       "%s; after: %s\n",
		       refused_before ? "refused" : "GRANTED — inherited",
		       claimed ? "taken" : "refused",
		       allowed_after ? "allowed" : "refused");
	} else {
		printf("dma_reclaim: [4] and [5] cannot run: no HAL\n");
		n_fail += 2;
	}

	printf("dma_reclaim: %d of %d arms passed\n", n_pass, n_pass + n_fail);
	die();
	return 0;			/* not reached */
}
