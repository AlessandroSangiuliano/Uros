/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * notify_forge_test -- a message with a notification's id, sent by a task, is
 * refused where the kernel's is taken (#981).
 *
 * The kernel sends a notification through the send-once right that
 * mach_port_request_notification() made for it, so it arrives with a send-once
 * right in its local bits.  This test holds a send right to three ports that
 * receive notifications -- the HAL's, a token of a pipe it registers with
 * pipe_server, the port of a libflipc2 endpoint it serves itself -- and sends
 * each a message with a notification's id and size through that send right,
 * with a reply port of its own, and right after it a probe the receiver
 * answers in order (forge()).  What comes back says what it made of it:
 *
 *   refused   an error reply: MIG_BAD_ID from a demux that does not know the
 *             id, MIG_BAD_ARGUMENTS from a notify stub's check;
 *   believed  no reply, the probe's answer first, which is what a
 *             notification gets from the HAL and from an endpoint, or
 *             KERN_SUCCESS, which a notify stub answers once its handler has
 *             run -- and what it named has ended: the pipe's token is dead,
 *             the endpoint reported its live client dead.
 *
 * [4] asks the other half: a client of the endpoint that really goes is still
 * reported dead, from the kernel's own notification.
 *
 * Every line ends in "-- WRONG" when the arm fails; the last line counts the
 * arms that passed.
 */

#include <mach.h>
#include <mach/message.h>
#include <mach/mig_errors.h>
#include <mach/ndr.h>
#include <mach/notify.h>
#include <mach/thread_switch.h>
#include <sys/timers.h>		/* #1009: getclock(), wait_for()'s ms */
#include <mach_init.h>			/* name_server_port */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <servers/netname_wait.h>

#include <flipc2.h>

#include "pipe_types.h"
#include "pipe_user.h"

#define ARMS		4
#define FORGE_WAIT_MS	20000		/* for the answers, however loaded the boot */
#define EVENT_WAIT_MS	2000		/* for the endpoint's pump to say a thing */
#define FORGED_NAME	0x981		/* a name the HAL is not asked to hold */
#define PROBE_ID	981981		/* an id no server asked here knows */

static int passed;

/* A notification's request as MIG lays it out: a dead-name's port name, or a
 * no-senders' make-send count, after the header and the NDR record. */
struct forged {
	mach_msg_header_t	h;
	NDR_record_t		ndr;
	natural_t		word;
};
_Static_assert(sizeof(struct forged) == 36,
	       "a dead-name or no-senders request is 36 bytes, as the notify "
	       "stub checks");

struct answer {
	mach_msg_header_t	h;
	NDR_record_t		ndr;
	kern_return_t		ret;
	mach_msg_format_0_trailer_t trailer;
	char			spare[64];	/* room for any trailer */
};

#define FORGE_SENT	0		/* answered: *ret holds its code */
#define FORGE_SILENT	1		/* no answer, the probe after it answered */
#define FORGE_STUCK	2		/* no answer to either in FORGE_WAIT_MS */
#define FORGE_FAILED	3		/* it could not be sent or read */

/* What say_answer() writes in the two longer cases, and its buffer. */
#define SAID_SILENT	"no answer to it, where the message sent after it " \
			"got one"
#define SAID_STUCK	"no answer to it or to the message after it in " \
			"%d ms"
#define SAID_MAX	160
_Static_assert(sizeof SAID_SILENT <= SAID_MAX &&
	       sizeof SAID_STUCK + 11 <= SAID_MAX,
	       "a text of say_answer() would be cut");

/*
 * Every line of the test, whole: the console keeps a line whole up to 256
 * bytes (#578), and past that another task's output can land inside it,
 * where ablate.sh, reading the raw log, misses its end (#943) -- as it
 * missed [3]'s WRONG at 299 bytes.  A longer line is said, as a WRONG.
 */
static void	verdict(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

static void
verdict(const char *fmt, ...)
{
	char	line[256];
	va_list	ap;
	int	n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof line)
		printf("notify_forge_test: a line of %d bytes, past the %u the "
		       "console keeps whole, begins \"%.48s\" -- WRONG\n", n,
		       (unsigned)(sizeof line - 1), line);
	else
		printf("%s", line);
}

