/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Claiming an interrupt line and giving it back, from where a driver stands
 * (#457).
 *
 * ── Why this exists ───────────────────────────────────────────────────
 *
 * The kernel's own boot self-test proves the machine-dependent half on
 * x86-64: a vector claimed, a pin routed, an interrupt delivered, deferred
 * and replayed.  It cannot prove the half above it, because it does not go
 * through the RPC and does not have a task to be refused as.
 *
 * And it cannot see the defect this was written for at all, because that one
 * is on the OTHER machine.
 *
 * ── 🔴 The defect ─────────────────────────────────────────────────────
 *
 * ds_master_device_intr_unregister() used to restore the displaced handler
 * with take_irq() alone.  take_irq() installs only into a line whose
 * intpri[] is zero, and otherwise prints "This device will clobber IRQ %d"
 * and spins for ever -- deliberately, because two devices on one line is a
 * wiring fault it cannot resolve.  But intpri[] for that line is not zero:
 * the matching register set it, to SPL6, a moment earlier.  So the restore
 * took the else branch every time, and giving an interrupt back HALTED THE
 * MACHINE -- from a privileged RPC, which means one call from a driver.
 *
 * ⚠️ It reads as though it could not, because take_irq() is guarded by
 * mp_v1_1_take_irq(), which would claim the line itself and answer TRUE.
 * The build compiles i386/AT386/mp/mp_stub.c and not mp_v1_1.c, and the stub
 * declines by contract -- so the body runs.  Asked of the build rather than
 * assumed: MP_V1_1 arrives as -DMP_V1_1=1 on the command line while the
 * generated header says 0.
 *
 * Nothing exercised it.  char_server has the call, in char_core_irq_unregister(),
 * reached from a module's detach() -- which core.c calls only when the device
 * table is full.  A hang one table entry away from every boot.
 *
 * So the pass/fail here is unusually blunt: BEFORE the fix this program does
 * not print its second line, because the kernel it is talking to has stopped.
 *
 * ── The line, and why this one ────────────────────────────────────────
 *
 * IRQ 5.  Free on the machine these tests boot -- no sound card, no second
 * parallel port, and the PCI devices land on 10 and 11 -- which is what makes
 * a delivered interrupt here evidence rather than noise.  ⚠️ Not assumed:
 * arm [4] reports what arrived, and zero is the control.
 *
 * ⚠️ NOT IRQ 13, which was the first choice because it was one of only two
 * lines the i386 kernel claimed with a non-zero priority, and so the only way
 * to reach the "restore what was displaced" branch from here.  It could not be
 * held still: it was also the FPU error line, and with CR0_NE never set a
 * numeric error arrived on that pin as FERR# -- a line that can fire on its
 * own makes the count in arm [4] a measurement of something this program does
 * not control.
 *
 * 🔴 THAT REASON IS GONE AND THE CONCLUSION IS NOT (#515).  CR0.NE is armed
 * now, so nothing asserts FERR# and IRQ 13 cannot fire on its own any more --
 * but the same change set intpri[13] to 0, because a line no handler serves
 * must not be left unmasked.  So it is no longer a line with a non-zero
 * priority either, and therefore no longer a route to that branch.  The reason
 * to avoid it changed completely; the choice of IRQ 5 did not.
 *
 * ⚠️ Which leaves IRQ 1, the keyboard at SPL6, as the only line the kernel
 * still claims with a priority -- and the displaced-handler branch still NOT
 * covered here.  Saying so is better than covering it with a line the test
 * cannot hold still.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/message.h>
#include <mach/port.h>

/*
 * ⚠️ bootstrap.h and not mach.h: bootstrap_ports() is generated from
 * bootstrap.defs and the umbrella header does not pull that half in.  Without
 * it the call compiles as an implicit declaration returning int -- six
 * arguments handed to a prototype nobody checked, which on this target is how
 * a port ends up half read.
 */
#include <mach/bootstrap.h>

#include <stdio.h>

#include "device_master.h"
#include "hal_server.h"		/* struct hal_device_info (#511) */
#include "hal.h"		/* the user half of hal.defs (#511) */
#include <mach/cap_types.h>	/* RESOURCE_PCI_DEVICE, CAP_OP_PCI_IRQ (#511) */
#include <libcap.h>		/* cap_request (#511) */
#include <mach_init.h>		/* name_server_port */
#include <mach/thread_switch.h>
#include <mach/clock.h>		/* host_get_clock_service, clock_get_time (#747) */
#include <mach/clock_types.h>	/* URMACH_CAP_CLOCK, NSEC_PER_SEC (#747) */
#include <servers/netname.h>
#include <string.h>

/*
 * The line under test, and how many times the round trip is made.
 *
 * More than once because "unregister returned" and "unregister released" are
 * different claims: a call that returned without giving the line up would let
 * the first registration through and refuse the second.  Ten is enough to
 * separate them and cheap enough not to matter under emulation.
 */
#define	TEST_IRQ		5u
#define	ROUND_TRIPS		10

/*
 * A line number the kernel must refuse.
 *
 * Sixteen is one past the last LINE, rather than an absurd value: the bound
 * is what is being checked, and an absurd value would pass a kernel that only
 * rejects absurd values.
 *
 * ⚠️ It is not one past the last SLOT.  The forwarding table is thirty-two
 * entries as of #457 and the upper half is message-signalled interrupts --
 * which the kernel hands out and a driver may not name.  Widening the table
 * without narrowing this check would have let a driver claim slot 16 as
 * though it were a line, and this arm is what says it cannot.
 */
#define	OUT_OF_RANGE_IRQ	16u

/*
 * ⚠️ NOT static any more, and not by choice: <hal_server.h> declares a
 * `master_device' of its own -- the HAL's, extern -- and this file now
 * includes it in order to learn which device it may own.  Two objects of the
 * same name where one was private is a collision the linker would have
 * resolved silently in one direction or the other; the compiler catches it
 * only because one of them is static.
 */
mach_port_t	master_device;

/*
 * A receive right to be told about, and a send right to hand over.
 *
 * 🔴 Released on every exit from every arm.  A test that leaks the rights it
 * asked for is not measuring the kernel it is running on for very long, and
 * this one asks for a new pair on each of the ten round trips.
 */
static int
claim(unsigned int irq, mach_port_t *port_out, kern_return_t *kr_out)
{
	mach_port_t	port;
	kern_return_t	kr;

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&port);
	if (kr != KERN_SUCCESS) {
		printf("irq_claim_test: cannot allocate a port: 0x%x\n", kr);
		return -1;
	}

	/*
	 * ⚠️ The last argument is the DISPOSITION, and MAKE_SEND is what makes
	 * a receive right into the send right the kernel keeps -- so no
	 * mach_port_insert_right() is needed and none is done.  Handing a name
	 * without saying what to do with it is how a port arrives as a name
	 * that means nothing on the other side.
	 */
	*kr_out = device_intr_register(master_device, irq, port,
				       MACH_MSG_TYPE_MAKE_SEND);

	if (*kr_out != KERN_SUCCESS) {
		(void) mach_port_destroy(mach_task_self(), port);
		*port_out = MACH_PORT_NULL;
		return 0;
	}

	*port_out = port;
	return 0;
}

static void
release(mach_port_t port)
{
	if (port != MACH_PORT_NULL)
		(void) mach_port_destroy(mach_task_self(), port);
}

/*
 * [1] and [2]: the claim, and the release that used to be a halt.
 */
static int
arm_one_claim_and_release(void)
{
	mach_port_t	port;
	kern_return_t	kr;

	if (claim(TEST_IRQ, &port, &kr) < 0)
		return 0;

	printf("irq_claim_test: [1] claiming irq %u answered 0x%x — %s\n",
	       TEST_IRQ, kr,
	       kr == KERN_SUCCESS
	       ? "the line is this task's"
	       : "WRONG, a free line was refused");
	if (kr != KERN_SUCCESS)
		return 0;

	/*
	 * ⚠️ The next line is the whole arm.  Before the fix the kernel does
	 * not come back from this call, so what fails is not a comparison --
	 * it is that nothing further is ever printed.
	 */
	kr = device_intr_unregister(master_device, TEST_IRQ);
	release(port);

	printf("irq_claim_test: [2] giving irq %u back answered 0x%x — %s\n",
	       TEST_IRQ, kr,
	       kr == KERN_SUCCESS
	       ? "the kernel returned, which is the whole claim"
	       : "WRONG, the release was refused");

	/* Two, because this arm answers two of the six lines. */
	return kr == KERN_SUCCESS ? 2 : 1;
}

/*
 * [3]: and the line is genuinely free afterwards.
 *
 * 🔑 Returning is not releasing.  A kernel that answered KERN_SUCCESS and
 * left the line claimed would pass arm [2] and fail here on the second
 * attempt, because register refuses a line that is already registered.
 */
static int
arm_two_round_trips(void)
{
	mach_port_t	port;
	kern_return_t	kr;
	int		completed = 0;

	for (int i = 0; i < ROUND_TRIPS; i++) {
		if (claim(TEST_IRQ, &port, &kr) < 0)
			break;
		if (kr != KERN_SUCCESS)
			break;
		kr = device_intr_unregister(master_device, TEST_IRQ);
		release(port);
		if (kr != KERN_SUCCESS)
			break;
		completed++;
	}

	printf("irq_claim_test: [3] %d of %d round trips on irq %u — %s\n",
	       completed, ROUND_TRIPS, TEST_IRQ,
	       completed == ROUND_TRIPS
	       ? "the release puts the line back, not just the table entry"
	       : "WRONG, the line does not come back");

	return completed == ROUND_TRIPS;
}

/*
 * [4]: what the line delivered while this task held it.
 *
 * The control for the three above: they all speak about a line nothing
 * drives, and that has to be shown rather than believed.  A message here
 * means IRQ 5 is not free on this machine and the arms above were reporting
 * about a line under someone else's influence.
 *
 * ⚠️ A receive with no timeout would wait for ever for a message that is not
 * coming, which is the point.  MACH_RCV_TIMEOUT with zero is the only shape
 * that can say "nothing, and I looked".
 */
static int
arm_three_nothing_was_delivered(void)
{
	mach_port_t		port;
	kern_return_t		kr;
	mach_msg_header_t	msg;
	kern_return_t		rcv;

	if (claim(TEST_IRQ, &port, &kr) < 0 || kr != KERN_SUCCESS) {
		printf("irq_claim_test: [4] could not hold irq %u to watch it"
		       " — WRONG\n", TEST_IRQ);
		return 0;
	}

	rcv = mach_msg(&msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
		       0, sizeof(msg), port, 200, MACH_PORT_NULL);

	(void) device_intr_unregister(master_device, TEST_IRQ);
	release(port);

	printf("irq_claim_test: [4] holding irq %u for 200 ms, the receive"
	       " answered 0x%x — %s\n",
	       TEST_IRQ, rcv,
	       rcv == MACH_RCV_TIMED_OUT
	       ? "nothing drives this line, so the arms above are about it alone"
	       : "WRONG, something is on this line and the arms above are noisy");

	return rcv == MACH_RCV_TIMED_OUT;
}

/*
 * [5] and [6]: the two things the contract says it refuses.
 *
 * Worth asking because both are answers rather than crashes, and an interface
 * that accepted either would be one a driver could rely on until the day the
 * line it was given did not exist.
 */
static int
arm_four_what_is_refused(void)
{
	mach_port_t	first, second;
	kern_return_t	kr_range, kr_first, kr_second;
	int		passed = 0;

	if (claim(OUT_OF_RANGE_IRQ, &first, &kr_range) < 0)
		return 0;
	release(first);

	printf("irq_claim_test: [5] irq %u, one past the last, answered 0x%x"
	       " — %s\n",
	       OUT_OF_RANGE_IRQ, kr_range,
	       kr_range == KERN_INVALID_ARGUMENT
	       ? "the bound is checked at the edge"
	       : "WRONG, a line that does not exist was accepted");
	passed += kr_range == KERN_INVALID_ARGUMENT;

	if (claim(TEST_IRQ, &first, &kr_first) < 0)
		return passed;
	if (kr_first != KERN_SUCCESS) {
		release(first);
		return passed;
	}

	if (claim(TEST_IRQ, &second, &kr_second) < 0) {
		(void) device_intr_unregister(master_device, TEST_IRQ);
		release(first);
		return passed;
	}
	release(second);

	printf("irq_claim_test: [6] claiming irq %u a second time answered"
	       " 0x%x — %s\n",
	       TEST_IRQ, kr_second,
	       kr_second != KERN_SUCCESS
	       ? "one handler per line, as the contract says"
	       : "WRONG, two owners were given the same line");
	passed += kr_second != KERN_SUCCESS;

	(void) device_intr_unregister(master_device, TEST_IRQ);
	release(first);

	return passed;
}

/*
 * [7] and [8]: an interrupt with no wire behind it, asked for from here.
 *
 * 🔑 The point of the call is what it does NOT take: there is no address in
 * it anywhere.  An MSI-X table entry holds an address the DEVICE writes to,
 * so a driver that could fill one in could aim the device at page zero or at
 * another task's pages, and no fault would be raised because the write is the
 * device's.  This says which device and which entry of its table; the kernel
 * finds the capability, allocates a vector, decides the address and the value
 * and writes them there.
 *
 * ⚠️ Every device is asked, rather than one being named.  Which device has
 * MSI-X is a property of the machine -- QEMU puts an 82574L on a Q35 and an
 * 82540EM on the default board, and only the first has a table -- while THAT
 * THE COUNT MATCHES THE BOARD is a property of the kernel.  Naming a device
 * would have made this a test of QEMU's defaults.
 */
#define IQ_NET_CLASS	0x020000u
#define IQ_HAL_TRIES	200

/*
 * ── The device this test owns, named by the HAL (#511) ───────────────────
 *
 * 🔑 A PROGRAM CANNOT FIND ITS OWN DEVICE ANY MORE, and that is not an
 * obstacle put in its way -- it is the point.  Reading configuration space
 * is something only a device's owner or the bus scanner may do, so
 * discovering what to own by reading it would be the circle #511 exists to
 * break.  The HAL enumerates; everybody else is told.
 */
/*
 * The owned card's first memory region, kept for arms [9] and [10] (#508):
 * zero size when the card was not owned or has none.
 */
static struct pci_bar_region	nic_region;
static natural_t		nic_bdf;	/* the owned card, for [11] (#598) */
static int			nic_owned;
static unsigned int		nic_class;	/* its class, for [13]'s tokens */
static uint64_t			nic_cap_id;	/* the claim's token, for [18] (#745) */

static int
own_the_network_card(natural_t *bdf_out)
{
	mach_port_t		hal_port = MACH_PORT_NULL;
	vm_offset_t		buf = 0;
	mach_msg_type_number_t	bytes = 0;
	unsigned int		n = 0, i;
	const struct hal_device_info *devs;
	struct uros_cap		tok;
	kern_return_t		kr;
	natural_t		bdf = 0;
	unsigned int		class_rev = 0;
	int			found = 0, tries;

	for (tries = 0; tries < IQ_HAL_TRIES; tries++) {
		if (netname_look_up(name_server_port, "", (char *)"hal",
				    &hal_port) == KERN_SUCCESS)
			break;
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 1);
	}
	if (hal_port == MACH_PORT_NULL) {
		printf("irq_claim_test: the HAL never appeared\n");
		return 0;
	}

	if (hal_list_devices(hal_port, &buf, &bytes, &n) != KERN_SUCCESS) {
		printf("irq_claim_test: hal_list_devices refused\n");
		return 0;
	}

	devs = (const struct hal_device_info *)buf;
	for (i = 0; i < n; i++)
		if ((devs[i].class_rev >> 8) == IQ_NET_CLASS) {
			unsigned int b;

			bdf = (natural_t)((devs[i].bus << 8)
					  | (devs[i].slot << 3) | devs[i].func);
			class_rev = devs[i].class_rev;
			found = 1;
			for (b = 0; b < devs[i].n_bars && b < HAL_MAX_BARS; b++)
				if ((devs[i].bars[b].flags & PCI_REGION_IO) == 0
				    && devs[i].bars[b].size != 0) {
					nic_region = devs[i].bars[b];
					break;
				}
			break;
		}

	if (bytes != 0)
		(void) vm_deallocate(mach_task_self(), buf, bytes);
	if (!found) {
		printf("irq_claim_test: the HAL listed %u device(s) and none "
		       "of class 0x%06x\n", n, IQ_NET_CLASS);
		return 0;
	}

	/*
	 * All three: [5] asks the card for a message-signalled interrupt, [9]
	 * to [12] map its registers, and [13] allocates DMA for it before the
	 * claim gives the operations up one at a time.  This asked for IRQ
	 * alone while [9] to [12] mapped the registers, which worked only
	 * because the kernel did not look at what a claim's capability
	 * carried (#621).
	 */
	nic_class = class_rev >> 8;
	memset(&tok, 0, sizeof(tok));
	kr = cap_request(RESOURCE_PCI_DEVICE, (uint64_t)nic_class,
			 CAP_OP_PCI_DMA_MAP | CAP_OP_PCI_MMIO_MAP
			 | CAP_OP_PCI_IRQ, 0, &tok);
	if (kr != KERN_SUCCESS) {
		printf("irq_claim_test: cap_server would not issue a "
		       "capability for class 0x%06x (kr=%d)\n",
		       class_rev >> 8, (int)kr);
		return 0;
	}

	kr = device_claim(master_device, bdf, (char *)&tok, sizeof(tok));
	if (kr != KERN_SUCCESS) {
		printf("irq_claim_test: the kernel refused the claim on "
		       "%u:%u.%u (kr=%d)\n", (unsigned)(bdf >> 8),
		       (unsigned)((bdf >> 3) & 0x1F), (unsigned)(bdf & 0x7),
		       (int)kr);
		return 0;
	}

	/*
	 * The token the claim rests on: a capability presented again only
	 * narrows the claim and is not recorded, so this is the one whose
	 * revocation ends it ([18], #745).
	 */
	nic_cap_id = tok.cap_id;
	*bdf_out = bdf;
	return 1;
}

