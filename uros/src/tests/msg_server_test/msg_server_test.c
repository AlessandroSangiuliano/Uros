/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A right a server kept must survive the server's next message (#583).
 *
 * libmach's mach_msg_server swaps its two buffers on every reply, so the
 * reply buffer it hands the demux holds the request before last.  A demux
 * that takes a one-way message by hand sets RetCode to MIG_NO_REPLY and
 * leaves the header alone -- char_server does it for every IRQ, hal_server
 * for a dead name, bootstrap for a request to a port nobody has taken.  When
 * the request before last was complex, its COMPLEX bit made the loop skip
 * the MIG_NO_REPLY test: the old request went out as the reply to its spent
 * reply port, the send failed, and mach_msg_destroy() released the rights in
 * it.  Rights the server had kept.  In char_server that was the send right
 * of the shell's notify port, and the shell waited for input forever.
 *
 * Three messages, in an order the client fixes:
 *
 *   KEEP     complex RPC; the server keeps the send right it carries
 *   ONE-WAY  no reply port; the demux sets RetCode and nothing else
 *   PROBE    RPC; the server says what it still holds under that name
 *
 * The same fact is then asked from the other side: the client holds the
 * receive right, so it can see whether any send right to it still exists.
 * Two halves that must agree.
 */

#include <mach.h>
#include <mach/message.h>
#include <mach/mig_errors.h>
#include <mach/port.h>

#include <stdio.h>
#include <pthread.h>

#define MST_KEEP	7100
#define MST_ONEWAY	7101
#define MST_PROBE	7102

/* Long enough for a loaded machine, short enough that a lost reply is a
 * line in the log and not a boot that never ends. */
#define MST_RCV_MS	10000

typedef struct {
	mach_msg_header_t		head;
	mach_msg_body_t			body;
	mach_msg_port_descriptor_t	port;
} keep_request_t;

typedef struct {
	mach_msg_header_t	head;
	NDR_record_t		ndr;
	kern_return_t		ret_code;	/* where mig_reply_error_t has it */
	kern_return_t		refs_kr;
	mach_port_urefs_t	refs;
} probe_reply_t;

typedef union {
	mig_reply_error_t	error;
	probe_reply_t		probe;
	char			room[256];
} reply_buffer_t;

static mach_port_t server_port;
static mach_port_t kept = MACH_PORT_NULL;

static boolean_t
mst_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
	mig_reply_error_t *r = (mig_reply_error_t *)out;

	if (in->msgh_id == MST_ONEWAY) {
		/* char_server's IRQ path, field for field. */
		r->RetCode = MIG_NO_REPLY;
		r->Head.msgh_size = sizeof(mig_reply_error_t);
		return TRUE;
	}

	out->msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(in->msgh_bits), 0);
	out->msgh_remote_port = in->msgh_remote_port;
	out->msgh_local_port = MACH_PORT_NULL;
	out->msgh_id = in->msgh_id + 100;
	out->msgh_size = sizeof(mig_reply_error_t);
	r->NDR = NDR_record;

	if (in->msgh_id == MST_KEEP) {
		kept = ((keep_request_t *)in)->port.name;
		r->RetCode = KERN_SUCCESS;
		return TRUE;
	}
	if (in->msgh_id == MST_PROBE) {
		probe_reply_t *p = (probe_reply_t *)out;

		p->ret_code = KERN_SUCCESS;
		p->refs = 0;
		p->refs_kr = mach_port_get_refs(mach_task_self(), kept,
						MACH_PORT_RIGHT_SEND, &p->refs);
		out->msgh_size = sizeof(probe_reply_t);
		return TRUE;
	}
	r->RetCode = MIG_BAD_ID;
	return FALSE;
}

static void *
server_main(void *arg)
{
	(void)arg;
	(void)mach_msg_server(mst_demux, sizeof(reply_buffer_t), server_port, 0);
	printf("msg_server_test: the server loop returned -- WRONG\n");
	return NULL;
}

/* One RPC: send `req', wait at most MST_RCV_MS for the reply on `reply_port'. */
static mach_msg_return_t
rpc(mach_msg_header_t *req, mach_msg_size_t size, mach_port_t reply_port,
    reply_buffer_t *rep)
{
	mach_msg_return_t mr;

	mr = mach_msg(req, MACH_SEND_MSG, size, 0, MACH_PORT_NULL,
		      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS)
		return mr;
	return mach_msg(&rep->error.Head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
			sizeof(*rep), reply_port, MST_RCV_MS, MACH_PORT_NULL);
}

