/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Who may mount where, and what a reader takes from a mount's server (#716).
 *
 * ── The registry ──
 *
 * A mount was checked in with MACH_PORT_NULL and was anyone's to check out,
 * "/" included; a longer prefix needed nobody's leave, and the look-up takes
 * the longest match, so a task that registered "/etc" answered every open
 * under it.  A prefix is now mounted on a capability for it and unmounted on
 * the signature it was checked in with.
 *
 * [2] to [6] are the two attacks #716 names, from a task whose manifest grants
 * a mount capability -- for /mount_test, which is the wrong one here, a closer
 * miss than holding none: "/" checked out with no signature and with one that
 * is not its owner's, and "/etc" checked in with no token, with a token this
 * task is refused, and with its own.  [7] to [11] are the same rules on its
 * own prefix, where it holds the right capability, and [9] is their control:
 * the right signature and the right token mount.  [12] asks for a path as long
 * as netname takes, which the name server's old 256-byte buffer destroyed
 * unread.  Each refusal is asserted by its whole value, and each is followed
 * by the look-up that shows the registry did not move.
 *
 * ── The reader ──
 *
 * [13] to [19] read and write through libvfs from the mount this task serves
 * (mount_liar.c), which answers wrong on the files that ask it to.  libvfs
 * copied what a server said it had: a Mach read's length, a FLIPC read's
 * length and offset, a write's count.  [13] and [17] are the controls -- the
 * Mach path and the FLIPC path, each shown to be the path that answered --
 * and without them the refusals after each could pass on a path never taken.
 * [20] checks the mount out, which is the control for [11].
 *
 * ── A server that dies registered (#574) ──
 *
 * `mount_test die' is the same program on a line of its own: it checks a name
 * and a mount in and terminates itself with both checked in, which is what a
 * server that faults leaves behind.  The name server kept the mounts of a
 * port that died -- the next look-up of one killed it -- and since #716
 * nobody could check such a record out.  [21] is the presence: the mount
 * there, then the death, seen as this task's right to its port turning dead.
 * [22] to [24] are what must follow: the mount gone with the name server
 * still answering, the name gone, and the prefix free to mount again.  [25]
 * is the notification bootstrap was refused when the name server itself
 * died, asked of the kernel directly (mount_notify.c).
 *
 * [26] is [12]'s question asked of bootstrap: a request as long as its
 * interface takes.  Both servers sized their buffers by hand, the name server
 * at 256 and bootstrap at 128, and a subsystem's maxsize, which they could
 * have used, is its largest reply and not its largest request (#716).
 *
 * ── What a server is handed (#911) ──
 *
 * libvfs sent its task port with every open, and FLIPC's connect sent it to
 * every endpoint: each server held all of each client, to learn when it died.
 * [27] asks what the liar's fs_open is handed now, and has the kernel refuse
 * this task's port offered by hand; [28] the same of FLIPC's connect.  [29]
 * and [30] are what the right was for: a connection ends with its token, when
 * the client lets it go and when the client dies with two of them -- `mount_test
 * die' connects twice to an endpoint this run serves, and dies.  [31] is the
 * real ext_server keeping a client's right through a failed open: it let the
 * right go and so did the request's destruction, and the second release came
 * out of the reference an open file was counting on.
 */

#include <mach.h>
#include <mach/message.h>
#include <mach/mig_errors.h>		/* MIG_SERVER_DIED */
#include <mach/mach_traps.h>
#include <mach/thread_switch.h>
#include <mach/bootstrap.h>		/* [26] */
#include <mach/cap_types.h>
#include <mach_init.h>			/* name_server_port */

#include <stdio.h>
#include <string.h>

#include <servers/netname.h>
#include <servers/netname_defs.h>

#include <libcap.h>
#include <libvfs.h>
#include <vfs_flipc.h>
#include <flipc2.h>
#include <pthread.h>

#include "vfs.h"			/* fs_open(), fs_close(): [27], [31] */
#include "flipc2_endpoint.h"		/* flipc2_endpoint_connect_rpc(): [28] */
#include "mount_test.h"

#define	MT_PREFIX	"/mount_test"
#define	MT_ARMS		31
#define	MT_WAIT_STEPS	1200		/* x 100 ms: "/" has 120 s to appear */

/* #574: what `mount_test die' checks in and dies with, and the word it waits
 * for from the main run before it dies. */
#define	MT_DEAD_PREFIX	"/mount_test_dead"
#define	MT_DEAD_NAME	"mount_test_dead"
#define	MT_SEEN_NAME	"mount_test_seen"

/* #911: the endpoints [29] and [30] connect to, served by this run; `mount_test
 * die' connects to the second twice before it dies. */
#define	MT_OWN_EP	"flipc.mount_test_own"
#define	MT_PAIR_EP	"flipc.mount_test_pair"

static int	passed, arms;

static void
mt_result(int ok)
{
	arms++;
	if (ok)
		passed++;
}

/*
 * One look-up of `path', the reference it hands back given back: the caller
 * compares the name, which outlives that reference for a port this task holds
 * another right to -- the receive right of its own mount, or the reference [1]
 * keeps to "/"'s.
 */
static kern_return_t
mt_look_up(const char *path, mach_port_t *port, netname_name_t matched)
{
	kern_return_t kr;

	*port = MACH_PORT_NULL;
	matched[0] = '\0';
	kr = netname_look_up_mount(name_server_port, (char *)path, port,
				   matched);
	if (kr == NETNAME_SUCCESS && MACH_PORT_VALID(*port))
		(void)mach_port_deallocate(mach_task_self(), *port);
	return kr;
}

/* Whether `path' still resolves to `want' under `prefix'. */
static int
mt_resolves(const char *path, mach_port_t want, const char *prefix)
{
	netname_name_t	matched;
	mach_port_t	got;

	return mt_look_up(path, &got, matched) == NETNAME_SUCCESS &&
	       got == want && strcmp(matched, prefix) == 0;
}

static mach_port_t
another_port(void)
{
	mach_port_t p = MACH_PORT_NULL;

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p)
	    != KERN_SUCCESS)
		return MACH_PORT_NULL;
	if (mach_port_insert_right(mach_task_self(), p, p,
				   MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS)
		return MACH_PORT_NULL;
	return p;
}

static int
mt_all(const char *buf, char c, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (buf[i] != c)
			return 0;
	return 1;
}

/* What a read into this buffer may write past its first LIAR_SIZE bytes. */
static struct {
	char	buf[LIAR_SIZE];
	char	guard[2 * LIAR_MORE_BY];
} mt_read;

static void
mt_read_reset(void)
{
	memset(mt_read.buf, 0, sizeof mt_read.buf);
	memset(mt_read.guard, 0xA5, sizeof mt_read.guard);
}

static int
mt_guard_intact(void)
{
	return mt_all(mt_read.guard, (char)0xA5, sizeof mt_read.guard);
}

static void
mt_print_buf(char *out, size_t size)
{
	size_t i;

	for (i = 0; i < LIAR_SIZE && i + 1 < size; i++) {
		unsigned char c = (unsigned char)mt_read.buf[i];
		out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
	}
	out[i] = '\0';
}

/*
 * #911: an endpoint this run serves for [29] and [30], and what its pump has
 * seen: connections made, and connections ended -- by a token's dead name,
 * the only way a connection ends now.
 */