static kern_return_t
send_one(mach_port_t to, mach_msg_type_name_t disp, mach_port_t reply,
	 mach_msg_id_t id, natural_t word)
{
	struct forged	m;

	memset(&m, 0, sizeof m);
	m.h.msgh_bits = MACH_MSGH_BITS(disp, MACH_MSG_TYPE_MAKE_SEND_ONCE);
	m.h.msgh_size = sizeof m;
	m.h.msgh_remote_port = to;
	m.h.msgh_local_port = reply;
	m.h.msgh_id = id;
	m.ndr = NDR_record;
	m.word = word;
	return mach_msg(&m.h, MACH_SEND_MSG, sizeof m, 0, MACH_PORT_NULL,
			MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

/*
 * Sends `to' a message with the id and word given, through a right of
 * disposition `disp' -- a send right, never a send-once -- and right after it,
 * on the same port and with the same reply port, a probe with an id no server
 * asked here knows.  Each receiver asked serves that port from one thread, so
 * it answers the two in order: an answer to the forged message, if it gives
 * one, comes before the probe's MIG_BAD_ID.  So the first answer says what the
 * receiver made of it, however loaded the boot: the forged message's own (its
 * id + 100), or the probe's, and then the forged message got none.  A wait for
 * the forged message's answer alone could not tell: on 22/11, in a loaded boot,
 * the HAL refused one more than two seconds after it was sent.  A send-once
 * notification, for a reply right destroyed unanswered, is passed over.
 */
static int
forge(mach_port_t to, mach_msg_type_name_t disp, mach_msg_id_t id,
      natural_t word, kern_return_t *ret)
{
	struct answer	a;
	mach_port_t	reply = MACH_PORT_NULL;
	kern_return_t	kr;
	int		r = FORGE_FAILED;

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			       &reply) != KERN_SUCCESS)
		return FORGE_FAILED;
	kr = send_one(to, disp, reply, id, word);
	if (kr != MACH_MSG_SUCCESS)
		verdict("notify_forge_test: the forged message was not sent: "
		        "0x%x\n", kr);
	else {
		/* The probe may find the port gone, if the forged message was
		 * believed already and ended it: its own answer then says so. */
		(void)send_one(to, disp, reply, PROBE_ID, 0);
		for (;;) {
			memset(&a, 0, sizeof a);
			kr = mach_msg(&a.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
				      sizeof a, reply, FORGE_WAIT_MS,
				      MACH_PORT_NULL);
			if (kr == MACH_RCV_TIMED_OUT) {
				r = FORGE_STUCK;
				break;
			}
			if (kr != MACH_MSG_SUCCESS)
				break;
			if (a.h.msgh_id == MACH_NOTIFY_SEND_ONCE)
				continue;
			if (a.h.msgh_id == id + 100) {
				*ret = a.ret;
				r = FORGE_SENT;
			} else if (a.h.msgh_id == PROBE_ID + 100)
				r = FORGE_SILENT;
			break;
		}
	}
	(void)mach_port_mod_refs(mach_task_self(), reply,
				 MACH_PORT_RIGHT_RECEIVE, -1);
	return r;
}

static void
say_answer(char *buf, size_t n, int r, kern_return_t ret)
{
	if (r == FORGE_SILENT)
		snprintf(buf, n, "%s", SAID_SILENT);
	else if (r == FORGE_SENT)
		snprintf(buf, n, "answered %d", (int)ret);
	else if (r == FORGE_STUCK)
		snprintf(buf, n, SAID_STUCK, FORGE_WAIT_MS);
	else
		snprintf(buf, n, "the exchange failed");
}

/* ------------------------------------------------------------------ */
/*  [1] the HAL                                                        */
/* ------------------------------------------------------------------ */

static void
arm_hal(void)
{
	mach_port_t	hal = MACH_PORT_NULL;
	kern_return_t	ret = KERN_SUCCESS;
	char		said[SAID_MAX];
	int		r;

	if (netname_wait(name_server_port, "hal", &hal) != KERN_SUCCESS ||
	    hal == MACH_PORT_NULL) {
		verdict("notify_forge_test: [1] the HAL's name could not be "
		        "had -- WRONG\n");
		return;
	}
	r = forge(hal, MACH_MSG_TYPE_COPY_SEND, MACH_NOTIFY_DEAD_NAME,
		  FORGED_NAME, &ret);
	say_answer(said, sizeof said, r, ret);
	if (r == FORGE_SENT && ret == MIG_BAD_ID) {
		verdict("notify_forge_test: [1] the HAL refused a message with "
		        "a dead-name notification's id from a task: %s, "
		        "MIG_BAD_ID\n", said);
		passed++;
	} else
		verdict("notify_forge_test: [1] a message with a dead-name "
		        "notification's id from a task: %s -- WRONG, the HAL "
		        "took it for the kernel's\n", said);
	(void)mach_port_deallocate(mach_task_self(), hal);
}

/* ------------------------------------------------------------------ */
/*  [2] pipe_server                                                    */
/* ------------------------------------------------------------------ */