int
main(int argc, char **argv)
{
	mach_port_t		gift, reply_port;
	keep_request_t		keep;
	mach_msg_header_t	oneway, probe;
	reply_buffer_t		rep;
	mach_port_status_t	st;
	mach_msg_type_number_t	cnt = MACH_PORT_RECEIVE_STATUS_COUNT;
	mach_msg_return_t	mr;
	kern_return_t		kr;
	pthread_t		server;
	int			server_holds, sender_exists;

	(void)argc;
	(void)argv;

	printf("msg_server_test: started (#583)\n");

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			       &server_port) != KERN_SUCCESS
	    || mach_port_insert_right(mach_task_self(), server_port,
				      server_port, MACH_MSG_TYPE_MAKE_SEND)
	       != KERN_SUCCESS
	    || mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				  &gift) != KERN_SUCCESS
	    || mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				  &reply_port) != KERN_SUCCESS) {
		printf("msg_server_test: no ports -- WRONG\n");
		return 1;
	}
	if (pthread_create(&server, NULL, server_main, NULL) != 0) {
		printf("msg_server_test: no server thread -- WRONG\n");
		return 1;
	}

	/* KEEP: the server is handed a send right to `gift' and keeps it. */
	keep.head.msgh_bits = MACH_MSGH_BITS_COMPLEX
		| MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
				 MACH_MSG_TYPE_MAKE_SEND_ONCE);
	keep.head.msgh_size = sizeof(keep);
	keep.head.msgh_remote_port = server_port;
	keep.head.msgh_local_port = reply_port;
	keep.head.msgh_id = MST_KEEP;
	keep.body.msgh_descriptor_count = 1;
	keep.port.name = gift;
	keep.port.disposition = MACH_MSG_TYPE_MAKE_SEND;
	keep.port.type = MACH_MSG_PORT_DESCRIPTOR;
	mr = rpc(&keep.head, sizeof(keep), reply_port, &rep);
	if (mr != MACH_MSG_SUCCESS || rep.error.RetCode != KERN_SUCCESS) {
		printf("msg_server_test: KEEP failed (mr=0x%x ret=%d) -- WRONG\n",
		       mr, mr == MACH_MSG_SUCCESS ? rep.error.RetCode : 0);
		return 1;
	}

	/* ONE-WAY: the reply buffer the server's loop now holds is KEEP. */
	oneway.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	oneway.msgh_size = sizeof(oneway);
	oneway.msgh_remote_port = server_port;
	oneway.msgh_local_port = MACH_PORT_NULL;
	oneway.msgh_id = MST_ONEWAY;
	mr = mach_msg(&oneway, MACH_SEND_MSG, sizeof(oneway), 0, MACH_PORT_NULL,
		      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (mr != MACH_MSG_SUCCESS) {
		printf("msg_server_test: ONE-WAY failed (mr=0x%x) -- WRONG\n", mr);
		return 1;
	}

	/* PROBE: the server's own account of what it kept. */
	probe.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
					 MACH_MSG_TYPE_MAKE_SEND_ONCE);
	probe.msgh_size = sizeof(probe);
	probe.msgh_remote_port = server_port;
	probe.msgh_local_port = reply_port;
	probe.msgh_id = MST_PROBE;
	mr = rpc(&probe, sizeof(probe), reply_port, &rep);
	if (mr != MACH_MSG_SUCCESS) {
		printf("msg_server_test: PROBE got no reply (mr=0x%x) -- WRONG\n",
		       mr);
		return 1;
	}
	server_holds = rep.probe.refs_kr == KERN_SUCCESS && rep.probe.refs == 1;
	printf("msg_server_test: [1] the server holds the right it kept: "
	       "get_refs kr=%d refs=%u -- %s\n", rep.probe.refs_kr,
	       (unsigned)rep.probe.refs, server_holds ? "ok" : "WRONG");

	/* The other half: does any send right to `gift' still exist? */
	kr = mach_port_get_attributes(mach_task_self(), gift,
				      MACH_PORT_RECEIVE_STATUS,
				      (mach_port_info_t)&st, &cnt);
	sender_exists = kr == KERN_SUCCESS && st.mps_srights != 0;
	printf("msg_server_test: [2] its receiver sees a sender: kr=%d "
	       "srights=%u -- %s\n", kr,
	       kr == KERN_SUCCESS ? (unsigned)st.mps_srights : 0u,
	       sender_exists ? "ok" : "WRONG");

	printf("msg_server_test: %d of 2 arms passed\n",
	       server_holds + sender_exists);
	return server_holds && sender_exists ? 0 : 1;
}