struct mt_ep {
	const char		*name;
	uint32_t		max;
	flipc2_endpoint_t	ep;
	volatile uint32_t	news;
	volatile uint32_t	deads;
};

static struct mt_ep	mt_own = { MT_OWN_EP, 1, 0, 0, 0 };
static struct mt_ep	mt_pair = { MT_PAIR_EP, 2, 0, 0, 0 };

static void *
mt_ep_pump(void *arg)
{
	struct mt_ep		*e = arg;
	flipc2_ep_event_t	ev;

	for (;;) {
		if (flipc2_endpoint_pump(e->ep, 100, &ev) != FLIPC2_SUCCESS) {
			thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 100);
			continue;
		}
		if (ev.type == FLIPC2_EP_EVENT_NEW) {
			e->news++;
		} else if (ev.type == FLIPC2_EP_EVENT_DEAD) {
			flipc2_channel_destroy(ev.fwd_ch);
			flipc2_channel_destroy(ev.rev_ch);
			e->deads++;
		}
	}
	return NULL;
}

/* NULL, or what failed. */
static const char *
mt_ep_start(struct mt_ep *e)
{
	pthread_t	t;

	if (flipc2_endpoint_create(e->name, e->max, VFS_FLIPC_CHANNEL_SIZE,
				   VFS_FLIPC_RING_ENTRIES, 0, &e->ep)
	    != FLIPC2_SUCCESS)
		return "flipc2_endpoint_create failed";
	if (pthread_create(&t, NULL, mt_ep_pump, e) != 0)
		return "pthread_create failed";
	return NULL;
}

/* Wait up to `tenths' tenths of a second for `*counter' to reach `want'. */
static int
mt_reaches(volatile uint32_t *counter, uint32_t want, unsigned int tenths)
{
	unsigned int	i;

	for (i = 0; i < tenths && *counter < want; i++)
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 100);
	return *counter >= want;
}

/*
 * `mount_test die''s half of [30]: two connections to the main run's pair
 * endpoint, which this task's death must end, both of them.  The endpoint may
 * not be up yet, so the first connect is tried until it is.  How many were
 * made.
 */
static int
mt_die_connect(void)
{
	flipc2_channel_t	fwd, rev;
	flipc2_return_t		ret = FLIPC2_ERR_NAME_NOT_FOUND;
	unsigned int		step;
	int			made = 0;

	for (step = 0; step < MT_WAIT_STEPS; step++) {
		ret = flipc2_endpoint_connect(MT_PAIR_EP, 0, 0, &fwd, &rev);
		if (ret != FLIPC2_ERR_NAME_NOT_FOUND)
			break;
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 100);
	}
	if (ret == FLIPC2_SUCCESS) {
		made++;
		ret = flipc2_endpoint_connect(MT_PAIR_EP, 0, 0, &fwd, &rev);
		if (ret == FLIPC2_SUCCESS)
			made++;
	}
	if (made == 2)
		printf("mount_test (die): two connections to " MT_PAIR_EP
		       "\n");
	else
		printf("mount_test (die): WRONG — %d of 2 connections to "
		       MT_PAIR_EP ", the last answered %d\n", made, ret);
	return made;
}

/*
 * `mount_test die' (#574): check a name and a mount in, both signed, wait for
 * the main run to say it has seen the mount, and terminate this task with both
 * still checked in -- no exit path and no cleanup, as a server that faults.
 * Since #911 it also connects twice to the main run's pair endpoint first, and
 * dies with both connections open.
 */
static int
mt_die(void)
{
	mach_port_t	port = MACH_PORT_NULL, sig = MACH_PORT_NULL, seen;
	struct uros_cap	tok;
	char		blob[CAP_TOKEN_MAX];
	kern_return_t	km = KERN_FAILURE, kn = KERN_FAILURE;
	unsigned int	step;

	printf("mount_test (die): started (#574)\n");
	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			       &port) == KERN_SUCCESS &&
	    mach_port_insert_right(mach_task_self(), port, port,
				   MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS &&
	    urmach_netname_signature(&sig) == KERN_SUCCESS) {
		km = cap_request(RESOURCE_MOUNT_POINT,
				 cap_name_hash(MT_DEAD_PREFIX),
				 CAP_OP_MOUNT_REGISTER, 0, &tok);
		if (km == KERN_SUCCESS) {
			memcpy(blob, &tok, sizeof tok);
			km = netname_check_in_mount(name_server_port,
						    MT_DEAD_PREFIX, sig, port,
						    blob,
						    (mach_msg_type_number_t)
						    sizeof tok);
		}
		kn = netname_check_in(name_server_port, MT_DEAD_NAME, sig,
				      port);
	}
	if (km == KERN_SUCCESS && kn == KERN_SUCCESS)
		printf("mount_test (die): " MT_DEAD_PREFIX " and \""
		       MT_DEAD_NAME "\" checked in\n");
	else
		printf("mount_test (die): WRONG — the mount: %d, the name: %d\n",
		       km, kn);

	(void)mt_die_connect();			/* [30], #911 */

	for (step = 0; step < MT_WAIT_STEPS; step++) {
		seen = MACH_PORT_NULL;
		if (netname_look_up(name_server_port, "", MT_SEEN_NAME, &seen)
		    == NETNAME_SUCCESS) {
			(void)mach_port_deallocate(mach_task_self(), seen);
			break;
		}
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 100);
	}
	if (step == MT_WAIT_STEPS)
		printf("mount_test (die): WRONG — the main run never said it "
		       "had seen the mount, after %u s\n", MT_WAIT_STEPS / 10);
	printf("mount_test (die): dying with both still checked in\n");
	(void)task_terminate(mach_task_self());
	return 1;
}

/*
 * [21] to [24] (#574), with this run's signature and the spare port it mounts
 * with.  See the top of this file.
 */
