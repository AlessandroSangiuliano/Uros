/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Two tasks claim the same I/O range at the same instant, many times, and
 * exactly one of them must win every time (#538).
 *
 * ── Why a test and not twenty green boots ──
 *
 * A torn table is rare by nature.  #511's specimen -- `claimed by task 0x0'
 * -- appeared twice in ten boots only once a claim entry had grown seven
 * times bigger, and before that in none of eighty.  So the evidence that the
 * writers are serialised has to be a PRESENCE: a program that puts two
 * processors on the same slot on purpose, over and over, and reports the
 * count.  Twenty boots that happened not to collide say nothing (#538,
 * done-when 7).
 *
 * ── What it does ──
 *
 * The same binary runs twice, as `a' and as `b', from bootstrap.conf.  Each
 * registers a port under its name and finds the other's.  Then, for every
 * round: both send READY and wait for the other's READY -- a barrier that
 * puts the two claims as close together as two tasks can be put -- both call
 * device_io_port_claim() on the same eight ports, both exchange their results
 * WHILE STILL HOLDING whatever they got, and only then does the winner give
 * the range back.  `a' judges: one KERN_SUCCESS and one KERN_NO_ACCESS is a
 * round that behaved; two successes is the table torn -- two tasks holding
 * one range at once; two refusals is a range nobody could take, which is the
 * retire-after-grace gone wrong.
 *
 * 🔥 THE ORDER OF RELEASE AND EXCHANGE IS THE TEST.  The first version had
 * the winner release BEFORE the exchange, and on one processor -- where the
 * two claims are serialised by the scheduler -- the second claimant found the
 * range free and won too: four hundred rounds, four hundred "torn", and not
 * one of them was.  A test that cannot tell "both held it at once" from "both
 * held it in turn" is a test of the scheduler.
 *
 * ⚠️ The range is COM3's, 0x3E8..0x3EF, and it is never touched -- claimed
 * and released only.  COM1 is uart.so's and would refuse both (#497); COM2
 * is char_test's control port.
 *
 * ── The third word (#563) ──
 *
 * On one processor the two claims are serialised by the scheduler, and a
 * run that never collided has not asked the question.  So with fewer than
 * two processors available and nothing wrong observed, the race arm says
 * NOT ASKED; with two or more it says PASS or WRONG.  The default harness
 * boots one processor, so a default boot reads NOT ASKED here, which is the
 * truth and not a failure; `run-x86_64.sh 180 -smp 4' asks it.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/message.h>
#include <mach/port.h>
#include <mach/thread_switch.h>
#include <mach/bootstrap.h>
#include <mach/mach_host.h>
#include <mach/host_info.h>
#include <mach/clock.h>
#include <mach/clock_types.h>

#include <stdio.h>
#include <string.h>

#include <sa_mach.h>
#include <servers/netname.h>
#include <servers/netname_defs.h>

#include "device_master.h"		/* MIG: device_io_port_claim/unclaim */

extern mach_port_t	name_server_port;
extern mach_port_t	bootstrap_port;

/*
 * COM3's window: never written, only claimed.  Not COM1, which is uart.so's
 * and would refuse both halves (#497); and not COM2, which the first version
 * used -- char_test's third arm pokes COM2's scratch register as its
 * "unclaimed" control, and found it claimed by this test four hundred times
 * in a row.  Two tests that share a port are one test that fails.
 */
#define	RACE_PORT	0x3E8u
#define	RACE_COUNT	8u

/*
 * Rounds, and a clock on them (#516).  Four hundred rounds is the number;
 * twenty seconds is the budget, and the budget wins.  Under KVM the rounds
 * take a second or two.  Under TCG on one processor, with pthread_test's
 * sweeps competing for that processor, the first boot of this test managed
 * 355 rounds in fifteen minutes -- not because a round is slow but because
 * two tasks passing messages get few time slices while another program
 * computes.  A test that cannot finish in that world reports nothing; one
 * that stops at the budget reports how far it got, which is the number the
 * harness can read.  A progress line every hundred rounds is what tells a
 * watcher the difference between slow and stuck.
 */
#define	ROUNDS		400u
#define	BUDGET_S	20u
#define	PROGRESS_EVERY	100u

#define	NAME_A		"io_claim_race_a"
#define	NAME_B		"io_claim_race_b"

#define	MSG_READY	5380
#define	MSG_RESULT	5381

#define	FIND_TRIES	400u		/* x 50 ms: the peer has 20 s to appear */
#define	FIND_WAIT_MS	50
#define	RCV_TIMEOUT_MS	5000

/*
 * A breath between rounds, and why it is not a courtesy.  A released slot
 * is RETIRING until a grace period has passed (#538), and a round here is
 * a few messages -- shorter than a grace period on a busy machine.  Four
 * slots, one of them uart.so's, hammered without a pause would run the
 * table out of reusable slots, and both claimants would be told
 * KERN_RESOURCE_SHORTAGE: not a torn table, a full one.  A millisecond lets
 * every processor report a quiescent state between rounds.  Rounds where
 * it still happens are counted apart, so that a starved table cannot be
 * mistaken for a torn one in either direction.
 */
#define	BREATH_MS	1

struct race_msg {
	mach_msg_header_t	head;
	NDR_record_t		ndr;
	natural_t		round;
	natural_t		value;
};

struct race_rcv {
	struct race_msg		msg;
	mach_msg_trailer_t	trailer;
};

static int
tell(mach_port_t peer, int id, unsigned round, unsigned value)
{
	struct race_msg m;

	memset(&m, 0, sizeof(m));
	m.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	m.head.msgh_size = sizeof(m);
	m.head.msgh_remote_port = peer;
	m.head.msgh_local_port = MACH_PORT_NULL;
	m.head.msgh_id = id;
	m.ndr = NDR_record;
	m.round = round;
	m.value = value;

	return mach_msg(&m.head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(m),
			0, MACH_PORT_NULL, RCV_TIMEOUT_MS, MACH_PORT_NULL)
		== MACH_MSG_SUCCESS;
}

/* Returns 1 and fills value when a message with this id and round arrives. */
static int
hear(mach_port_t mine, int id, unsigned round, unsigned *value)
{
	struct race_rcv r;

	memset(&r, 0, sizeof(r));
	if (mach_msg(&r.msg.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
		     sizeof(r), mine, RCV_TIMEOUT_MS, MACH_PORT_NULL)
	    != MACH_MSG_SUCCESS)
		return 0;
	if (r.msg.head.msgh_id != id || r.msg.round != round)
		return 0;
	*value = (unsigned)r.msg.value;
	return 1;
}

static mach_port_t
find(const char *name)
{
	mach_port_t	p = MACH_PORT_NULL;
	unsigned	t;

	for (t = 0; t < FIND_TRIES; t++) {
		if (netname_look_up(name_server_port, "", (char *)name, &p)
		    == NETNAME_SUCCESS && p != MACH_PORT_NULL)
			return p;
		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT,
				     FIND_WAIT_MS);
	}
	return MACH_PORT_NULL;
}