static void
arm_pipe(void)
{
	mach_port_t		service = MACH_PORT_NULL, entry = MACH_PORT_NULL;
	mach_port_t		pipe = MACH_PORT_NULL, sem = MACH_PORT_NULL;
	mach_port_t		tok[2] = { MACH_PORT_NULL, MACH_PORT_NULL };
	mach_port_type_t	type = 0;
	flipc2_channel_t	ch = 0;
	kern_return_t		kr, ret = KERN_SUCCESS;
	char			said[SAID_MAX];
	int			r, alive;

	if (netname_wait(name_server_port, "pipe_server", &service)
	    != KERN_SUCCESS || service == MACH_PORT_NULL) {
		verdict("notify_forge_test: [2] pipe_server's name could not "
		        "be had -- WRONG\n");
		return;
	}
	/* A pipe of this task's, as pipe() registers one (posix_pipe.c). */
	if (flipc2_channel_create_ex(PIPE_CHANNEL_SIZE, PIPE_SLOTS,
				     FLIPC2_CREATE_FLAT, &ch, &sem)
	    != FLIPC2_SUCCESS ||
	    flipc2_channel_make_entry(ch, PIPE_CHANNEL_SIZE,
				      FLIPC2_ROLE_PRODUCER, &entry)
	    != FLIPC2_SUCCESS) {
		verdict("notify_forge_test: [2] no channel for a pipe -- "
		        "WRONG\n");
		return;
	}
	kr = pipe_register(service, entry, PIPE_CHANNEL_SIZE, &pipe);
	if (kr == KERN_SUCCESS)
		kr = pipe_token(pipe, PIPE_END_READ, FORGED_NAME,
				&tok[PIPE_END_READ]);
	if (kr == KERN_SUCCESS)
		kr = pipe_token(pipe, PIPE_END_WRITE, FORGED_NAME,
				&tok[PIPE_END_WRITE]);
	if (kr != KERN_SUCCESS) {
		verdict("notify_forge_test: [2] pipe_server would not register "
		        "a pipe and its tokens: 0x%x -- WRONG\n", kr);
		return;
	}

	r = forge(tok[PIPE_END_READ], MACH_MSG_TYPE_COPY_SEND,
		  MACH_NOTIFY_NO_SENDERS, 1, &ret);
	say_answer(said, sizeof said, r, ret);
	(void)mach_port_type(mach_task_self(), tok[PIPE_END_READ], &type);
	alive = (type & MACH_PORT_TYPE_SEND) != 0;
	if (r == FORGE_SENT && ret == MIG_BAD_ARGUMENTS && alive) {
		verdict("notify_forge_test: [2] pipe_server refused a message "
		        "with a no-senders notification's id from a task, on a "
		        "token it holds: %s, MIG_BAD_ARGUMENTS, and the token is "
		        "still a send right\n", said);
		passed++;
	} else
		verdict("notify_forge_test: [2] a message with a no-senders "
		        "notification's id from a task, on a token: %s, and the "
		        "token is %s -- WRONG, pipe_server took it for the "
		        "kernel's\n", said,
		        alive ? "still a send right" : "dead");

	/* The pipe goes with its tokens' and its port's last senders. */
	(void)mach_port_deallocate(mach_task_self(), tok[PIPE_END_READ]);
	(void)mach_port_deallocate(mach_task_self(), tok[PIPE_END_WRITE]);
	(void)mach_port_deallocate(mach_task_self(), pipe);
	(void)mach_port_deallocate(mach_task_self(), service);
}

/* ------------------------------------------------------------------ */
/*  [3] and [4] a libflipc2 endpoint this task serves                  */
/* ------------------------------------------------------------------ */

static flipc2_endpoint_t	ep;
static volatile int		ep_new, ep_dead, ep_stop;

/* The endpoint's one thread, as a reactor server has it: every event is
 * counted, and a dead client's channels freed, which the pump leaves to it. */
static void *
pump(void *arg)
{
	flipc2_ep_event_t	ev;

	(void)arg;
	while (!ep_stop) {
		if (flipc2_endpoint_pump(ep, 100, &ev) != FLIPC2_SUCCESS)
			continue;
		if (ev.type == FLIPC2_EP_EVENT_NEW)
			ep_new++;
		else if (ev.type == FLIPC2_EP_EVENT_DEAD) {
			ep_dead++;
			flipc2_channel_destroy(ev.fwd_ch);
			flipc2_channel_destroy(ev.rev_ch);
		}
	}
	return NULL;
}

/*
 * Waits for *counter to reach want; whether it did, the milliseconds it took
 * in *took.  #1009: a window -- [3]'s, in which nothing may come -- is read
 * by the clock, ms of it.  A patience -- for the endpoint's pump to say a
 * thing, which it can say only when the processor comes back to it -- is a
 * number of chances, not a time: it gives up after ms / 10 waits of 10 ms,
 * as they were counted before, and the clock only says how long they took.
 * Bounded by a second of the clock, act_test's rpc_pair_start() lost its
 * thread at one processor under TCG (27/11).  A clock refused counts the
 * waits for the window too, as before.
 */