static void
mt_death_arms(mach_port_t sig, mach_port_t other)
{
	netname_name_t	matched;
	mach_port_t	dead = MACH_PORT_NULL, got;
	mach_port_type_t type;
	struct uros_cap	tok;
	char		blob[CAP_TOKEN_MAX];
	kern_return_t	kr = KERN_FAILURE;
	unsigned int	step;
	int		seen = 0, signalled = 0, died = 0;

	/* [21] The mount, then its server's death. */
	for (step = 0; step < MT_WAIT_STEPS; step++) {
		dead = MACH_PORT_NULL;
		matched[0] = '\0';
		kr = netname_look_up_mount(name_server_port,
					   MT_DEAD_PREFIX "/x", &dead, matched);
		if (kr == NETNAME_SUCCESS && MACH_PORT_VALID(dead) &&
		    strcmp(matched, MT_DEAD_PREFIX) == 0) {
			seen = 1;
			break;
		}
		if (kr == NETNAME_SUCCESS && MACH_PORT_VALID(dead))
			(void)mach_port_deallocate(mach_task_self(), dead);
		dead = MACH_PORT_NULL;
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 100);
	}
	if (seen)
		signalled = (netname_check_in(name_server_port, MT_SEEN_NAME,
					      sig, other) == NETNAME_SUCCESS);
	for (step = 0; signalled && step < MT_WAIT_STEPS; step++) {
		if (mach_port_type(mach_task_self(), dead, &type)
		    == KERN_SUCCESS && (type & MACH_PORT_TYPE_DEAD_NAME)) {
			died = 1;
			break;
		}
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 100);
	}
	mt_result(died);
	if (died)
		printf("mount_test: [21] " MT_DEAD_PREFIX " was mounted by "
		       "`mount_test die', which then terminated with it "
		       "checked in: this task's right to its port is dead\n");
	else
		printf("mount_test: [21] WRONG — %s\n",
		       !seen ? "`mount_test die' never mounted " MT_DEAD_PREFIX
		       : !signalled ? "could not check \"" MT_SEEN_NAME "\" in "
		       "to let it die" : "its port never died");

	/* [22] The mount went with its server, and the name server answers. */
	if (!died) {
		printf("mount_test: [22] WRONG — not asked: no death\n");
		mt_result(0);
	} else {
		for (step = 0; step < 50; step++) {
			kr = mt_look_up(MT_DEAD_PREFIX "/x", &got, matched);
			if (kr != NETNAME_SUCCESS ||
			    strcmp(matched, MT_DEAD_PREFIX) != 0)
				break;
			thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 100);
		}
		if (kr == NETNAME_SUCCESS && strcmp(matched, "/") == 0) {
			printf("mount_test: [22] after its server's death "
			       MT_DEAD_PREFIX "/x resolves to \"/\": the mount "
			       "went with it, and the name server answers\n");
			mt_result(1);
		} else {
			printf("mount_test: [22] WRONG — after its server's death "
			       "a look-up under " MT_DEAD_PREFIX ": %d, matched "
			       "\"%s\"%s\n", kr, matched,
			       kr == MIG_SERVER_DIED ? ": the name server died "
			       "answering it" : "");
			mt_result(0);
		}
	}

	/* [23] The name went with it too. */
	if (!died) {
		printf("mount_test: [23] WRONG — not asked: no death\n");
		mt_result(0);
	} else {
		got = MACH_PORT_NULL;
		kr = netname_look_up(name_server_port, "", MT_DEAD_NAME, &got);
		if (kr == NETNAME_SUCCESS && MACH_PORT_VALID(got))
			(void)mach_port_deallocate(mach_task_self(), got);
		if (kr == NETNAME_NOT_CHECKED_IN) {
			printf("mount_test: [23] \"" MT_DEAD_NAME "\" is gone "
			       "with its server: NETNAME_NOT_CHECKED_IN\n");
			mt_result(1);
		} else {
			printf("mount_test: [23] WRONG — \"" MT_DEAD_NAME "\" "
			       "after its server's death: %d, not "
			       "NETNAME_NOT_CHECKED_IN (%d)\n", kr,
			       NETNAME_NOT_CHECKED_IN);
			mt_result(0);
		}
	}

	/* [24] And the prefix is free: this run mounts it, and takes it out. */
	if (!died) {
		printf("mount_test: [24] WRONG — not asked: no death\n");
		mt_result(0);
	} else {
		kr = cap_request(RESOURCE_MOUNT_POINT,
				 cap_name_hash(MT_DEAD_PREFIX),
				 CAP_OP_MOUNT_REGISTER, 0, &tok);
		if (kr == KERN_SUCCESS) {
			memcpy(blob, &tok, sizeof tok);
			kr = netname_check_in_mount(name_server_port,
						    MT_DEAD_PREFIX, sig, other,
						    blob,
						    (mach_msg_type_number_t)
						    sizeof tok);
		}
		if (kr == NETNAME_SUCCESS &&
		    netname_check_out_mount(name_server_port, MT_DEAD_PREFIX,
					    sig) == NETNAME_SUCCESS) {
			printf("mount_test: [24] " MT_DEAD_PREFIX " mounted again "
			       "by this task, and checked out\n");
			mt_result(1);
		} else {
			printf("mount_test: [24] WRONG — " MT_DEAD_PREFIX " "
			       "mounted again by this task: %d%s\n", kr,
			       kr == NETNAME_MOUNT_EXISTS ? ", the dead server's "
			       "mount still in the way" : "");
			mt_result(0);
		}
	}

	if (signalled)
		(void)netname_check_out(name_server_port, MT_SEEN_NAME, sig);
	if (MACH_PORT_VALID(dead))
		(void)mach_port_deallocate(mach_task_self(), dead);
}

/*
 * #911 [31]: whether a no-senders notification is waiting on `notify', within
 * `tenths' tenths of a second.
 */
static int
mt_no_senders(mach_port_t notify, unsigned int tenths)
{
	union {
		mach_msg_header_t	h;
		char			bytes[256];
	} m;
	mach_msg_return_t	mr;

	mr = mach_msg(&m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof m,
		      notify, tenths * 100, MACH_PORT_NULL);
	return mr == MACH_MSG_SUCCESS &&
	       m.h.msgh_id == MACH_NOTIFY_NO_SENDERS;
}

/*
 * [27] to [31] (#911), with the liar's port, "/"'s when there is one, and why
 * the two endpoints did not start when they did not.  See the top of this
 * file.
 */