/*
 * ⚠️ THIS USED TO SWEEP ALL THIRTY-TWO SLOTS, asking each for a
 * message-signalled interrupt, and the comment above it explained that
 * naming a device would have made this a test of QEMU's defaults.  That was
 * right about the reasoning and wrong about the authority: an MSI is
 * programmed into the DEVICE's own table, so the sweep was writing to
 * hardware belonging to other servers.
 *
 * 🔑 So it asks the one device it OWNS.  What it loses is the count across
 * the board; what it keeps is the thing the count was evidence for -- that a
 * slot the kernel chose comes back above the sixteen a driver can name, and
 * that the same unregister releases both kinds.
 */
static int
arm_five_an_interrupt_with_no_wire(void)
{
	mach_port_t	port;
	kern_return_t	kr;
	unsigned int	slot = 0;
	natural_t	bdf = 0;
	int		released_ok = 1;

	if (own_the_network_card(&bdf)) {
		nic_bdf = bdf;
		nic_owned = 1;
	} else {
		printf("irq_claim_test: [7] DID NOT RUN — no device of class "
		       "0x%06x this task may own, so a message-signalled "
		       "interrupt cannot be asked for without writing to "
		       "somebody else's hardware\n", IQ_NET_CLASS);
		printf("irq_claim_test: [8] DID NOT RUN — nothing was taken, "
		       "so there is nothing to give back\n");
		return 2;
	}

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&port);
	if (kr != KERN_SUCCESS)
		return 0;

	kr = device_msi_register(master_device, (bdf >> 8), (bdf >> 3) & 0x1F,
				 bdf & 0x7, 0, port,
				 MACH_MSG_TYPE_MAKE_SEND, &slot);
	if (kr != KERN_SUCCESS) {
		printf("irq_claim_test: [7] %u:%u.%u, which this task owns, "
		       "refused a message-signalled interrupt (kr=%d) — which "
		       "is what a board with no MSI-X table should say\n",
		       (unsigned)(bdf >> 8), (unsigned)((bdf >> 3) & 0x1F),
		       (unsigned)(bdf & 0x7), (int)kr);
		printf("irq_claim_test: [8] DID NOT RUN — nothing was taken, "
		       "so there is nothing to give back\n");
		release(port);
		return 2;
	}

	if (device_intr_unregister(master_device, slot) != KERN_SUCCESS)
		released_ok = 0;
	release(port);

	printf("irq_claim_test: [7] %u:%u.%u, which this task owns, accepted a"
	       " message-signalled interrupt at slot %u — %s\n",
	       (unsigned)(bdf >> 8), (unsigned)((bdf >> 3) & 0x1F),
	       (unsigned)(bdf & 0x7), slot,
	       slot >= 16
	       ? "a slot the KERNEL chose, above the sixteen a driver can name"
	       : "WRONG, that is a line number and a line was not asked for");

	printf("irq_claim_test: [8] giving it back answered %s — %s\n",
	       released_ok ? "success" : "a failure",
	       released_ok
	       ? "the same unregister works for both kinds of slot"
	       : "WRONG, a message-signalled slot cannot be released");

	return (released_ok && slot >= 16) ? 2 : 0;
}

