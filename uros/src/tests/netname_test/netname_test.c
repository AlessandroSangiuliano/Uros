/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Can the name server be CONTACTED on this target? (#426)
 *
 * The last clause of #426's done-when, and the one thing the other two could
 * not show.  A Mach trap from userland works and mach_print reaches the
 * console -- both proven by a boot -- but neither of those crosses the whole
 * surface this issue is about: a MIG-generated stub, a message built by it, a
 * reply demultiplexed by a server written on the other side of the port, and
 * an out-of-line port right coming back.
 *
 * ⚠️ Which is why "the name server starts" is NOT this test.  It starts on
 * x86-64 today: bootstrap loads it, it prints, and its own service_checkin()
 * to bootstrap succeeds.  What that shows is that the server can reach
 * bootstrap, not that anybody can reach the SERVER.  Nothing on this target
 * does -- cap_server, proc_server and the rest of netname's clients are not
 * ported, and bootstrap's own netname_look_up() is in stage 2, behind a disk
 * this target does not have.  So the client had to be written.
 *
 * ── What it does, and why each half is here ──
 *
 * Register a port under a name, ask for it back, and check that the right that
 * came back names the same port.  Then ask for a name nobody registered, and
 * check that the answer is a refusal rather than a port.
 *
 * 🔑 The second half is the one that makes the first mean anything.  A stub
 * that returned success and a null port for everything would pass a check that
 * only ever asks for a name that exists.  [feedback: proofs by absence need a
 * presence, and a test needs an arm that can fail]
 *
 * ── Whose name it is (#744) ──
 *
 * Arms [3] to [8] ask what a signature is for.  A name checked in again, or
 * checked out, is refused to anyone who does not present the signature it was
 * checked in with -- [3] and [5] with no signature, [4] with another -- and a
 * name nobody holds yet is refused without one ([6]), or under the port being
 * registered, which everyone who looks the name up can present ([7]).
 *
 * 🔑 [8] is the control for all five: the name's own signature checks it out.
 * A name server that refused everything would pass [3] to [7], and only [8]
 * tells refusing the wrong caller from refusing every caller.
 *
 * Each refusal is asserted by its WHOLE value, as [2] is, and the two values
 * are two halves: NETNAME_INVALID_PORT is the name server refusing a signature
 * nobody holds alone, NETNAME_NOT_YOURS the signature the name was checked in
 * with refusing a different one.  With the first taken out, [3] and [5] get the
 * second, and [6] and [7] get through.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/message.h>
#include <mach/port.h>

#include <stdio.h>

#include <servers/netname.h>
#include <servers/netname_defs.h>

/*
 * libmach fills this from mach_ports_lookup() during startup, out of the port
 * set bootstrap registered on this task.  MACH_PORT_NULL here is a result and
 * not a reason to skip: it means the task was started without the name server
 * in its registered ports, which is a boot-path answer to #426's question.
 */
extern mach_port_t	name_server_port;

#define	TEST_NAME	"netname_test"
#define	ABSENT_NAME	"netname_test_absent"
#define	UNSIGNED_NAME	"netname_test_unsigned"
#define	SELF_SIGNED_NAME "netname_test_self_signed"

/*
 * Whether TEST_NAME still answers with `mine'.  A task holds one name for all
 * its rights to one port, so the send right a look-up hands back arrives under
 * the name of the receive right this task already has: equal names are the
 * same port.  The name server's own signature check rests on the same fact.
 */
static int
still_mine(mach_port_t mine, mach_port_t *got)
{
	*got = MACH_PORT_NULL;
	return netname_look_up(name_server_port, "", TEST_NAME, got)
		== NETNAME_SUCCESS && *got == mine;
}

/*
 * A second port, with a send right to sign with: another task's signature as
 * far as the name server can tell, since all it compares is the port.
 */
static mach_port_t
another_port(void)
{
	mach_port_t	p = MACH_PORT_NULL;

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p)
	    != KERN_SUCCESS)
		return MACH_PORT_NULL;
	if (mach_port_insert_right(mach_task_self(), p, p,
				   MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS)
		return MACH_PORT_NULL;
	return p;
}

int
main(int argc, char **argv)
{
	mach_port_t	mine = MACH_PORT_NULL;
	mach_port_t	got  = MACH_PORT_NULL;
	mach_port_t	sig  = MACH_PORT_NULL;
	mach_port_t	other = MACH_PORT_NULL;
	kern_return_t	kr;
	int		passed = 0, arms = 0, registered = 0, kept;

	printf("netname_test: started (#426)\n");
	if (argc > 0 && argv != 0 && argv[0] != 0)
		printf("netname_test: argv[0] is \"%s\" (#488)\n", argv[0]);

	if (name_server_port == MACH_PORT_NULL) {
		printf("netname_test: WRONG — no name server port in this "
		       "task's registered ports, so it cannot be contacted "
		       "from here at all\n");
		printf("netname_test: 0 of 8 arms passed\n");
		return 1;
	}

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &mine);
	if (kr == KERN_SUCCESS)
		kr = urmach_netname_signature(&sig);
	if (kr != KERN_SUCCESS) {
		printf("netname_test: WRONG — no port or no signature: %d\n",
		       kr);
		printf("netname_test: 0 of 8 arms passed\n");
		return 1;
	}

	/*
	 * [1] Register, then look up, and compare the NAMES.
	 *
	 * The right that comes back is a send right the server made from its
	 * own copy, and it arrives under the name of the receive right this
	 * task holds, because a task holds one name for all its rights to one
	 * port (still_mine()).  So the look-up must hand back `mine' itself:
	 * a valid send right to some other port is a wrong answer, and it used
	 * to pass here.
	 */
	arms++;
	kr = netname_check_in(name_server_port, TEST_NAME, sig, mine);
	if (kr != NETNAME_SUCCESS) {
		printf("netname_test: [1] WRONG — netname_check_in(\"%s\") "
		       "returned %d\n", TEST_NAME, kr);
	} else {
		registered = 1;
		kr = netname_look_up(name_server_port, "",
				     TEST_NAME, &got);
		if (kr != NETNAME_SUCCESS)
			printf("netname_test: [1] WRONG — checked in, and "
			       "netname_look_up(\"%s\") returned %d\n",
			       TEST_NAME, kr);
		else if (got != mine)
			printf("netname_test: [1] WRONG — look-up succeeded "
			       "and handed back port 0x%x, not 0x%x\n",
			       got, mine);
		else {
			printf("netname_test: [1] registered \"%s\" and got "
			       "the same port back (0x%x) — the name server "
			       "answered an RPC from another task\n",
			       TEST_NAME, got);
			passed++;
		}
	}

	/*
	 * [2] A name nobody registered must be refused.
	 *
	 * This is the control.  Without it, [1] cannot tell a working server
	 * from a stub that says NETNAME_SUCCESS to everything.
	 *
	 * ⚠️ It asserts the WHOLE value, not merely "not NETNAME_SUCCESS", and
	 * that is the difference between a control and a decoration.  Written
	 * the loose way it accepted MIG_SERVER_DIED (-308) -- the reply port
	 * coming back with a send-once notification instead of an answer, i.e.
	 * the RPC never reaching the name server at all -- and reported the arm
	 * as passed.  Measured at 1.40 GHz with four processors it did exactly
	 * that in three runs out of seven: [1] failed with -308, [2] "passed"
	 * with -308, and the run above them said one arm of two.  A control
	 * that passes when the server is dead is passing in precisely the case
	 * it exists to exclude.
	 */
	arms++;
	got = MACH_PORT_NULL;
	kr = netname_look_up(name_server_port, "",
			     ABSENT_NAME, &got);
	if (kr == NETNAME_NOT_CHECKED_IN) {
		printf("netname_test: [2] an unregistered name is refused "
		       "with NETNAME_NOT_CHECKED_IN — the answer to [1] was "
		       "about that name and not about every name\n");
		passed++;
	} else if (kr == NETNAME_SUCCESS)
		printf("netname_test: [2] WRONG — \"%s\" was never registered "
		       "and the look-up succeeded, handing back 0x%x\n",
		       ABSENT_NAME, got);
	else
		printf("netname_test: [2] WRONG — \"%s\" was refused with %d, "
		       "not NETNAME_NOT_CHECKED_IN(%d): the name server did "
		       "not answer this call\n",
		       ABSENT_NAME, kr, NETNAME_NOT_CHECKED_IN);

	/*
	 * [3] The name checked in again with no signature is refused, by the
	 * name server and before the name is looked at.
	 */
	arms++;
	kr = netname_check_in(name_server_port, TEST_NAME, MACH_PORT_NULL,
			      mine);
	kept = still_mine(mine, &got);
	if (!registered)
		printf("netname_test: [3] WRONG — not asked: [1] never "
		       "checked \"%s\" in\n", TEST_NAME);
	else if (kr == NETNAME_INVALID_PORT && kept) {
		printf("netname_test: [3] \"%s\" checked in again with no "
		       "signature: refused with NETNAME_INVALID_PORT, and it "
		       "still answers with 0x%x (#744)\n", TEST_NAME, got);
		passed++;
	} else
		printf("netname_test: [3] WRONG — \"%s\" checked in again with "
		       "no signature: %d, not NETNAME_INVALID_PORT(%d), and it "
		       "answers with 0x%x, not 0x%x (#744)\n", TEST_NAME, kr,
		       NETNAME_INVALID_PORT, got, mine);

	/*
	 * [4] The name checked in again under another signature is refused by
	 * the one it was checked in with.
	 *
	 * ⚠️ For `mine' and not for `other': a port signing for itself is [7]'s
	 * refusal, which the name server makes before it looks for the name,
	 * so this arm would be asking that question and not its own.
	 */
	arms++;
	other = another_port();
	kr = (other == MACH_PORT_NULL) ? KERN_RESOURCE_SHORTAGE
		: netname_check_in(name_server_port, TEST_NAME, other, mine);
	kept = still_mine(mine, &got);
	if (!registered)
		printf("netname_test: [4] WRONG — not asked: [1] never "
		       "checked \"%s\" in\n", TEST_NAME);
	else if (kr == NETNAME_NOT_YOURS && kept) {
		printf("netname_test: [4] \"%s\" checked in again under another "
		       "signature: refused with NETNAME_NOT_YOURS, and it still "
		       "answers with 0x%x (#744)\n", TEST_NAME, got);
		passed++;
	} else
		printf("netname_test: [4] WRONG — \"%s\" checked in again under "
		       "another signature: %d, not NETNAME_NOT_YOURS(%d), and "
		       "it answers with 0x%x, not 0x%x (#744)\n", TEST_NAME, kr,
		       NETNAME_NOT_YOURS, got, mine);

	/*
	 * [5] The name checked out with no signature is refused.
	 */
	arms++;
	kr = netname_check_out(name_server_port, TEST_NAME, MACH_PORT_NULL);
	kept = still_mine(mine, &got);
	if (!registered)
		printf("netname_test: [5] WRONG — not asked: [1] never "
		       "checked \"%s\" in\n", TEST_NAME);
	else if (kr == NETNAME_INVALID_PORT && kept) {
		printf("netname_test: [5] \"%s\" checked out with no signature: "
		       "refused with NETNAME_INVALID_PORT, and it still answers "
		       "with 0x%x (#744)\n", TEST_NAME, got);
		passed++;
	} else
		printf("netname_test: [5] WRONG — \"%s\" checked out with no "
		       "signature: %d, not NETNAME_INVALID_PORT(%d), and it "
		       "answers with 0x%x, not 0x%x (#744)\n", TEST_NAME, kr,
		       NETNAME_INVALID_PORT, got, mine);

	/*
	 * [6] A name nobody holds is refused without a signature: refusing a
	 * null signature only where the name is taken would leave every name
	 * to whoever asks for it first without one.
	 */
	arms++;
	kr = netname_check_in(name_server_port, UNSIGNED_NAME, MACH_PORT_NULL,
			      mine);
	got = MACH_PORT_NULL;
	kept = netname_look_up(name_server_port, "", UNSIGNED_NAME, &got)
	       == NETNAME_NOT_CHECKED_IN;
	if (kr == NETNAME_INVALID_PORT && kept) {
		printf("netname_test: [6] \"%s\", which nobody holds, checked in "
		       "with no signature: refused with NETNAME_INVALID_PORT, "
		       "and it is not checked in (#744)\n", UNSIGNED_NAME);
		passed++;
	} else
		printf("netname_test: [6] WRONG — \"%s\" checked in with no "
		       "signature: %d, not NETNAME_INVALID_PORT(%d), and a "
		       "look-up hands back 0x%x (#744)\n", UNSIGNED_NAME, kr,
		       NETNAME_INVALID_PORT, got);

	/*
	 * [7] A port cannot sign for itself: everyone who looks the name up
	 * holds a send right to it, and could present it.
	 */
	arms++;
	kr = (other == MACH_PORT_NULL) ? KERN_RESOURCE_SHORTAGE
		: netname_check_in(name_server_port, SELF_SIGNED_NAME, other,
				   other);
	got = MACH_PORT_NULL;
	kept = netname_look_up(name_server_port, "", SELF_SIGNED_NAME, &got)
	       == NETNAME_NOT_CHECKED_IN;
	if (kr == NETNAME_INVALID_PORT && kept) {
		printf("netname_test: [7] \"%s\" checked in signed by the port "
		       "it publishes: refused with NETNAME_INVALID_PORT, and it "
		       "is not checked in (#744)\n", SELF_SIGNED_NAME);
		passed++;
	} else
		printf("netname_test: [7] WRONG — \"%s\" checked in signed by "
		       "the port it publishes: %d, not NETNAME_INVALID_PORT(%d), "
		       "and a look-up hands back 0x%x (#744)\n",
		       SELF_SIGNED_NAME, kr, NETNAME_INVALID_PORT, got);

	/*
	 * [8] The control for [3] to [7]: the name's own signature checks it
	 * out, and then the name is gone.
	 */
	arms++;
	kr = netname_check_out(name_server_port, TEST_NAME, sig);
	got = MACH_PORT_NULL;
	kept = netname_look_up(name_server_port, "", TEST_NAME, &got)
	       == NETNAME_NOT_CHECKED_IN;
	if (!registered)
		printf("netname_test: [8] WRONG — not asked: [1] never "
		       "checked \"%s\" in\n", TEST_NAME);
	else if (kr == NETNAME_SUCCESS && kept) {
		printf("netname_test: [8] \"%s\" checked out under its own "
		       "signature, and it is gone — the refusals above were "
		       "about who asked (#744)\n", TEST_NAME);
		passed++;
	} else
		printf("netname_test: [8] WRONG — \"%s\" checked out under its "
		       "own signature: %d, not NETNAME_SUCCESS, and a look-up "
		       "hands back 0x%x (#744)\n", TEST_NAME, kr, got);

	printf("netname_test: %d of %d arms passed\n", passed, arms);
	return (passed == arms) ? 0 : 1;
}