static void
mt_rights_arms(mach_port_t liar, mach_port_t root, int have_root,
	       const char *eps_why)
{
	uint32_t		opens = liar_opens;
	uint32_t		tasks = liar_opens_with_a_task;
	mach_port_t		ep = MACH_PORT_NULL;
	mach_port_t		fe = MACH_PORT_NULL, re = MACH_PORT_NULL;
	mach_port_t		token = MACH_PORT_NULL, notify = MACH_PORT_NULL;
	mach_port_t		prev = MACH_PORT_NULL;
	flipc2_channel_t	f1, r1, f2, r2;
	flipc2_return_t		first, full, again, a, b;
	vfs_u64_t		handle = 0, absent = 0;
	vfs_u32_t		type = 0;
	unsigned32		so = 0, eo = 0;
	kern_return_t		raw = KERN_FAILURE, kr, kn = KERN_FAILURE;
	kern_return_t		closed = KERN_FAILURE;
	int			control, early = 0, late = 0, ok;

	/*
	 * [27] fs_open hands the liar no task port.  Every open [13] to [19]
	 * made of /mount_test reached it, and it asked the right each one came
	 * with; [20] has unmounted it since, so the offer by hand goes to its
	 * port, which it still serves.  The control is the probe asked of this
	 * task's own port: if it could not see a task port where there is one,
	 * "none" would say nothing.
	 */
	control = mt_is_task(mach_task_self());
	raw = fs_open(liar, mach_task_self(), "/honest", VFS_O_RDONLY, 0,
		      &handle, &type);
	if (raw == KERN_SUCCESS)
		(void)fs_close(liar, handle);
	ok = control && opens > 0 && tasks == 0 &&
	     raw == MACH_SEND_INVALID_RIGHT;
	mt_result(ok);
	if (ok)
		printf("mount_test: [27] %u opens handed the liar a right that "
		       "is no task port, and this task's port offered by hand is "
		       "refused by the kernel: MACH_SEND_INVALID_RIGHT\n",
		       (unsigned)opens);
	else
		printf("mount_test: [27] WRONG — %u open(s) answered, %u with a "
		       "task port; this task's port offered by hand: %d, not "
		       "MACH_SEND_INVALID_RIGHT (%d)%s\n", (unsigned)opens,
		       (unsigned)tasks, raw, MACH_SEND_INVALID_RIGHT,
		       control ? "" : "; and the probe does not see this "
		       "task's own port as a task");

	/*
	 * [28] FLIPC's connect: the same offer to the liar's endpoint.  Its
	 * one slot is libvfs's since [17], so a request that got through would
	 * be answered KERN_RESOURCE_SHORTAGE and take nothing.
	 */
	raw = KERN_FAILURE;
	kr = netname_look_up(name_server_port, "",
			     VFS_FLIPC_ENDPOINT_PREFIX "mount_test", &ep);
	if (kr == NETNAME_SUCCESS && MACH_PORT_VALID(ep)) {
		raw = flipc2_endpoint_connect_rpc(ep, mach_task_self(), 0, 0,
						  &fe, &re, &so, &eo);
		if (raw == KERN_SUCCESS) {
			(void)mach_port_deallocate(mach_task_self(), fe);
			(void)mach_port_deallocate(mach_task_self(), re);
		}
		(void)mach_port_deallocate(mach_task_self(), ep);
	}
	mt_result(raw == MACH_SEND_INVALID_RIGHT);
	if (raw == MACH_SEND_INVALID_RIGHT)
		printf("mount_test: [28] this task's port offered to a FLIPC "
		       "endpoint as a connection's token is refused by the "
		       "kernel: MACH_SEND_INVALID_RIGHT\n");
	else
		printf("mount_test: [28] WRONG — this task's port offered to a "
		       "FLIPC endpoint as a connection's token: %d (look-up %d), "
		       "not MACH_SEND_INVALID_RIGHT (%d)\n", raw, kr,
		       MACH_SEND_INVALID_RIGHT);

	/*
	 * [29] A connection ends with its token.  The endpoint holds one; the
	 * control is a second connect refused while the first is held, without
	 * which a reconnect would show nothing.
	 */
	if (eps_why != NULL) {
		printf("mount_test: [29] WRONG — not asked: %s\n", eps_why);
		mt_result(0);
	} else {
		first = flipc2_endpoint_connect(MT_OWN_EP, 0, 0, &f1, &r1);
		full = flipc2_endpoint_connect(MT_OWN_EP, 0, 0, &f2, &r2);
		if (full == FLIPC2_SUCCESS)
			(void)flipc2_endpoint_disconnect(f2, r2);
		if (first == FLIPC2_SUCCESS)
			(void)flipc2_endpoint_disconnect(f1, r1);
		ok = mt_reaches(&mt_own.deads, 1, 50);
		again = flipc2_endpoint_connect(MT_OWN_EP, 0, 0, &f2, &r2);
		if (again == FLIPC2_SUCCESS)
			(void)flipc2_endpoint_disconnect(f2, r2);
		ok = ok && first == FLIPC2_SUCCESS &&
		     full == FLIPC2_ERR_MAX_CLIENTS && again == FLIPC2_SUCCESS;
		mt_result(ok);
		if (ok)
			printf("mount_test: [29] an endpoint of one slot, held: "
			       "a second connect refused, and once the first let "
			       "its token go, the endpoint saw it end and took a "
			       "new one\n");
		else
			printf("mount_test: [29] WRONG — a connect: %d; another "
			       "while it held the slot: %d, not "
			       "FLIPC2_ERR_MAX_CLIENTS (%d); ended seen %u time(s); "
			       "a connect after: %d\n", first, full,
			       FLIPC2_ERR_MAX_CLIENTS, (unsigned)mt_own.deads,
			       again);
	}

	/*
	 * [30] `mount_test die' connected twice and died: both connections
	 * end, and both slots take a new one.
	 */
	if (eps_why != NULL) {
		printf("mount_test: [30] WRONG — not asked: %s\n", eps_why);
		mt_result(0);
	} else {
		ok = mt_reaches(&mt_pair.deads, 2, 100);
		a = flipc2_endpoint_connect(MT_PAIR_EP, 0, 0, &f1, &r1);
		b = flipc2_endpoint_connect(MT_PAIR_EP, 0, 0, &f2, &r2);
		if (a == FLIPC2_SUCCESS)
			(void)flipc2_endpoint_disconnect(f1, r1);
		if (b == FLIPC2_SUCCESS)
			(void)flipc2_endpoint_disconnect(f2, r2);
		ok = ok && mt_pair.news >= 2 && a == FLIPC2_SUCCESS &&
		     b == FLIPC2_SUCCESS;
		mt_result(ok);
		if (ok)
			printf("mount_test: [30] `mount_test die' connected twice "
			       "and died: the endpoint saw both connections end, "
			       "and both slots took a new one\n");
		else
			printf("mount_test: [30] WRONG — `mount_test die' made %u "
			       "connection(s), %u seen to end; two new ones: %d "
			       "and %d\n", (unsigned)mt_pair.news,
			       (unsigned)mt_pair.deads, a, b);
	}

	/*
	 * [31] ext_server keeps a client's right through a failed open.  The
	 * token is this arm's own, so its only send rights are the ones
	 * ext_server holds: an open of a directory every "/" has, the
	 * notification asked for, a failed open, and no notification -- then
	 * the close lets the last right go, and the notification that comes is
	 * the control.
	 */
	if (!have_root) {
		printf("mount_test: [31] WRONG — not asked: no \"/\"\n");
		mt_result(0);
	} else {
		kr = mach_port_allocate(mach_task_self(),
					MACH_PORT_RIGHT_RECEIVE, &token);
		if (kr == KERN_SUCCESS)
			kr = mach_port_allocate(mach_task_self(),
						MACH_PORT_RIGHT_RECEIVE,
						&notify);
		if (kr == KERN_SUCCESS)
			kr = fs_open(root, token, "/mach_servers",
				     VFS_O_RDONLY, 0, &handle, &type);
		if (kr == KERN_SUCCESS)
			kr = mach_port_request_notification(mach_task_self(),
				token, MACH_NOTIFY_NO_SENDERS, 1, notify,
				MACH_MSG_TYPE_MAKE_SEND_ONCE, &prev);
		if (kr == KERN_SUCCESS) {
			kn = fs_open(root, token, "/mount_test_911_absent",
				     VFS_O_RDONLY, 0, &absent, &type);
			early = mt_no_senders(notify, 2);
		}
		if (kr == KERN_SUCCESS) {
			closed = fs_close(root, handle);
			late = mt_no_senders(notify, 20);
		}
		if (MACH_PORT_VALID(token))
			(void)mach_port_mod_refs(mach_task_self(), token,
						 MACH_PORT_RIGHT_RECEIVE, -1);
		if (MACH_PORT_VALID(notify))
			(void)mach_port_mod_refs(mach_task_self(), notify,
						 MACH_PORT_RIGHT_RECEIVE, -1);
		ok = kr == KERN_SUCCESS && kn != KERN_SUCCESS && !early &&
		     closed == KERN_SUCCESS && late;
		mt_result(ok);
		if (ok)
			printf("mount_test: [31] ext_server held a client's right "
			       "through a failed open (%d) and let it go at the "
			       "close\n", kn);
		else if (kr != KERN_SUCCESS)
			printf("mount_test: [31] WRONG — not asked: the open of "
			       "/mach_servers or the notification failed (%d)\n",
			       kr);
		else
			printf("mount_test: [31] WRONG — after a failed open "
			       "(%d) ext_server held %s of the client's right; "
			       "the close: %d, %s\n", kn,
			       early ? "NONE" : "some",
			       closed, late ? "and then none"
			       : "and still some -- the control failed");
	}
}