/*
 * [9] and [10]: a mapping ends where its region ends (#508).
 *
 * The kernel used to check only the page a mapping started on, and a task
 * holding one BAR could ask for enough pages to run past it -- on these
 * boards into the I/O APIC, the HPET and the local APIC, which sit just
 * above the PCI windows.  So from the card this task owns: the whole memory
 * region maps, and the same base asked for one page more is refused with
 * KERN_NO_ACCESS.  Nothing is read or written through either mapping.
 */
static int
arm_six_a_mapping_ends_with_its_region(void)
{
	vm_address_t	uva = 0;
	uint64_t	page = 4096, whole;
	kern_return_t	kr;
	int		ok = 0;

	if (nic_region.size == 0) {
		printf("irq_claim_test: [9] DID NOT RUN — no owned card with a "
		       "memory region to map\n");
		printf("irq_claim_test: [10] DID NOT RUN — so nothing to run "
		       "past\n");
		return 0;
	}

	whole = (nic_region.size + page - 1) & ~(page - 1);

	kr = device_mmio_map(master_device, (vm_address_t)nic_region.base,
			     (vm_size_t)whole, mach_task_self(), &uva);
	if (kr == KERN_SUCCESS) {
		(void) device_mmio_unmap(master_device, uva, (vm_size_t)whole,
					 mach_task_self());
		printf("irq_claim_test: [9] 0x%llx..0x%llx, the whole of the "
		       "owned card's memory region, maps and is given back\n",
		       (unsigned long long)nic_region.base,
		       (unsigned long long)(nic_region.base + whole - 1));
		ok++;
	} else
		printf("irq_claim_test: [9] WRONG — the owned card's own "
		       "memory region 0x%llx (+0x%llx) would not map "
		       "(kr=%d)\n", (unsigned long long)nic_region.base,
		       (unsigned long long)whole, (int)kr);

	uva = 0;
	kr = device_mmio_map(master_device, (vm_address_t)nic_region.base,
			     (vm_size_t)(whole + page), mach_task_self(), &uva);
	if (kr == KERN_NO_ACCESS) {
		printf("irq_claim_test: [10] one page more, to 0x%llx, refused "
		       "with KERN_NO_ACCESS — a mapping ends where its region "
		       "does (#508)\n",
		       (unsigned long long)(nic_region.base + whole + page - 1));
		ok++;
	} else {
		if (kr == KERN_SUCCESS)
			(void) device_mmio_unmap(master_device, uva,
						 (vm_size_t)(whole + page),
						 mach_task_self());
		printf("irq_claim_test: [10] WRONG — a mapping one page past "
		       "the owned card's region answered kr=%d, and a task that "
		       "holds one BAR can reach whatever lies above it\n",
		       (int)kr);
	}

	return ok;
}

