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

int
main(int argc, char **argv)
{
	mach_port_t	mine = MACH_PORT_NULL;
	mach_port_t	got  = MACH_PORT_NULL;
	kern_return_t	kr;
	int		passed = 0, arms = 0;

	printf("netname_test: started (#426)\n");
	if (argc > 0 && argv != 0 && argv[0] != 0)
		printf("netname_test: argv[0] is \"%s\" (#488)\n", argv[0]);

	if (name_server_port == MACH_PORT_NULL) {
		printf("netname_test: WRONG — no name server port in this "
		       "task's registered ports, so it cannot be contacted "
		       "from here at all\n");
		printf("netname_test: 0 of 2 arms passed\n");
		return 1;
	}

	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &mine);
	if (kr != KERN_SUCCESS) {
		printf("netname_test: WRONG — mach_port_allocate %d\n", kr);
		printf("netname_test: 0 of 2 arms passed\n");
		return 1;
	}

	/*
	 * [1] Register, then look up, and compare the NAMES.
	 *
	 * ⚠️ The right that comes back is a send right the server made from
	 * its own copy, so it arrives under a name of this space's choosing --
	 * which may differ from `mine'.  What must agree is what the two names
	 * refer to, and mach_port_names() would be a long way round: asking
	 * the kernel to compare them is what MACH_MSG_TYPE_MAKE_SEND on the
	 * same receive right guarantees, so a successful look-up that yields a
	 * VALID send right is the claim, and a null or dead one is the failure.
	 */
	arms++;
	kr = netname_check_in(name_server_port, TEST_NAME,
			      MACH_PORT_NULL, mine);
	if (kr != NETNAME_SUCCESS) {
		printf("netname_test: [1] WRONG — netname_check_in(\"%s\") "
		       "returned %d\n", TEST_NAME, kr);
	} else {
		kr = netname_look_up(name_server_port, "",
				     TEST_NAME, &got);
		if (kr != NETNAME_SUCCESS)
			printf("netname_test: [1] WRONG — checked in, and "
			       "netname_look_up(\"%s\") returned %d\n",
			       TEST_NAME, kr);
		else if (got == MACH_PORT_NULL || got == MACH_PORT_DEAD)
			printf("netname_test: [1] WRONG — look-up succeeded "
			       "and handed back port 0x%x\n", got);
		else {
			printf("netname_test: [1] registered \"%s\" and got a "
			       "live send right back (0x%x) — the name server "
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
	 */
	arms++;
	got = MACH_PORT_NULL;
	kr = netname_look_up(name_server_port, "",
			     ABSENT_NAME, &got);
	if (kr == NETNAME_SUCCESS)
		printf("netname_test: [2] WRONG — \"%s\" was never registered "
		       "and the look-up succeeded, handing back 0x%x\n",
		       ABSENT_NAME, got);
	else {
		printf("netname_test: [2] an unregistered name is refused "
		       "(%d) — the answer to [1] was about that name and not "
		       "about every name\n", kr);
		passed++;
	}

	printf("netname_test: %d of %d arms passed\n", passed, arms);
	return (passed == arms) ? 0 : 1;
}