/* Seconds since the clock service's epoch; 0 if the service is absent. */
static unsigned
seconds_now(mach_port_t host)
{
	static mach_port_t	clock_port = MACH_PORT_NULL;
	tvalspec_t		tv;

	if (clock_port == MACH_PORT_NULL
	    && host_get_clock_service(host, REALTIME_CLOCK, &clock_port)
	       != KERN_SUCCESS)
		return 0;
	if (clock_get_time(clock_port, &tv) != KERN_SUCCESS)
		return 0;
	return (unsigned)tv.tv_sec;
}

static unsigned
processors(void)
{
	host_basic_info_data_t	hi;
	mach_msg_type_number_t	c = HOST_BASIC_INFO_COUNT;

	/* host_info, not host_statistics: HOST_BASIC_INFO is host_info's
	 * flavour (kern/host.c:218), and the other call answered 1 on a
	 * four-processor boot. */
	if (host_info(mach_host_self(), HOST_BASIC_INFO,
		      (host_info_t)&hi, &c) != KERN_SUCCESS)
		return 1;
	return hi.avail_cpus > 0 ? (unsigned)hi.avail_cpus : 1u;
}

int
main(int argc, char **argv)
{
	const char	*role = (argc > 1 && argv[1] != 0) ? argv[1] : "a";
	int		judge = (strcmp(role, "a") == 0);
	mach_port_t	host = MACH_PORT_NULL, device = MACH_PORT_NULL;
	mach_port_t	lw = MACH_PORT_NULL, lp = MACH_PORT_NULL, sec = MACH_PORT_NULL;
	mach_port_t	mine = MACH_PORT_NULL, peer;
	kern_return_t	kr;
	unsigned	round, ncpu;
	unsigned	torn = 0, empty = 0, starved = 0, odd_refusal = 0;
	unsigned	behaved = 0;
	unsigned	my_wins = 0;
	unsigned	t0, elapsed = 0;
	int		passed = 0, arms = 0, peer_gone = 0, out_of_time = 0;

	kr = bootstrap_ports(bootstrap_port, &host, &device, &lw, &lp, &sec);
	if (kr != KERN_SUCCESS)
		_exit(1);
	printf_init(device);

	if (judge)
		printf("io_claim_race: started (#538)\n");

	if (name_server_port == MACH_PORT_NULL) {
		if (judge) {
			printf("io_claim_race: WRONG — no name server port, "
			       "so the two halves cannot find each other\n");
			printf("io_claim_race: 0 of 3 arms passed\n");
		}
		return 1;
	}

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			       &mine) != KERN_SUCCESS
	    || mach_port_insert_right(mach_task_self(), mine, mine,
				      MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS
	    || netname_check_in(name_server_port, (char *)(judge ? NAME_A
							         : NAME_B),
				MACH_PORT_NULL, mine) != NETNAME_SUCCESS) {
		if (judge) {
			printf("io_claim_race: WRONG — could not register "
			       "\"%s\"\n", judge ? NAME_A : NAME_B);
			printf("io_claim_race: 0 of 3 arms passed\n");
		}
		return 1;
	}

	peer = find(judge ? NAME_B : NAME_A);
	if (peer == MACH_PORT_NULL) {
		if (judge) {
			printf("io_claim_race: [1] NOT ASKED — the other half "
			       "never registered, so the question was never "
			       "put (#563)\n");
			printf("io_claim_race: 0 of 0 arms passed\n");
		}
		return 0;
	}

	ncpu = processors();
	t0 = seconds_now(host);

	/*
	 * [1] The race.  Every round: barrier, claim, winner releases,
	 * exchange, judge.  The READY message carries a flag: 1 means "this
	 * is the last round I will play", so both halves stop together when
	 * either runs out of time.
	 */
	for (round = 0; round < ROUNDS; round++) {
		unsigned	got, peer_kr, last;
		natural_t	released = 0, klog_from = 0;

		elapsed = seconds_now(host) - t0;
		last = (elapsed >= BUDGET_S) ? 1u : 0u;

		if (!tell(peer, MSG_READY, round, last)
		    || !hear(mine, MSG_READY, round, &got)) {
			peer_gone = 1;
			break;
		}
		if (last || got) {
			out_of_time = 1;
			break;
		}

		if (judge && round != 0 && round % PROGRESS_EVERY == 0)
			printf("io_claim_race: %u rounds in %u s\n", round,
			       elapsed);

		kr = device_io_port_claim(device, RACE_PORT, RACE_COUNT,
					  &released, &klog_from);
		if (kr == KERN_SUCCESS)
			my_wins++;

		/* Exchange while still holding: see the header. */
		if (!tell(peer, MSG_RESULT, round, (unsigned)kr)
		    || !hear(mine, MSG_RESULT, round, &peer_kr)) {
			if (kr == KERN_SUCCESS)
				(void) device_io_port_unclaim(device, RACE_PORT);
			peer_gone = 1;
			break;
		}

		/* The next round's READY from this side is sent after this
		 * release, and the peer claims only after hearing it. */
		if (kr == KERN_SUCCESS)
			(void) device_io_port_unclaim(device, RACE_PORT);

		(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT,
				     BREATH_MS);

		if (!judge)
			continue;

		{
			unsigned wins = (kr == KERN_SUCCESS)
				      + (peer_kr == KERN_SUCCESS);

			if (wins == 1) {
				kern_return_t loser = (kr == KERN_SUCCESS)
					? (kern_return_t)peer_kr : kr;

				behaved++;
				if (loser != KERN_NO_ACCESS)
					odd_refusal++;
			} else if (wins == 2)
				torn++;
			else if (kr == KERN_RESOURCE_SHORTAGE
				 && (kern_return_t)peer_kr
				    == KERN_RESOURCE_SHORTAGE)
				starved++;
			else
				empty++;
		}
	}

	if (!judge) {
		/* The other half says nothing the harness reads; the judge
		 * has both results.  It only records that it took part. */
		printf("io_claim_race b: %u rounds, won %u\n", round, my_wins);
		return 0;
	}

	/* What was actually played, for every line below. */
	if (round < ROUNDS)
		printf("io_claim_race: %u of %u rounds played in %u s%s\n",
		       round, ROUNDS, elapsed,
		       out_of_time ? " — the time budget ended them" : "");

	arms++;
	if (peer_gone) {
		printf("io_claim_race: [1] NOT ASKED — the other half went "
		       "quiet after %u rounds; the question was put %u times "
		       "and answered %u (#563)\n", round, round, behaved);
		arms--;
	} else if (torn != 0 || empty != 0) {
		printf("io_claim_race: [1] WRONG — in %u rounds the table was "
		       "torn %u time%s (two winners) and empty %u time%s "
		       "(no winner, neither for want of a slot)\n", round,
		       torn, torn == 1 ? "" : "s", empty, empty == 1 ? "" : "s");
	} else if (behaved == 0) {
		printf("io_claim_race: [1] NOT ASKED — every one of %u rounds "
		       "found the table full of retiring slots; no round could "
		       "have two winners because none had one (#563)\n",
		       round);
		arms--;
	} else if (ncpu < 2) {
		printf("io_claim_race: [1] NOT ASKED — %u rounds, one winner "
		       "each, on %u processor (#563)\n", round, ncpu);
		printf("io_claim_race:     the scheduler serialised the two "
		       "claims; ask with -smp 2 or more\n");
		arms--;
	} else {
		printf("io_claim_race: [1] %u rounds on %u processors, one "
		       "winner each — two writers never took one slot\n",
		       round, ncpu);
		printf("io_claim_race:     a won %u, b won %u, %u rounds found "
		       "the table full of retiring slots\n",
		       my_wins, behaved - my_wins, starved);
		passed++;
	}

	/*
	 * [2] The loser was told WHY.  A refusal for the right reason is
	 * KERN_NO_ACCESS -- "another task holds it".  Any other code would
	 * mean the loser hit a full table or a bad argument, which is a
	 * different defect wearing the same outcome.
	 */
	if (!peer_gone) {
		arms++;
		if (odd_refusal == 0) {
			printf("io_claim_race: [2] every loser was refused "
			       "with KERN_NO_ACCESS and nothing else\n");
			passed++;
		} else
			printf("io_claim_race: [2] WRONG — %u loser%s refused "
			       "with a code other than KERN_NO_ACCESS\n",
			       odd_refusal, odd_refusal == 1 ? " was" : "s were");
	}

	/*
	 * [3] Nothing leaked.  After the last round the range must be free:
	 * a claim that succeeds now, and a release that finds it, say that
	 * every winner's release was honoured and no retired slot is still
	 * answering.  Asked whether the peer stayed or not.
	 */
	arms++;
	{
		natural_t released = 0, klog_from = 0;

		kr = device_io_port_claim(device, RACE_PORT, RACE_COUNT,
					  &released, &klog_from);
		if (kr == KERN_SUCCESS
		    && device_io_port_unclaim(device, RACE_PORT)
		       == KERN_SUCCESS) {
			printf("io_claim_race: [3] the range is free after "
			       "the last round — no claim leaked\n");
			passed++;
		} else
			printf("io_claim_race: [3] WRONG — the range is not "
			       "clean after the rounds (claim kr=%d)\n",
			       (int)kr);
	}

	printf("io_claim_race: %d of %d arms passed\n", passed, arms);
	return (passed == arms) ? 0 : 1;
}