/*
 * [11] and [12]: the owned card rings, and the ring arrives (#598).
 *
 * [7] asks for a message-signalled interrupt and gives it back, which shows
 * that the kernel writes the card's table entry -- and, with interrupts
 * remapped, the remapping entry -- but not that a message the CARD sends
 * reaches anybody.  Here the card sends one.  An 82574L raises its "other"
 * causes on the MSI-X entry its IVAR register names for them, and a link
 * status change written into ICS is one of those causes (QEMU's model of it:
 * e1000e_raise_interrupts() and e1000e_msix_notify() in hw/net/e1000e_core.c).
 * So: every cause masked and cleared, entry 0 asked for, IVAR's "other" field
 * pointed at it, the port looked at once and found quiet -- then LSC
 * unmasked and set, and the slot's notification must come.
 *
 * 🔑 With interrupts remapped (-i) the message goes through the entry the
 * kernel wrote for this slot, which names this card as its only source: an
 * entry missing, or naming another function, is refused, and this is the
 * arm that notices.
 */
#define	E1000_ICR		0x000C0u	/* cause read; a written 1 clears */
#define	E1000_ICS		0x000C8u	/* cause set                      */
#define	E1000_IMS		0x000D0u	/* mask set                       */
#define	E1000_IMC		0x000D8u	/* mask clear                     */
#define	E1000_IVAR		0x000E4u	/* the MSI-X entry of each cause  */
#define	E1000_ICR_LSC		0x00000004u
#define	E1000_ICR_OTHER		0x01000000u
#define	E1000_IVAR_OTHER_ENTRY0	(0x8u << 16)	/* "other" causes: entry 0, valid */
#define	IQ_NOTIFY_BASE		3000		/* <device/device_master.h> */
#define	IQ_PAGE			4096u

static void
nic_write(volatile uint32_t *regs, unsigned int off, uint32_t value)
{
	regs[off / 4] = value;
}

static int
arm_seven_the_card_rings(void)
{
	volatile uint32_t	*regs;
	vm_address_t		uva = 0;
	mach_port_t		port = MACH_PORT_NULL;
	struct {
		mach_msg_header_t	h;
		mach_msg_trailer_t	t;
	}			msg;
	kern_return_t		kr, quiet, rang;
	unsigned int		slot = 0;
	int			ok = 0;

	if (!nic_owned || nic_region.size == 0) {
		printf("irq_claim_test: [11] DID NOT RUN — no owned card with "
		       "registers to ring\n");
		printf("irq_claim_test: [12] DID NOT RUN — so nothing rang\n");
		return 2;
	}

	kr = device_mmio_map(master_device, (vm_address_t)nic_region.base,
			     (vm_size_t)IQ_PAGE, mach_task_self(), &uva);
	if (kr != KERN_SUCCESS) {
		printf("irq_claim_test: [11] WRONG — the owned card's registers "
		       "would not map (kr=%d)\n", (int)kr);
		printf("irq_claim_test: [12] DID NOT RUN — nothing to ring\n");
		return 0;
	}
	regs = (volatile uint32_t *)uva;
	nic_write(regs, E1000_IMC, 0xFFFFFFFFu);
	nic_write(regs, E1000_ICR, 0xFFFFFFFFu);

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&port);
	if (kr == KERN_SUCCESS)
		kr = device_msi_register(master_device, (nic_bdf >> 8),
					 (nic_bdf >> 3) & 0x1F, nic_bdf & 0x7,
					 0, port, MACH_MSG_TYPE_MAKE_SEND,
					 &slot);
	if (kr != KERN_SUCCESS) {
		printf("irq_claim_test: [11] NOT ASKED — the owned card gave no "
		       "message-signalled interrupt (kr=%d), which a card with no "
		       "MSI-X table answers, and so do other refusals (#590)\n",
		       (int)kr);
		printf("irq_claim_test: [12] NOT ASKED — so it cannot ring "
		       "one\n");
		(void) device_mmio_unmap(master_device, uva,
					 (vm_size_t)IQ_PAGE, mach_task_self());
		release(port);
		return 2;
	}

	nic_write(regs, E1000_IVAR, E1000_IVAR_OTHER_ENTRY0);

	quiet = mach_msg(&msg.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			 sizeof(msg), port, 0, MACH_PORT_NULL);

	nic_write(regs, E1000_IMS, E1000_ICR_OTHER | E1000_ICR_LSC);
	nic_write(regs, E1000_ICS, E1000_ICR_LSC);

	rang = mach_msg(&msg.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			sizeof(msg), port, 1000, MACH_PORT_NULL);

	nic_write(regs, E1000_IMC, 0xFFFFFFFFu);
	nic_write(regs, E1000_ICR, 0xFFFFFFFFu);
	nic_write(regs, E1000_IVAR, 0);

	/*
	 * ⚠️ Quiet before the slot is given back.  One ring can bring more
	 * than one message -- the boot's own card test counts two -- and one
	 * still on its way when the handler is taken away arrives at a vector
	 * nobody claims, which halts the machine.  Under KVM one did.  So the
	 * port is drained until it has been silent for 200 ms.
	 */
	{
		struct {
			mach_msg_header_t	h;
			mach_msg_trailer_t	t;
		} late;

		while (mach_msg(&late.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
				sizeof(late), port, 200, MACH_PORT_NULL)
		       == MACH_MSG_SUCCESS)
			;
	}
	(void) device_intr_unregister(master_device, slot);
	(void) device_mmio_unmap(master_device, uva, (vm_size_t)IQ_PAGE,
				 mach_task_self());
	release(port);

	printf("irq_claim_test: [11] before the card was told to ring, slot %u's"
	       " port answered 0x%x — %s\n", slot, quiet,
	       quiet == MACH_RCV_TIMED_OUT
	       ? "quiet, so what arrives next is the ring's"
	       : "WRONG, something arrived unasked");
	if (quiet == MACH_RCV_TIMED_OUT)
		ok++;

	if (rang == MACH_MSG_SUCCESS
	    && msg.h.msgh_id == (mach_msg_id_t)(IQ_NOTIFY_BASE + slot)) {
		printf("irq_claim_test: [12] told to ring its other causes on "
		       "entry 0, %u:%u.%u's MSI-X arrived as slot %u's "
		       "notification\n", (unsigned)(nic_bdf >> 8),
		       (unsigned)((nic_bdf >> 3) & 0x1F),
		       (unsigned)(nic_bdf & 0x7), slot);
		ok++;
	} else
		printf("irq_claim_test: [12] WRONG — told to ring its other "
		       "causes on entry 0, %u:%u.%u's MSI-X never reached slot "
		       "%u (receive 0x%x, id %d)\n", (unsigned)(nic_bdf >> 8),
		       (unsigned)((nic_bdf >> 3) & 0x1F),
		       (unsigned)(nic_bdf & 0x7), slot, rang,
		       rang == MACH_MSG_SUCCESS ? (int)msg.h.msgh_id : -1);

	return ok;
}

