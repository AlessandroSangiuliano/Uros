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
 * ⚠️ NOT IRQ 13, which was the first choice because it is one of only two
 * lines the i386 kernel claims with a non-zero priority, and so the only way
 * to reach the "restore what was displaced" branch from here.  It is also the
 * FPU error line, and CR0_NE is never set in this tree -- the only two
 * mentions of it are in locore.S's revision history -- so numeric errors
 * still arrive as FERR# on that pin.  A line that can fire on its own makes
 * the count in arm [4] a measurement of something this program does not
 * control.  The displaced-handler branch is therefore NOT covered here, and
 * saying so is better than covering it with a line the test cannot hold
 * still.
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
			bdf = (natural_t)((devs[i].bus << 8)
					  | (devs[i].slot << 3) | devs[i].func);
			class_rev = devs[i].class_rev;
			found = 1;
			break;
		}

	if (bytes != 0)
		(void) vm_deallocate(mach_task_self(), buf, bytes);
	if (!found) {
		printf("irq_claim_test: the HAL listed %u device(s) and none "
		       "of class 0x%06x\n", n, IQ_NET_CLASS);
		return 0;
	}

	memset(&tok, 0, sizeof(tok));
	kr = cap_request(RESOURCE_PCI_DEVICE, (uint64_t)(class_rev >> 8),
			 CAP_OP_PCI_IRQ, 0, &tok);
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

	if (!own_the_network_card(&bdf)) {
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
	printf("irq_claim_test: %d of 2 message-signalled arms passed\n",
	       passed);
	return passed == 2 ? 0 : 1;
}