#define WAIT_WINDOW	1
#define WAIT_PATIENCE	0

static int
wait_for(volatile int *counter, int want, int ms, int window, int *took)
{
	struct timespec	t0, t;
	int		clocked, waited = 0, waits;

	clocked = getclock(TIMEOFDAY, &t0) == 0;
	for (waits = 0;; waits++) {
		if (clocked && getclock(TIMEOFDAY, &t) == 0)
			waited = (int)((t.tv_sec - t0.tv_sec) * 1000 +
				       (t.tv_nsec - t0.tv_nsec) / 1000000);
		else
			waited = waits * 10;
		if (took != NULL)
			*took = waited;
		if (*counter >= want)
			return 1;
		if (window ? waited >= ms : waits >= ms / 10)
			return *counter >= want;
		(void)thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 10);
	}
}

static void
arm_endpoint(void)
{
	flipc2_channel_t	fwd = 0, rev = 0;
	pthread_t		t;
	kern_return_t		ret = KERN_SUCCESS;
	char			said[SAID_MAX];
	int			r, dead, took = 0;

	if (flipc2_endpoint_create("notify_forge_test", 2,
				   FLIPC2_CHANNEL_SIZE_MIN,
				   FLIPC2_RING_ENTRIES_MIN, 0, &ep)
	    != FLIPC2_SUCCESS) {
		verdict("notify_forge_test: [3] no endpoint -- WRONG\n");
		verdict("notify_forge_test: [4] no endpoint -- WRONG\n");
		return;
	}
	if (pthread_create(&t, NULL, pump, NULL) != 0) {
		verdict("notify_forge_test: [3] no thread for the endpoint -- "
		        "WRONG\n");
		verdict("notify_forge_test: [4] no thread for the endpoint -- "
		        "WRONG\n");
		return;
	}
	if (flipc2_endpoint_connect_port(flipc2_endpoint_port(ep),
					 FLIPC2_CHANNEL_SIZE_MIN,
					 FLIPC2_RING_ENTRIES_MIN, &fwd, &rev)
	    != FLIPC2_SUCCESS ||
	    !wait_for(&ep_new, 1, EVENT_WAIT_MS, WAIT_PATIENCE, NULL)) {
		verdict("notify_forge_test: [3] the client did not connect -- "
		        "WRONG\n");
		verdict("notify_forge_test: [4] the client did not connect -- "
		        "WRONG\n");
		ep_stop = 1;
		return;
	}

	/*
	 * [3] In this task the endpoint knows the client's token by the name
	 * the client made it with: a send right received for a port a task
	 * already holds a right to keeps that right's name.
	 */
	r = forge(flipc2_endpoint_port(ep), MACH_MSG_TYPE_MAKE_SEND,
		  MACH_NOTIFY_DEAD_NAME, fwd->conn_token, &ret);
	say_answer(said, sizeof said, r, ret);
	dead = wait_for(&ep_dead, 1, EVENT_WAIT_MS / 4, WAIT_WINDOW, NULL);
	if (r == FORGE_SENT && ret == MIG_BAD_ID && !dead) {
		verdict("notify_forge_test: [3] the endpoint refused a message "
		        "with a dead-name notification's id from a task, naming "
		        "a live client's token: %s, MIG_BAD_ID, and the client is "
		        "still connected\n", said);
		passed++;
	} else
		verdict("notify_forge_test: [3] a message with a dead-name "
		        "notification's id from a task, naming a live client's "
		        "token: %s, and the endpoint %s -- WRONG, it took it for "
		        "the kernel's\n", said,
		        dead ? "reported the client dead" :
		        "said nothing of it");

	/* [4] The client really goes: its token's receive right with it. */
	if (dead)
		verdict("notify_forge_test: [4] the client was given up by [3]'s "
		        "message, so its real going cannot be asked -- WRONG\n");
	else {
		(void)flipc2_endpoint_disconnect(fwd, rev);
		if (wait_for(&ep_dead, 1, EVENT_WAIT_MS, WAIT_PATIENCE, &took)) {
			verdict("notify_forge_test: [4] a client that went was "
			        "reported dead, from the kernel's dead-name "
			        "notification\n");
			passed++;
		} else
			verdict("notify_forge_test: [4] a client that went was "
			        "not reported dead within %d ms -- WRONG, the "
			        "kernel's own notification was refused\n",
			        took);
	}
	ep_stop = 1;
	(void)pthread_join(t, NULL);
	(void)flipc2_endpoint_destroy(ep);
}

int
main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	verdict("notify_forge_test: started\n");
	arm_hal();
	arm_pipe();
	arm_endpoint();
	verdict("notify_forge_test: %d of %d arms passed\n", passed, ARMS);
	return 0;
}