/*
 * [13] to [17]: each operation asks for its own (#621).
 *
 * 🔑 ONE DEVICE, ONE TASK, AND THE CLAIM NARROWED A STEP AT A TIME.  The card
 * was claimed with DMA, MMIO and IRQ; [13] shows the calls working, and every
 * refusal after it is the same call on the same device by the same task with
 * one operation fewer in the claim.  A refusal for any other reason -- the
 * device somebody else's, a line somebody else's device raises -- would have
 * refused the control as well, and where a control cannot be had the question
 * is said NOT ASKED instead of passed.
 *
 * ⚠️ The order is forced: a claim can give an operation up and never gain one
 * back, which [17] asks, so nothing after this uses the claim -- [18] to [20]
 * end it and claim the card afresh (#745).
 */
static kern_return_t
narrow_the_claim(uint64_t ops)
{
	struct uros_cap	tok;
	kern_return_t	kr;

	memset(&tok, 0, sizeof(tok));
	kr = cap_request(RESOURCE_PCI_DEVICE, (uint64_t)nic_class, ops, 0,
			 &tok);
	if (kr != KERN_SUCCESS)
		return kr;
	return device_claim(master_device, nic_bdf, (char *)&tok, sizeof(tok));
}

/* A mapping of the card's first page, given back; its kr. */
static kern_return_t
map_a_page(void)
{
	vm_address_t	uva = 0;
	kern_return_t	kr;

	kr = device_mmio_map(master_device, (vm_address_t)nic_region.base,
			     (vm_size_t)IQ_PAGE, mach_task_self(), &uva);
	if (kr == KERN_SUCCESS)
		(void) device_mmio_unmap(master_device, uva, (vm_size_t)IQ_PAGE,
					 mach_task_self());
	return kr;
}

/* Region 0 mapped by index, given back; its kr. */
static kern_return_t
map_region_zero(void)
{
	vm_address_t	uva = 0;
	vm_size_t	size = 0;
	kern_return_t	kr;

	kr = device_region_map(master_device, nic_bdf, 0, mach_task_self(),
			       &uva, &size);
	if (kr == KERN_SUCCESS)
		(void) device_mmio_unmap(master_device, uva & ~(vm_address_t)
					 (IQ_PAGE - 1),
					 (size + (uva & (IQ_PAGE - 1))
					  + IQ_PAGE - 1) & ~(vm_size_t)
					 (IQ_PAGE - 1), mach_task_self());
	return kr;
}

/* A DMA page for the card, given back; its kr. */
static kern_return_t
dma_a_page(void)
{
	vm_address_t	vaddr = 0, dma = 0;
	cap_u64_t	region_id = 0;
	kern_return_t	kr;

	kr = device_dma_alloc(master_device, nic_bdf, (vm_size_t)IQ_PAGE,
			      &vaddr, &dma, &region_id);
	if (kr == KERN_SUCCESS)
		(void) device_dma_free(master_device, nic_bdf, vaddr,
				       (vm_size_t)IQ_PAGE);
	return kr;
}

static int
arm_eight_each_operation_its_own(void)
{
	natural_t	bus = nic_bdf >> 8, dev = (nic_bdf >> 3) & 0x1F;
	natural_t	fn = nic_bdf & 0x7, cfg = 0, line = 0, slot = 0;
	natural_t	iso = 0, ident = 0, rops = 0;
	vm_address_t	kva = 0, uva = 0, base = 0, dma = 0;
	cap_u64_t	region_id = 0;
	dma_sg_addr_t	sg = 0, pages = 0;
	mach_msg_type_number_t sg_n = 0, pages_n = 0;
	struct uros_cap	none;
	mach_port_t	port = MACH_PORT_NULL;
	kern_return_t	kr, k1, k2, k3, k4, k5, kc;
	int		ok = 0, line_ours = 0;

	if (!nic_owned || nic_region.size == 0) {
		printf("irq_claim_test: [13] NOT ASKED — no owned card with a "
		       "memory region, so no claim to narrow (#621)\n");
		printf("irq_claim_test: [14] NOT ASKED — nor [15], [16] and "
		       "[17]\n");
		return 5;
	}

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			       &port) != KERN_SUCCESS)
		return 0;

	/*
	 * [13] The controls, with all three in the claim.  The card's line is
	 * a control only if it registers now: on a board where another
	 * claimed device raises the same line, it is refused for that, and
	 * its refusal in [15] would say nothing.
	 */
	k1 = dma_a_page();
	k2 = map_region_zero();
	k3 = device_pci_config_read(master_device, bus, dev, fn, 0x3C, &cfg);
	line = cfg & 0xFF;
	k4 = KERN_INVALID_ARGUMENT;
	if (k3 == KERN_SUCCESS && line != 0 && line < 16) {
		k4 = device_intr_register(master_device, line, port,
					  MACH_MSG_TYPE_MAKE_SEND);
		if (k4 == KERN_SUCCESS) {
			line_ours = 1;
			(void) device_intr_unregister(master_device, line);
		}
	}
	if (k1 == KERN_SUCCESS && k2 == KERN_SUCCESS) {
		printf("irq_claim_test: [13] with DMA, MMIO and IRQ in the "
		       "claim, a DMA page was allocated and given back, region "
		       "0 mapped by index, and the card's line %u %s (#621)\n",
		       line, line_ours ? "registered and was given back"
		       : "is no control here");
		ok++;
	} else {
		printf("irq_claim_test: [13] WRONG — with DMA, MMIO and IRQ in "
		       "the claim, dma_alloc answered %d and region_map %d\n",
		       (int)k1, (int)k2);
		release(port);
		return ok;
	}

	/* [14] DMA given up: every buffer call refused, MMIO still works. */
	kr = narrow_the_claim(CAP_OP_PCI_MMIO_MAP | CAP_OP_PCI_IRQ);
	if (kr != KERN_SUCCESS) {
		printf("irq_claim_test: [14] WRONG — narrowing the claim to "
		       "MMIO and IRQ was refused (kr=%d)\n", (int)kr);
		release(port);
		return ok;
	}
	k1 = dma_a_page();
	k2 = device_dma_alloc_sg(master_device, nic_bdf, 1, mach_task_self(),
				 &kva, &uva, &sg, &sg_n, &region_id);
	if (k2 == KERN_SUCCESS) {
		(void) device_dma_free(master_device, nic_bdf, kva,
				       (vm_size_t)IQ_PAGE);
		if (sg != 0)
			(void) vm_deallocate(mach_task_self(), (vm_address_t)sg,
					     sg_n * sizeof(vm_address_t));
	}
	k3 = device_dma_identity(master_device, nic_bdf);
	memset(&none, 0, sizeof(none));
	k4 = device_dma_map_foreign_op(master_device, nic_bdf,
				       (vm_address_t)nic_region.base,
				       CAP_OP_DMA_DEVICE_READ, (char *)&none,
				       sizeof(none), &dma);
	k5 = device_dma_map_foreign_region(master_device, nic_bdf,
					   (char *)&none, sizeof(none), &iso,
					   &ident, &base, &rops, &pages,
					   &pages_n);
	if (k5 == KERN_SUCCESS && pages != 0)
		(void) vm_deallocate(mach_task_self(), (vm_address_t)pages,
				     pages_n * sizeof(vm_address_t));
	kc = map_a_page();
	if (kr == KERN_SUCCESS && k1 == KERN_NO_ACCESS && k2 == KERN_NO_ACCESS
	    && k3 == KERN_NO_ACCESS && k4 == KERN_NO_ACCESS
	    && k5 == KERN_NO_ACCESS && kc == KERN_SUCCESS) {
		printf("irq_claim_test: [14] narrowed to MMIO and IRQ, "
		       "dma_alloc, dma_alloc_sg, dma_identity, "
		       "dma_map_foreign_op and dma_map_foreign_region were each "
		       "refused with KERN_NO_ACCESS, and the card's page still "
		       "maps\n");
		ok++;
	} else
		printf("irq_claim_test: [14] WRONG — narrowed to MMIO and IRQ "
		       "(kr=%d), dma_alloc %d, dma_alloc_sg %d, dma_identity "
		       "%d, dma_map_foreign_op %d, dma_map_foreign_region %d, "
		       "and the page %d\n", (int)kr, (int)k1, (int)k2, (int)k3,
		       (int)k4, (int)k5, (int)kc);

	/* [15] IRQ given up: the card's interrupts refused, MMIO still works. */
	kr = narrow_the_claim(CAP_OP_PCI_MMIO_MAP);
	if (kr != KERN_SUCCESS) {
		printf("irq_claim_test: [15] WRONG — narrowing the claim to "
		       "MMIO was refused (kr=%d)\n", (int)kr);
		release(port);
		return ok;
	}
	k1 = device_msi_register(master_device, bus, dev, fn, 0, port,
				 MACH_MSG_TYPE_MAKE_SEND, &slot);
	if (k1 == KERN_SUCCESS)
		(void) device_intr_unregister(master_device, slot);
	k2 = KERN_NO_ACCESS;
	if (line_ours) {
		k2 = device_intr_register(master_device, line, port,
					  MACH_MSG_TYPE_MAKE_SEND);
		if (k2 == KERN_SUCCESS)
			(void) device_intr_unregister(master_device, line);
	}
	kc = map_a_page();
	if (kr == KERN_SUCCESS && k1 == KERN_NO_ACCESS && k2 == KERN_NO_ACCESS
	    && kc == KERN_SUCCESS) {
		printf("irq_claim_test: [15] narrowed to MMIO, msi_register was "
		       "refused with KERN_NO_ACCESS%s, and the card's page still "
		       "maps\n", line_ours ? ", and so was intr_register on the "
		       "card's own line" : "");
		ok++;
	} else
		printf("irq_claim_test: [15] WRONG — narrowed to MMIO (kr=%d), "
		       "msi_register %d, intr_register on line %u %d, and the "
		       "page %d\n", (int)kr, (int)k1, line, (int)k2, (int)kc);
	if (!line_ours)
		printf("irq_claim_test: [15] NOT ASKED for the card's line %u — "
		       "it did not register with IRQ in the claim either "
		       "(kr=%d), so its refusal now would say nothing\n", line,
		       (int)k4);
	release(port);

	/* [16] MMIO given up: the mappings refused, the claim still held. */
	kr = narrow_the_claim(CAP_OP_PCI_IRQ);
	if (kr != KERN_SUCCESS) {
		printf("irq_claim_test: [16] WRONG — narrowing the claim to "
		       "no device operation was refused (kr=%d)\n", (int)kr);
		return ok;
	}
	k1 = map_a_page();
	k2 = map_region_zero();
	k3 = device_pci_config_read(master_device, bus, dev, fn, 0x00, &cfg);
	if (kr == KERN_SUCCESS && k1 == KERN_NO_ACCESS && k2 == KERN_NO_ACCESS
	    && k3 == KERN_SUCCESS) {
		printf("irq_claim_test: [16] narrowed to no device operation, "
		       "mmio_map and region_map were refused with "
		       "KERN_NO_ACCESS, and the card is still this task's: its "
		       "configuration space answers 0x%08x\n", cfg);
		ok++;
	} else
		printf("irq_claim_test: [16] WRONG — narrowed to nothing "
		       "(kr=%d), mmio_map %d, region_map %d, configuration "
		       "read %d\n", (int)kr, (int)k1, (int)k2, (int)k3);

	/* [17] All three presented again: nothing comes back. */
	kr = narrow_the_claim(CAP_OP_PCI_DMA_MAP | CAP_OP_PCI_MMIO_MAP
			      | CAP_OP_PCI_IRQ);
	k1 = map_a_page();
	k2 = dma_a_page();
	if (kr == KERN_SUCCESS && k1 == KERN_NO_ACCESS && k2 == KERN_NO_ACCESS) {
		printf("irq_claim_test: [17] a capability carrying all three, "
		       "presented again, was answered with success and gave "
		       "nothing back: the page and a DMA page are still refused "
		       "— a claim gives operations up and never gains one\n");
		ok++;
	} else
		printf("irq_claim_test: [17] WRONG — all three presented again "
		       "(kr=%d), and the page answered %d and a DMA page %d\n",
		       (int)kr, (int)k1, (int)k2);

	return ok;
}