int
main(int argc, char **argv)
{
	netname_name_t	matched;
	mach_port_t	root = MACH_PORT_NULL, mine = MACH_PORT_NULL;
	mach_port_t	sig = MACH_PORT_NULL, other = MACH_PORT_NULL, got;
	struct uros_cap	tok;
	char		tok_blob[CAP_TOKEN_MAX];
	int		have_root = 0, have_tok = 0, mounted = 0;
	const char	*why, *eps_why;
	kern_return_t	kr;
	unsigned int	step;
	char		shown[LIAR_SIZE + 1];

	if (argc > 1 && argv != NULL && argv[1] != NULL &&
	    strcmp(argv[1], "die") == 0)
		return mt_die();
	printf("mount_test: started (#716)\n");
	memset(tok_blob, 0, sizeof tok_blob);

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			       &mine) != KERN_SUCCESS ||
	    mach_port_insert_right(mach_task_self(), mine, mine,
				   MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS ||
	    urmach_netname_signature(&sig) != KERN_SUCCESS ||
	    (other = another_port()) == MACH_PORT_NULL) {
		printf("mount_test: WRONG — no port or no signature\n");
		printf("mount_test: 0 of %d arms passed\n", MT_ARMS);
		return 1;
	}
	why = liar_start(mine);
	if (why != NULL) {
		printf("mount_test: WRONG — the mount's server did not start: "
		       "%s\n", why);
		printf("mount_test: 0 of %d arms passed\n", MT_ARMS);
		return 1;
	}

	/* #911: [29]'s and [30]'s endpoints, early -- `mount_test die' looks
	 * for the second from the moment it starts. */
	eps_why = mt_ep_start(&mt_own);
	if (eps_why == NULL)
		eps_why = mt_ep_start(&mt_pair);

	/*
	 * [1] "/" is ext_server's.  The presence every arm about "/" stands on:
	 * a refusal to take over a mount that is not there shows nothing.
	 */
	for (step = 0; step < MT_WAIT_STEPS; step++) {
		root = MACH_PORT_NULL;
		matched[0] = '\0';
		kr = netname_look_up_mount(name_server_port, "/", &root,
					   matched);
		if (kr == NETNAME_SUCCESS && MACH_PORT_VALID(root))
			break;
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 100);
	}
	have_root = (step < MT_WAIT_STEPS && strcmp(matched, "/") == 0);
	mt_result(have_root);
	if (have_root)
		printf("mount_test: [1] \"/\" is mounted, port 0x%x\n",
		       (unsigned)root);
	else
		printf("mount_test: [1] WRONG — no \"/\" mount after %u s "
		       "(kr=%d, matched \"%s\")\n", MT_WAIT_STEPS / 10, kr,
		       matched);

	/* [2] "/" checked out with no signature. */
	if (!have_root) {
		printf("mount_test: [2] WRONG — not asked: no \"/\"\n");
		mt_result(0);
	} else {
		kr = netname_check_out_mount(name_server_port, "/",
					     MACH_PORT_NULL);
		if (kr == NETNAME_INVALID_PORT && mt_resolves("/", root, "/")) {
			printf("mount_test: [2] \"/\" checked out with no "
			       "signature: NETNAME_INVALID_PORT, and \"/\" is "
			       "still 0x%x's\n", (unsigned)root);
			mt_result(1);
		} else {
			printf("mount_test: [2] WRONG — \"/\" checked out with "
			       "no signature: %d, not NETNAME_INVALID_PORT "
			       "(%d)%s\n", kr, NETNAME_INVALID_PORT,
			       mt_resolves("/", root, "/") ? ""
			       : ", and \"/\" is no longer ext_server's");
			mt_result(0);
		}
	}

	/* [3] "/" checked out with a signature that is not its owner's. */
	if (!have_root) {
		printf("mount_test: [3] WRONG — not asked: no \"/\"\n");
		mt_result(0);
	} else {
		kr = netname_check_out_mount(name_server_port, "/", sig);
		if (kr == NETNAME_NOT_YOURS && mt_resolves("/", root, "/")) {
			printf("mount_test: [3] \"/\" checked out with this "
			       "task's signature: NETNAME_NOT_YOURS, and \"/\" "
			       "is still 0x%x's\n", (unsigned)root);
			mt_result(1);
		} else {
			printf("mount_test: [3] WRONG — \"/\" checked out with "
			       "this task's signature: %d, not "
			       "NETNAME_NOT_YOURS (%d)%s\n", kr,
			       NETNAME_NOT_YOURS,
			       mt_resolves("/", root, "/") ? ""
			       : ", and \"/\" is no longer ext_server's");
			mt_result(0);
		}
	}

	/*
	 * [4] "/etc" checked in with no capability.  An empty token: what a
	 * task that never asked cap_server for one has to send.
	 */
	kr = netname_check_in_mount(name_server_port, "/etc", sig, mine,
				    tok_blob, 0);
	if (kr == CAP_ERR_INVALID_TOKEN &&
	    (!have_root || mt_resolves("/etc/passwd", root, "/"))) {
		printf("mount_test: [4] \"/etc\" checked in with no "
		       "capability: CAP_ERR_INVALID_TOKEN, and /etc/passwd "
		       "is still \"/\"'s\n");
		mt_result(1);
	} else {
		printf("mount_test: [4] WRONG — \"/etc\" checked in with no "
		       "capability: %d, not CAP_ERR_INVALID_TOKEN (%d)\n", kr,
		       CAP_ERR_INVALID_TOKEN);
		if (kr == NETNAME_SUCCESS)
			(void)netname_check_out_mount(name_server_port, "/etc",
						      sig);
		mt_result(0);
	}

	/* [5] A capability for "/etc", asked of cap_server: not this task's. */
	kr = cap_request(RESOURCE_MOUNT_POINT, cap_name_hash("/etc"),
			 CAP_OP_MOUNT_REGISTER, 0, &tok);
	if (kr == CAP_ERR_NOT_IN_MANIFEST) {
		printf("mount_test: [5] a capability for \"/etc\" is refused by "
		       "cap_server: CAP_ERR_NOT_IN_MANIFEST\n");
		mt_result(1);
	} else {
		printf("mount_test: [5] WRONG — a capability for \"/etc\": %d, "
		       "not CAP_ERR_NOT_IN_MANIFEST (%d)\n", kr,
		       CAP_ERR_NOT_IN_MANIFEST);
		mt_result(0);
	}

	/*
	 * [6] "/etc" checked in with this task's own capability, which is for
	 * /mount_test.  The token is valid -- only its prefix is not this one.
	 */
	kr = cap_request(RESOURCE_MOUNT_POINT, cap_name_hash(MT_PREFIX),
			 CAP_OP_MOUNT_REGISTER, 0, &tok);
	have_tok = (kr == KERN_SUCCESS);
	if (have_tok)
		memcpy(tok_blob, &tok, sizeof tok);
	else
		printf("mount_test: WRONG — this task's manifest grants "
		       MT_PREFIX ", and cap_server refused it: %d\n", kr);
	if (!have_tok) {
		printf("mount_test: [6] WRONG — not asked: no capability\n");
		mt_result(0);
	} else {
		kr = netname_check_in_mount(name_server_port, "/etc", sig,
					    mine, tok_blob,
					    (mach_msg_type_number_t)sizeof tok);
		if (kr == CAP_ERR_RESOURCE_MISMATCH &&
		    (!have_root || mt_resolves("/etc/passwd", root, "/"))) {
			printf("mount_test: [6] \"/etc\" checked in with the "
			       "capability for " MT_PREFIX ": "
			       "CAP_ERR_RESOURCE_MISMATCH, and /etc/passwd is "
			       "still \"/\"'s\n");
			mt_result(1);
		} else {
			printf("mount_test: [6] WRONG — \"/etc\" checked in "
			       "with the capability for " MT_PREFIX ": %d, not "
			       "CAP_ERR_RESOURCE_MISMATCH (%d)\n", kr,
			       CAP_ERR_RESOURCE_MISMATCH);
			if (kr == NETNAME_SUCCESS)
				(void)netname_check_out_mount(
					name_server_port, "/etc", sig);
			mt_result(0);
		}
	}

	/* [7] Its own prefix, with the right capability and no signature. */
	if (!have_tok) {
		printf("mount_test: [7] WRONG — not asked: no capability\n");
		mt_result(0);
	} else {
		kr = netname_check_in_mount(name_server_port, MT_PREFIX,
					    MACH_PORT_NULL, mine, tok_blob,
					    (mach_msg_type_number_t)sizeof tok);
		if (kr == NETNAME_INVALID_PORT &&
		    mt_look_up(MT_PREFIX "/honest", &got, matched)
		    == NETNAME_SUCCESS && strcmp(matched, MT_PREFIX) != 0) {
			printf("mount_test: [7] " MT_PREFIX " checked in with no "
			       "signature: NETNAME_INVALID_PORT\n");
			mt_result(1);
		} else {
			printf("mount_test: [7] WRONG — " MT_PREFIX " checked in "
			       "with no signature: %d, not "
			       "NETNAME_INVALID_PORT (%d)\n", kr,
			       NETNAME_INVALID_PORT);
			if (kr == NETNAME_SUCCESS)
				(void)netname_check_out_mount(
					name_server_port, MT_PREFIX,
					MACH_PORT_NULL);
			mt_result(0);
		}
	}

	/*
	 * [8] Signed by the port being mounted, which every task that looks
	 * the mount up holds a send right to.
	 */
	if (!have_tok) {
		printf("mount_test: [8] WRONG — not asked: no capability\n");
		mt_result(0);
	} else {
		kr = netname_check_in_mount(name_server_port, MT_PREFIX, mine,
					    mine, tok_blob,
					    (mach_msg_type_number_t)sizeof tok);
		if (kr == NETNAME_INVALID_PORT) {
			printf("mount_test: [8] " MT_PREFIX " checked in signed "
			       "by the port being mounted: "
			       "NETNAME_INVALID_PORT\n");
			mt_result(1);
		} else {
			printf("mount_test: [8] WRONG — " MT_PREFIX " checked in "
			       "signed by the port being mounted: %d, not "
			       "NETNAME_INVALID_PORT (%d)\n", kr,
			       NETNAME_INVALID_PORT);
			if (kr == NETNAME_SUCCESS)
				(void)netname_check_out_mount(
					name_server_port, MT_PREFIX, mine);
			mt_result(0);
		}
	}

	/* [9] The control for [7] and [8]: signature and capability both. */
	if (!have_tok) {
		printf("mount_test: [9] WRONG — not asked: no capability\n");
		mt_result(0);
	} else {
		kr = netname_check_in_mount(name_server_port, MT_PREFIX, sig,
					    mine, tok_blob,
					    (mach_msg_type_number_t)sizeof tok);
		mounted = (kr == NETNAME_SUCCESS);
		if (mounted &&
		    mt_resolves(MT_PREFIX "/honest", mine, MT_PREFIX)) {
			printf("mount_test: [9] " MT_PREFIX " checked in with "
			       "this task's signature and capability, and "
			       MT_PREFIX "/honest resolves to it\n");
			mt_result(1);
		} else {
			printf("mount_test: [9] WRONG — " MT_PREFIX " checked in "
			       "with this task's signature and capability: "
			       "%d%s\n", kr, mounted
			       ? ", and a look-up under it does not reach it"
			       : "");
			mt_result(0);
		}
	}

	/* [10] A second mount of a prefix that has one, capability or not. */
	if (!mounted) {
		printf("mount_test: [10] WRONG — not asked: [9] did not "
		       "mount\n");
		mt_result(0);
	} else {
		kr = netname_check_in_mount(name_server_port, MT_PREFIX, sig,
					    other, tok_blob,
					    (mach_msg_type_number_t)sizeof tok);
		if (kr == NETNAME_MOUNT_EXISTS &&
		    mt_resolves(MT_PREFIX "/honest", mine, MT_PREFIX)) {
			printf("mount_test: [10] " MT_PREFIX " checked in again "
			       "for another port: NETNAME_MOUNT_EXISTS\n");
			mt_result(1);
		} else {
			printf("mount_test: [10] WRONG — " MT_PREFIX " checked "
			       "in again for another port: %d, not "
			       "NETNAME_MOUNT_EXISTS (%d)\n", kr,
			       NETNAME_MOUNT_EXISTS);
			mt_result(0);
		}
	}

	/* [11] Its own mount checked out with no signature, and another. */
	if (!mounted) {
		printf("mount_test: [11] WRONG — not asked: [9] did not "
		       "mount\n");
		mt_result(0);
	} else {
		kern_return_t kr2;

		kr = netname_check_out_mount(name_server_port, MT_PREFIX,
					     MACH_PORT_NULL);
		kr2 = netname_check_out_mount(name_server_port, MT_PREFIX,
					      other);
		if (kr == NETNAME_INVALID_PORT && kr2 == NETNAME_NOT_YOURS &&
		    mt_resolves(MT_PREFIX "/honest", mine, MT_PREFIX)) {
			printf("mount_test: [11] " MT_PREFIX " checked out with "
			       "no signature: NETNAME_INVALID_PORT; with "
			       "another: NETNAME_NOT_YOURS; and it is still "
			       "mounted\n");
			mt_result(1);
		} else {
			printf("mount_test: [11] WRONG — " MT_PREFIX " checked "
			       "out with no signature: %d (not %d), with "
			       "another: %d (not %d)\n", kr,
			       NETNAME_INVALID_PORT, kr2, NETNAME_NOT_YOURS);
			mounted = mt_resolves(MT_PREFIX "/honest", mine,
					      MT_PREFIX);
			mt_result(0);
		}
	}

	/*
	 * [12] A path as long as netname_look_up_mount takes.  The name server
	 * received into 256 bytes, and the kernel destroyed a longer request
	 * unread: MIG_SERVER_DIED, from a server that was alive.
	 */
	{
		static char	path[NETNAME_PATH_MAX];
		size_t		len = sizeof MT_PREFIX "/" - 1;

		memcpy(path, MT_PREFIX "/", len);
		memset(path + len, 'a', sizeof path - 1 - len);
		path[sizeof path - 1] = '\0';
		kr = mt_look_up(path, &got, matched);
		if (kr == NETNAME_SUCCESS &&
		    (!mounted || (got == mine &&
				  strcmp(matched, MT_PREFIX) == 0))) {
			printf("mount_test: [12] a look-up of a %u-byte path is "
			       "answered, by \"%s\"\n",
			       (unsigned)(sizeof path - 1), matched);
			mt_result(1);
		} else {
			printf("mount_test: [12] WRONG — a look-up of a %u-byte "
			       "path: %d, matched \"%s\"\n",
			       (unsigned)(sizeof path - 1), kr, matched);
			mt_result(0);
		}
	}

	/* ── The reader: the Mach path first. ── */
	vfs_flipc_set_enabled(0);

	/* [13] The control: a read of an honest file, answered on Mach. */
	if (!mounted) {
		printf("mount_test: [13] WRONG — not asked: not mounted\n");
		mt_result(0);
	} else {
		vfs_fd_t fd = vfs_open(MT_PREFIX "/honest", VFS_O_RDONLY, 0);
		ssize_t	 n = -1;

		mt_read_reset();
		if (fd >= 0) {
			n = vfs_read(fd, mt_read.buf, LIAR_SIZE);
			(void)vfs_close(fd);
		}
		mt_print_buf(shown, sizeof shown);
		if (n == LIAR_SIZE && mt_all(mt_read.buf, 'M', LIAR_SIZE) &&
		    liar_mach_reads[LIAR_HONEST] > 0) {
			printf("mount_test: [13] %d bytes of an honest file read "
			       "on the Mach path: \"%s\"\n", (int)n, shown);
			mt_result(1);
		} else {
			printf("mount_test: [13] WRONG — an honest file on the "
			       "Mach path: fd %d, %ld bytes \"%s\", %u Mach "
			       "reads\n", fd, (long)n, shown,
			       liar_mach_reads[LIAR_HONEST]);
			mt_result(0);
		}
	}

	/* [14] A Mach read answered LIAR_MORE_BY bytes longer than asked. */
	if (!mounted) {
		printf("mount_test: [14] WRONG — not asked: not mounted\n");
		mt_result(0);
	} else {
		vfs_fd_t fd = vfs_open(MT_PREFIX "/more", VFS_O_RDONLY, 0);
		ssize_t	 n = 0;

		mt_read_reset();
		if (fd >= 0) {
			n = vfs_read(fd, mt_read.buf, LIAR_SIZE);
			(void)vfs_close(fd);
		}
		if (fd >= 0 && n == -1 && mt_guard_intact() &&
		    liar_mach_reads[LIAR_MORE] > 0) {
			printf("mount_test: [14] a read of %d bytes answered "
			       "with %d: refused, and nothing written past the "
			       "buffer\n", LIAR_SIZE, LIAR_SIZE + LIAR_MORE_BY);
			mt_result(1);
		} else {
			printf("mount_test: [14] WRONG — a read of %d bytes "
			       "answered with %d: returned %ld, %s, fd %d\n",
			       LIAR_SIZE, LIAR_SIZE + LIAR_MORE_BY, (long)n,
			       mt_guard_intact() ? "the guard after the buffer "
			       "intact" : "the guard after the buffer written",
			       fd);
			mt_result(0);
		}
	}

	/* [15] A Mach write answered as one byte more than was sent. */
	if (!mounted) {
		printf("mount_test: [15] WRONG — not asked: not mounted\n");
		mt_result(0);
	} else {
		vfs_fd_t fd = vfs_open(MT_PREFIX "/write_more", VFS_O_RDWR, 0);
		ssize_t	 n = 0;
		off_t	 at = -1;

		if (fd >= 0) {
			n = vfs_write(fd, "12345678", 8);
			at = vfs_lseek(fd, 0, VFS_SEEK_CUR);
			(void)vfs_close(fd);
		}
		if (fd >= 0 && n == -1 && at == 0) {
			printf("mount_test: [15] a write of 8 bytes answered as "
			       "9: refused, and the offset is still 0\n");
			mt_result(1);
		} else {
			printf("mount_test: [15] WRONG — a write of 8 bytes "
			       "answered as 9: returned %ld, offset %ld, fd "
			       "%d\n", (long)n, (long)at, fd);
			mt_result(0);
		}
	}

	/*
	 * [16] A read of 4 GiB, of a LIAR_SIZE-byte file.  The count went to
	 * the server in 32 bits, as zero, and zero bytes back read as the end
	 * of the file.
	 */
	if (!mounted) {
		printf("mount_test: [16] WRONG — not asked: not mounted\n");
		mt_result(0);
	} else {
		vfs_fd_t fd = vfs_open(MT_PREFIX "/honest", VFS_O_RDONLY, 0);
		ssize_t	 n = -1;

		mt_read_reset();
		if (fd >= 0) {
			n = vfs_read(fd, mt_read.buf, (size_t)1 << 32);
			(void)vfs_close(fd);
		}
		if (n == LIAR_SIZE && mt_all(mt_read.buf, 'M', LIAR_SIZE)) {
			printf("mount_test: [16] a read of 4 GiB of a %d-byte "
			       "file returns its %d bytes\n", LIAR_SIZE,
			       LIAR_SIZE);
			mt_result(1);
		} else {
			printf("mount_test: [16] WRONG — a read of 4 GiB of a "
			       "%d-byte file returned %ld, fd %d\n", LIAR_SIZE,
			       (long)n, fd);
			mt_result(0);
		}
	}

	/* ── The FLIPC path. ── */
	why = mounted ? liar_flipc_start() : "not mounted";
	if (why != NULL)
		printf("mount_test: WRONG — no FLIPC endpoint: %s\n", why);
	vfs_flipc_set_enabled(1);

	/* [17] The control: a read of an honest file, answered on FLIPC. */
	if (why != NULL) {
		printf("mount_test: [17] WRONG — not asked: no endpoint\n");
		mt_result(0);
	} else {
		vfs_fd_t fd = vfs_open(MT_PREFIX "/flipc_honest",
				       VFS_O_RDONLY, 0);
		ssize_t	 n = -1;

		mt_read_reset();
		if (fd >= 0) {
			n = vfs_read(fd, mt_read.buf, LIAR_SIZE);
			(void)vfs_close(fd);
		}
		mt_print_buf(shown, sizeof shown);
		if (n == LIAR_SIZE && mt_all(mt_read.buf, 'F', LIAR_SIZE) &&
		    liar_flipc_reads[LIAR_FLIPC_HONEST] > 0 &&
		    liar_mach_reads[LIAR_FLIPC_HONEST] == 0) {
			printf("mount_test: [17] %d bytes of an honest file read "
			       "through FLIPC: \"%s\"\n", (int)n, shown);
			mt_result(1);
		} else {
			printf("mount_test: [17] WRONG — an honest file through "
			       "FLIPC: fd %d, %ld bytes \"%s\", %u FLIPC and %u "
			       "Mach reads\n", fd, (long)n, shown,
			       liar_flipc_reads[LIAR_FLIPC_HONEST],
			       liar_mach_reads[LIAR_FLIPC_HONEST]);
			mt_result(0);
		}
	}

	/*
	 * [18] A FLIPC read answered LIAR_FLIPC_MORE_BY bytes longer than
	 * asked, from inside the region.  libvfs reads through a window of
	 * VFS_FLIPC_MAX_READ bytes it allocates on the fd's first read, and
	 * the extra bytes land past it.
	 *
	 * ⚠️ So a guard page is put where they land, or the arm would turn on
	 * what this task happens to have mapped there: the window is allocated
	 * first-fit, and a probe of its size, allocated and given back, finds
	 * the hole it will take.  Nothing in this task allocates between the
	 * probe and the read.  If the page cannot be placed the arm says so
	 * and judges by what the read returned.
	 */
	if (why != NULL) {
		printf("mount_test: [18] WRONG — not asked: no endpoint\n");
		mt_result(0);
	} else {
		vm_address_t	probe = 0, guard = 0;
		vfs_fd_t	fd;
		ssize_t		n = -1;
		int		guard_ok = 1;

		if (vm_allocate(mach_task_self(), &probe, VFS_FLIPC_MAX_READ,
				TRUE) == KERN_SUCCESS) {
			(void)vm_deallocate(mach_task_self(), probe,
					    VFS_FLIPC_MAX_READ);
			guard = probe + VFS_FLIPC_MAX_READ;
			if (vm_allocate(mach_task_self(), &guard, vm_page_size,
					FALSE) == KERN_SUCCESS)
				memset((void *)guard, 0xA5, vm_page_size);
			else
				guard = 0;
		}
		fd = vfs_open(MT_PREFIX "/flipc_more", VFS_O_RDONLY, 0);
		mt_read_reset();
		if (fd >= 0) {
			n = vfs_read(fd, mt_read.buf, LIAR_SIZE);
			(void)vfs_close(fd);
		}
		if (guard != 0) {
			guard_ok = mt_all((const char *)guard, (char)0xA5,
					  vm_page_size);
			(void)vm_deallocate(mach_task_self(), guard,
					    vm_page_size);
		}
		mt_print_buf(shown, sizeof shown);
		if (n == LIAR_SIZE && mt_all(mt_read.buf, 'M', LIAR_SIZE) &&
		    guard_ok && liar_flipc_reads[LIAR_FLIPC_MORE] > 0 &&
		    liar_mach_reads[LIAR_FLIPC_MORE] > 0) {
			printf("mount_test: [18] a FLIPC read answered %d bytes "
			       "longer than asked: refused, read again on the "
			       "Mach path (\"%s\")%s\n", LIAR_FLIPC_MORE_BY,
			       shown, guard ? ", nothing written past the "
			       "window" : ", no guard page could be placed");
			mt_result(1);
		} else {
			printf("mount_test: [18] WRONG — a FLIPC read answered "
			       "%d bytes longer than asked: %ld bytes \"%s\", "
			       "%u FLIPC and %u Mach reads, %s\n",
			       LIAR_FLIPC_MORE_BY, (long)n, shown,
			       liar_flipc_reads[LIAR_FLIPC_MORE],
			       liar_mach_reads[LIAR_FLIPC_MORE],
			       guard == 0 ? "no guard page placed" : guard_ok
			       ? "the page past the window intact"
			       : "the page past the window written");
			mt_result(0);
		}
	}

	/*
	 * [19] A FLIPC read answered from an offset that wraps to just before
	 * the data region, for a length that would fit.
	 */
	if (why != NULL) {
		printf("mount_test: [19] WRONG — not asked: no endpoint\n");
		mt_result(0);
	} else {
		vfs_fd_t fd = vfs_open(MT_PREFIX "/flipc_outside",
				       VFS_O_RDONLY, 0);
		ssize_t	 n = -1;

		mt_read_reset();
		if (fd >= 0) {
			n = vfs_read(fd, mt_read.buf, LIAR_SIZE);
			(void)vfs_close(fd);
		}
		mt_print_buf(shown, sizeof shown);
		if (n == LIAR_SIZE && mt_all(mt_read.buf, 'M', LIAR_SIZE) &&
		    liar_flipc_reads[LIAR_FLIPC_OUTSIDE] > 0 &&
		    liar_mach_reads[LIAR_FLIPC_OUTSIDE] > 0) {
			printf("mount_test: [19] a FLIPC read answered from "
			       "outside the data region: refused, read again on "
			       "the Mach path (\"%s\")\n", shown);
			mt_result(1);
		} else {
			printf("mount_test: [19] WRONG — a FLIPC read answered "
			       "from outside the data region: %ld bytes \"%s\", "
			       "%u FLIPC and %u Mach reads\n", (long)n, shown,
			       liar_flipc_reads[LIAR_FLIPC_OUTSIDE],
			       liar_mach_reads[LIAR_FLIPC_OUTSIDE]);
			mt_result(0);
		}
	}

	/* [20] The control for [11]: the mount's own signature checks it out. */
	if (!mounted) {
		printf("mount_test: [20] WRONG — not asked: not mounted\n");
		mt_result(0);
	} else {
		kr = netname_check_out_mount(name_server_port, MT_PREFIX, sig);
		if (kr == NETNAME_SUCCESS &&
		    mt_look_up(MT_PREFIX "/honest", &got, matched)
		    == NETNAME_SUCCESS && strcmp(matched, MT_PREFIX) != 0) {
			printf("mount_test: [20] " MT_PREFIX " checked out with "
			       "its own signature, and " MT_PREFIX "/honest "
			       "resolves to \"%s\" again\n", matched);
			mt_result(1);
		} else {
			printf("mount_test: [20] WRONG — " MT_PREFIX " checked "
			       "out with its own signature: %d, a look-up under "
			       "it now matches \"%s\"\n", kr, matched);
			mt_result(0);
		}
	}

	mt_death_arms(sig, other);

	/*
	 * [25] The notification that brings a dead server's receive right back
	 * to whoever lent it, through the stub that receives it (mount_notify.c).
	 */
	{
		mach_msg_id_t	id;
		mach_msg_size_t	size;

		why = mt_port_destroyed(&id, &size);
		mt_result(why == NULL);
		if (why == NULL)
			printf("mount_test: [25] a port-destroyed notification "
			       "(id %d, %u bytes) reached its MIG handler with "
			       "the receive right\n", (int)id, (unsigned)size);
		else
			printf("mount_test: [25] WRONG — a port-destroyed "
			       "notification (id %d, %u bytes): %s\n", (int)id,
			       (unsigned)size, why);
	}

	/*
	 * [26] A bootstrap request as long as bootstrap.defs allows: two module
	 * names of 63 characters, 176 bytes.  bootstrap received into 128, and a
	 * longer request was destroyed unread -- MIG_SERVER_DIED -- where the
	 * answer for a module that is not there is KERN_INVALID_ARGUMENT.
	 */
	{
		char			class[64], name[64];
		vm_offset_t		data = 0;
		mach_msg_type_number_t	count = 0;

		memset(class, 'c', sizeof class - 1);
		class[sizeof class - 1] = '\0';
		memset(name, 'n', sizeof name - 1);
		name[sizeof name - 1] = '\0';
		kr = bootstrap_get_module(bootstrap_port, class, name, &data,
					  &count);
		if (kr == KERN_SUCCESS && count > 0)
			(void)vm_deallocate(mach_task_self(), data, count);
		mt_result(kr == KERN_INVALID_ARGUMENT);
		if (kr == KERN_INVALID_ARGUMENT)
			printf("mount_test: [26] a bootstrap request as long as "
			       "its interface takes, two names of %u "
			       "characters, is answered: KERN_INVALID_ARGUMENT, "
			       "no such module\n", (unsigned)(sizeof class - 1));
		else
			printf("mount_test: [26] WRONG — a bootstrap request as "
			       "long as its interface takes: %d, not "
			       "KERN_INVALID_ARGUMENT (%d)%s\n", kr,
			       KERN_INVALID_ARGUMENT, kr == MIG_SERVER_DIED ?
			       ": bootstrap never saw it" : "");
	}

	mt_rights_arms(mine, root, have_root, eps_why);	/* [27] to [31], #911 */

	if (have_root)
		(void)mach_port_deallocate(mach_task_self(), root);
	printf("mount_test: %d of %d arms passed\n", passed, arms);
	return (passed == arms && arms == MT_ARMS) ? 0 : 1;
}