/*
 * Does a region of this task's map cover `a'?  vm_region() answers the region
 * at the address or the next one above it, so a region that starts above it is
 * the address unmapped -- as is no region at all.
 */
static int
page_mapped(vm_address_t a)
{
	vm_address_t			at = a;
	vm_size_t			sz = 0;
	vm_region_basic_info_data_t	bi;
	mach_msg_type_number_t		c = VM_REGION_BASIC_INFO_COUNT;
	mach_port_t			name = MACH_PORT_NULL;
	kern_return_t			kr;

	kr = vm_region(mach_task_self(), &at, &sz, VM_REGION_BASIC_INFO,
		       (vm_region_info_t) &bi, &c, &name);
	if (name != MACH_PORT_NULL)
		(void) mach_port_deallocate(mach_task_self(), name);
	return kr == KERN_SUCCESS && at <= a && a - at < sz;
}

/*
 * [18] to [20]: a revoked claim takes back what it made (#745).
 *
 * 🔴 A REVOCATION ENDED THE CLAIM AND NOTHING THE CLAIM HAD MADE.  The kernel
 * dropped the claim and the device's DMA, and left registered the line or the
 * message-signalled slot taken under it, and the device's registers mapped in
 * every task that had mapped them: the holder whose capability was gone kept
 * hearing the device and kept programming it, and only its next call was
 * refused.
 *
 * 🔑 So the card is claimed afresh, its interrupt registered and its first
 * page mapped -- each seen in place, the controls -- and the claim's own
 * capability is revoked.  Then the slot must be gone, so that giving it back
 * is refused as nothing registered, and the page must be gone from this task's
 * map.  Nothing is read or written through the mapping, and nothing is printed
 * between the revocation and the two questions, so that nothing else can come
 * to rest at the page's address meanwhile.
 *
 * ⚠️ AFRESH, because [13] to [17] left the claim holding nothing and a claim
 * never gains an operation back.  This task gives it up the one way a holder
 * can, by revoking the capability it rests on, which is [18]: the card's
 * configuration space, which answered in [16], is refused now, and a new
 * capability claims the card with all three operations.  So this is the last
 * thing done with the card, and the card is left unclaimed.
 */
static int
arm_nine_a_revocation_takes_back_what_the_claim_made(void)
{
	natural_t	bus = nic_bdf >> 8, dev = (nic_bdf >> 3) & 0x1F;
	natural_t	fn = nic_bdf & 0x7, cfg = 0, slot = 0, line = 0;
	struct uros_cap	tok;
	vm_address_t	uva = 0;
	mach_port_t	port = MACH_PORT_NULL;
	kern_return_t	k_rev, k_cfg, k_claim, k_irq, k_map;
	kern_return_t	k_unreg = KERN_SUCCESS;
	const char	*kind = "message-signalled slot";
	int		ok = 0, before = 0, after = 0;

	if (!nic_owned) {
		printf("irq_claim_test: [18] NOT ASKED — no owned card, so no "
		       "claim to revoke (#745)\n");
		printf("irq_claim_test: [19] NOT ASKED — nor [20]\n");
		return 3;
	}

	/* [18] The claim's own capability revoked: the claim ends. */
	k_rev = cap_revoke(nic_cap_id);
	k_cfg = device_pci_config_read(master_device, bus, dev, fn, 0x00, &cfg);
	memset(&tok, 0, sizeof(tok));
	k_claim = cap_request(RESOURCE_PCI_DEVICE, (uint64_t)nic_class,
			      CAP_OP_PCI_DMA_MAP | CAP_OP_PCI_MMIO_MAP
			      | CAP_OP_PCI_IRQ, 0, &tok);
	if (k_claim == KERN_SUCCESS)
		k_claim = device_claim(master_device, nic_bdf, (char *)&tok,
				       sizeof(tok));
	if (k_rev != KERN_SUCCESS || k_cfg != KERN_NO_ACCESS
	    || k_claim != KERN_SUCCESS) {
		printf("irq_claim_test: [18] WRONG — revoking the claim's "
		       "capability answered %d, the card's configuration space "
		       "then %d, and a new claim %d (#745)\n", (int)k_rev,
		       (int)k_cfg, (int)k_claim);
		printf("irq_claim_test: [19] DID NOT RUN — nor [20]: no claim "
		       "made afresh to revoke\n");
		return ok;
	}
	printf("irq_claim_test: [18] the claim's capability revoked, the card's "
	       "configuration space, which answered in [16], was refused with "
	       "KERN_NO_ACCESS, and a new capability claimed the card afresh "
	       "(#745)\n");
	ok++;

	/*
	 * The controls: an interrupt and a page, each seen in place.  A
	 * message-signalled slot where the card has a table for one, else the
	 * card's own line -- the release is one function for both, and a board
	 * gives one kind or the other.
	 */
	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			       &port) != KERN_SUCCESS)
		return ok;
	k_irq = device_msi_register(master_device, bus, dev, fn, 0, port,
				    MACH_MSG_TYPE_MAKE_SEND, &slot);
	if (k_irq != KERN_SUCCESS
	    && device_pci_config_read(master_device, bus, dev, fn, 0x3C,
				      &cfg) == KERN_SUCCESS
	    && (cfg & 0xFF) != 0 && (cfg & 0xFF) < 16) {
		line = cfg & 0xFF;
		kind = "line";
		k_irq = device_intr_register(master_device, line, port,
					     MACH_MSG_TYPE_MAKE_SEND);
		slot = line;
	}
	k_map = KERN_INVALID_ARGUMENT;
	if (nic_region.size != 0) {
		k_map = device_mmio_map(master_device,
					(vm_address_t)nic_region.base,
					(vm_size_t)IQ_PAGE, mach_task_self(),
					&uva);
		if (k_map == KERN_SUCCESS)
			before = page_mapped(uva);
	}

	k_rev = cap_revoke(tok.cap_id);
	if (k_irq == KERN_SUCCESS)
		k_unreg = device_intr_unregister(master_device, slot);
	if (k_map == KERN_SUCCESS)
		after = page_mapped(uva);

	/* [19] The interrupt: given back by the revocation, so not here. */
	if (k_irq != KERN_SUCCESS) {
		printf("irq_claim_test: [19] NOT ASKED — the card took neither a "
		       "message-signalled interrupt nor its line (kr=%d), so "
		       "there was none to take back (#745)\n", (int)k_irq);
		ok++;
	} else if (k_rev != KERN_SUCCESS)
		printf("irq_claim_test: [19] WRONG — revoking the new claim's "
		       "capability answered %d, so nothing was taken back "
		       "(#745)\n", (int)k_rev);
	else if (k_unreg == KERN_INVALID_ARGUMENT) {
		printf("irq_claim_test: [19] the card's %s %u, registered with "
		       "the claim, was refused as nothing registered once its "
		       "capability was revoked: the revocation gave it back "
		       "(#745)\n", kind, slot);
		ok++;
	} else
		printf("irq_claim_test: [19] WRONG — the card's %s %u, given "
		       "back after its claim's capability was revoked, answered "
		       "%d: %s (#745)\n", kind, slot, (int)k_unreg,
		       k_unreg == KERN_SUCCESS
		       ? "it was still this task's, and so were its "
		       "notifications" : "not as a slot nobody holds");
	release(port);

	/* [20] The page: taken out of this task's map by the revocation. */
	if (nic_region.size == 0) {
		printf("irq_claim_test: [20] NOT ASKED — the card has no memory "
		       "region to map (#745)\n");
		ok++;
	} else if (k_map != KERN_SUCCESS || !before)
		printf("irq_claim_test: [20] WRONG — the card's first page, "
		       "the control, %s (kr=%d) (#745)\n", k_map == KERN_SUCCESS
		       ? "mapped and was not found in this task's map"
		       : "would not map", (int)k_map);
	else if (k_rev != KERN_SUCCESS)
		printf("irq_claim_test: [20] WRONG — revoking the new claim's "
		       "capability answered %d, so the page at 0x%llx was not "
		       "asked about (#745)\n", (int)k_rev,
		       (unsigned long long)uva);
	else if (!after) {
		printf("irq_claim_test: [20] the card's first page, mapped at "
		       "0x%llx and found there, was gone from this task's map "
		       "once the claim's capability was revoked (#745)\n",
		       (unsigned long long)uva);
		ok++;
	} else {
		printf("irq_claim_test: [20] WRONG — the card's first page was "
		       "still mapped at 0x%llx after its claim's capability was "
		       "revoked: the registers of a device this task no longer "
		       "holds (#745)\n", (unsigned long long)uva);
		(void) device_mmio_unmap(master_device, uva, (vm_size_t)IQ_PAGE,
					 mach_task_self());
	}

	return ok;
}

/*
 * [21]: a claim made with a capability that has a deadline ends at it, and
 * takes back what it made (#747).
 *
 * 🔴 A DEADLINE ENDED THE CAPABILITY AND NOTHING IT HAD BOUGHT.  Past it the
 * token was refused wherever it was presented, and a claim made with it before
 * then went on: the card stayed claimed and its registers mapped for as long
 * as the holder lived.  cap_server revokes a capability when its deadline
 * comes now, and the revocation takes back what [18] to [20] show it takes
 * back.
 *
 * 🔑 So the card is claimed with a capability asked for IQ_DEADLINE_MS, its
 * first page mapped and found in place -- the control -- and nothing is asked
 * of cap_server again: by the deadline alone, the page must go from this
 * task's map, and the card's configuration space must be refused, as in [18].
 * The page is looked at every IQ_POLL_MS once the deadline has passed, for
 * IQ_WAIT_MS at most, with nothing printed meanwhile, so that nothing else
 * comes to rest at its address; how long it took is said.
 *
 * ⚠️ After [18] to [20], which leave the card unclaimed.  A claim not made and
 * mapped inside the lifetime, on a machine slow enough, is NOT ASKED: the
 * question needs the claim made before the deadline.
 */
#define IQ_DEADLINE_MS	300u
#define IQ_POLL_MS	20u
#define IQ_WAIT_MS	5000u

/* Now on the clock a capability's deadline is written on; 0 if unreadable. */
static uint64_t
iq_now(mach_port_t clk)
{
	tvalspec_t	ts;

	if (clk == MACH_PORT_NULL || clock_get_time(clk, &ts) != KERN_SUCCESS)
		return 0;
	return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

static int
arm_ten_a_deadline_takes_back_what_the_claim_made(void)
{
	natural_t	bus = nic_bdf >> 8, dev = (nic_bdf >> 3) & 0x1F;
	natural_t	fn = nic_bdf & 0x7, cfg = 0;
	struct uros_cap	tok;
	vm_address_t	uva = 0;
	mach_port_t	host = mach_host_self(), clk = MACH_PORT_NULL;
	kern_return_t	k_tok, k_claim = KERN_FAILURE, k_map = KERN_FAILURE;
	kern_return_t	k_cfg = KERN_FAILURE;
	uint64_t	mapped, now = 0, gone = 0;
	unsigned int	polls = 0;
	int		before = 0, after = 1;

	if (!nic_owned || nic_region.size == 0) {
		printf("irq_claim_test: [21] NOT ASKED — %s (#747)\n",
		       !nic_owned ? "no owned card, so no claim to end"
		       : "the card has no memory region to map");
		(void) mach_port_deallocate(mach_task_self(), host);
		return 1;
	}
	if (host_get_clock_service(host, URMACH_CAP_CLOCK, &clk)
	    != KERN_SUCCESS)
		clk = MACH_PORT_NULL;
	(void) mach_port_deallocate(mach_task_self(), host);

	memset(&tok, 0, sizeof(tok));
	k_tok = cap_request(RESOURCE_PCI_DEVICE, (uint64_t)nic_class,
			    CAP_OP_PCI_DMA_MAP | CAP_OP_PCI_MMIO_MAP
			    | CAP_OP_PCI_IRQ, IQ_DEADLINE_MS, &tok);
	if (k_tok == KERN_SUCCESS)
		k_claim = device_claim(master_device, nic_bdf, (char *)&tok,
				       sizeof(tok));
	if (k_claim == KERN_SUCCESS)
		k_map = device_mmio_map(master_device,
					(vm_address_t)nic_region.base,
					(vm_size_t)IQ_PAGE, mach_task_self(),
					&uva);
	if (k_map == KERN_SUCCESS)
		before = page_mapped(uva);
	mapped = iq_now(clk);

	if (mapped == 0 || k_tok != KERN_SUCCESS || tok.expires == 0
	    || k_claim != KERN_SUCCESS || !before) {
		if (mapped == 0)
			printf("irq_claim_test: [21] WRONG — the capability "
			       "clock could not be read (#747)\n");
		else if (k_tok == KERN_SUCCESS && tok.expires != 0
			 && mapped >= tok.expires)
			printf("irq_claim_test: [21] NOT ASKED — the claim and "
			       "its page were not done before the deadline of "
			       "%u ms (#747)\n", IQ_DEADLINE_MS);
		else
			printf("irq_claim_test: [21] WRONG — a capability asked "
			       "for with %u ms (%d, its deadline %s), the claim "
			       "%d, and the card's first page %s (%d) (#747)\n",
			       IQ_DEADLINE_MS, (int)k_tok,
			       tok.expires != 0 ? "written" : "not written",
			       (int)k_claim, before ? "mapped"
			       : "not found in this task's map", (int)k_map);
		if (k_map == KERN_SUCCESS)
			(void) device_mmio_unmap(master_device, uva,
						 (vm_size_t)IQ_PAGE,
						 mach_task_self());
		if (k_tok == KERN_SUCCESS)
			(void) cap_revoke(tok.cap_id);
		if (clk != MACH_PORT_NULL)
			(void) mach_port_deallocate(mach_task_self(), clk);
		return mapped != 0 && k_tok == KERN_SUCCESS && tok.expires != 0
		       && mapped >= tok.expires;
	}

	/* Past the deadline, the page looked at: nothing printed meanwhile. */
	do {
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT,
				     IQ_POLL_MS);
		now = iq_now(clk);
		if (now != 0 && now >= tok.expires) {
			after = page_mapped(uva);
			if (!after)
				gone = now;
		}
	} while (now != 0 && after
		 && now < tok.expires + (uint64_t)IQ_WAIT_MS * 1000000ULL
		 && ++polls < 2 * (IQ_DEADLINE_MS + IQ_WAIT_MS) / IQ_POLL_MS);
	k_cfg = device_pci_config_read(master_device, bus, dev, fn, 0x00,
				       &cfg);
	(void) mach_port_deallocate(mach_task_self(), clk);

	if (now != 0 && !after && k_cfg == KERN_NO_ACCESS) {
		printf("irq_claim_test: [21] a claim made with a capability "
		       "asked for with %u ms had the card's first page, mapped "
		       "at 0x%llx and found there, taken from this task's map "
		       "within %llu ms of the deadline, and its configuration "
		       "space refused with KERN_NO_ACCESS, with nothing revoked "
		       "by this task: the deadline ended the claim (#747)\n",
		       IQ_DEADLINE_MS, (unsigned long long)uva,
		       (unsigned long long)((gone - tok.expires) / 1000000ULL));
		return 1;
	}
	printf("irq_claim_test: [21] WRONG — a claim made with a capability "
	       "asked for with %u ms: %s, the card's first page %s at 0x%llx "
	       "%u ms past the deadline, and its configuration space answered "
	       "%d, where the page must be gone and the space refused with "
	       "KERN_NO_ACCESS(%d) (#747)\n", IQ_DEADLINE_MS,
	       now == 0 ? "the clock could not be read while waiting"
	       : "the clock read past the deadline",
	       after ? "still mapped" : "gone", (unsigned long long)uva,
	       IQ_WAIT_MS, (int)k_cfg, (int)KERN_NO_ACCESS);
	if (after)
		(void) device_mmio_unmap(master_device, uva, (vm_size_t)IQ_PAGE,
					 mach_task_self());
	if (k_cfg == KERN_SUCCESS)
		(void) cap_revoke(tok.cap_id);
	return 0;
}

int
main(int argc, char **argv)
{
	mach_port_t	host;
	mach_port_t	root_wired, root_paged, security;
	kern_return_t	kr;
	int		passed = 0;
	int		msi_half = (argc > 1 && argv[1] != 0
				    && argv[1][0] == 'm');

	kr = bootstrap_ports(bootstrap_port, &host, &master_device,
			     &root_wired, &root_paged, &security);
	if (kr != KERN_SUCCESS) {
		printf("irq_claim_test: no bootstrap ports: 0x%x — WRONG\n", kr);
		return 1;
	}

	/*
	 * ── Two halves, and where each one sits in the boot is the reason ──
	 *
	 * 🔴 THIS PROGRAM USED TO BE ONE ENTRY, THIRD, and its position was
	 * load-bearing: bootstrap.conf's own note says a halt in it is
	 * unmistakable there, where the same halt at the end would look like a
	 * run that simply finished.  What could halt is GIVING A LINE BACK,
	 * which is the first four arms.
	 *
	 * 🔑 And #511 gave the fifth arm two dependencies the first four do
	 * not have: it must own a device to ask it for a message-signalled
	 * interrupt, so it needs cap_server for the capability and the HAL to
	 * learn which device -- both of which start much later.  One entry
	 * could not be in both places.
	 *
	 * ⚠️ So the halves run as two entries, chosen by argv, the way
	 * dma_reclaim_test's holder and checker already do.  The line arms
	 * keep the position their failure mode needs; the MSI arms move to
	 * where the servers they depend on are running.
	 */
	if (!msi_half) {
		passed += arm_one_claim_and_release();
		passed += arm_two_round_trips();
		passed += arm_three_nothing_was_delivered();
		passed += arm_four_what_is_refused();

		printf("irq_claim_test: %d of 6 line arms passed\n", passed);
		return passed == 6 ? 0 : 1;
	}

	passed += arm_five_an_interrupt_with_no_wire();
	passed += arm_six_a_mapping_ends_with_its_region();
	passed += arm_seven_the_card_rings();
	passed += arm_eight_each_operation_its_own();	/* it narrows */
	/* It revokes, and the card is left unclaimed (#745). */
	passed += arm_nine_a_revocation_takes_back_what_the_claim_made();
	/* Last: it claims the card again, until a deadline (#747). */
	passed += arm_ten_a_deadline_takes_back_what_the_claim_made();
	printf("irq_claim_test: %d of 15 message-signalled, mapping, operation, "
	       "revocation and deadline arms passed\n", passed);
	return passed == 15 ? 0 : 1;
}
