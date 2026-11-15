/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * pipe_server -- the witness of a pipe's ends (#904).
 *
 * The bytes of a pipe go through memory its holders share
 * (libposix-uros/posix_pipe.c), and no server is in their path.  What memory
 * cannot tell is that the other end has gone: that the last task holding it
 * closed it or died.  A port's death can.  Every task that holds an end of a
 * pipe holds a token for it, a send right whose receive right is here, and a
 * token's no-senders notification is that task leaving the end.  When the
 * last token of an end goes, the pipe's writers_gone or readers_gone is set
 * and the other side's event count moved and woken: end of file, EPIPE.  A
 * token whose task holds one of the pipe's two locks and has no other token
 * on the pipe frees that lock, whose holder can no longer let it go.
 *
 * pipe_server maps each pipe through the memory entry its maker registers
 * (#911), read-write: a flat channel is one segment.  Every holder may write
 * any byte of the channel, so nothing read from it decides where this task
 * writes: the words are at the offsets pipe_types.h fixes.  A shared futex is
 * keyed on the memory object and offset (kern/sync_sema.c), the same in every
 * holder's mapping and in this one, so a wake from here reaches a sleeper in
 * any of them.
 *
 * One thread, one message at a time: nothing here needs a lock.
 */

#include "pipe_types.h"

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/bootstrap.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <mach/notify.h>
#include <mach/mach_interface.h>
#include <mach/urmach_futex.h>
#include <sa_mach.h>
#include <servers/netname.h>
#include <servers/netname_defs.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* flipc2.h's flipc2_ring_count() never reads its `hdr'. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <flipc2.h>
#pragma GCC diagnostic pop

#include "pipe_server.h"	/* MIG: pipe.defs */
#include "notify_server.h"	/* MIG: notify.defs */

_Static_assert(PIPE_HEADER_BYTES == FLIPC2_HEADER_SIZE &&
	       PIPE_DESC_BYTES == FLIPC2_DESC_SIZE,
	       "pipe_types.h's layout and flipc2.h's disagree");
_Static_assert(sizeof(struct uros_pipe_shared) <= PIPE_SHARED,
	       "the pipe's shared state outgrew its page");

#define PIPES_MAX	256
#define TOKENS_MAX	2048

struct pipe_rec {
	mach_port_t			port;	 /* receive right: the pipe's name;
						    MACH_PORT_NULL when free */
	mach_port_t			entry;	 /* the channel's memory entry */
	vm_address_t			base;	 /* this task's mapping of it */
	struct uros_pipe_shared		*sh;	 /* in it, at PIPE_DATA_OFFSET */
	unsigned			live[2]; /* tokens alive, by end */
	unsigned			given[2];/* tokens ever given, by end */
};

struct token_rec {
	mach_port_t	port;	/* receive right; MACH_PORT_NULL when free */
	int		pipe;	/* index in pipes[] */
	int		end;	/* PIPE_END_READ or PIPE_END_WRITE */
	uint32_t	pid;	/* what the holder's lock words hold */
};

static struct pipe_rec	pipes[PIPES_MAX];
static struct token_rec	tokens[TOKENS_MAX];

static mach_port_t	host_port;
static mach_port_t	device_port;
static mach_port_t	security_port;
static mach_port_t	root_ledger_wired;
static mach_port_t	root_ledger_paged;
static mach_port_t	pipe_service;	/* pipe_register */
static mach_port_t	port_set;	/* the service, every pipe, every token */

static const char *const end_name[2] = { "readers'", "writers'" };

static struct pipe_rec *
pipe_find(mach_port_t port)
{
	int	i;

	if (port == MACH_PORT_NULL)
		return NULL;
	for (i = 0; i < PIPES_MAX; i++)
		if (pipes[i].port == port)
			return &pipes[i];
	return NULL;
}

static struct token_rec *
token_find(mach_port_t port)
{
	int	i;

	if (port == MACH_PORT_NULL)
		return NULL;
	for (i = 0; i < TOKENS_MAX; i++)
		if (tokens[i].port == port)
			return &tokens[i];
	return NULL;
}

/*
 * The no-senders notification of `port', sent to the port itself so that it
 * names the port it is about.  sync 1: not before the send right the reply
 * makes, which is the first.
 */
static kern_return_t
watch(mach_port_t port)
{
	mach_port_t	prev = MACH_PORT_NULL;
	kern_return_t	kr;

	kr = mach_port_request_notification(mach_task_self(), port,
					    MACH_NOTIFY_NO_SENDERS, 1, port,
					    MACH_MSG_TYPE_MAKE_SEND_ONCE, &prev);
	if (kr == KERN_SUCCESS && prev != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), prev);
	return kr;
}

static void
drop_receive(mach_port_t port)
{
	(void)mach_port_mod_refs(mach_task_self(), port,
				 MACH_PORT_RIGHT_RECEIVE, -1);
}

static void
pipe_free(struct pipe_rec *p)
{
	(void)vm_deallocate(mach_task_self(), p->base, PIPE_CHANNEL_SIZE);
	(void)mach_port_deallocate(mach_task_self(), p->entry);
	drop_receive(p->port);
	memset(p, 0, sizeof *p);
}

/* Whether the process pid still holds a token on pipe pi. */
static int
pid_holds(int pi, uint32_t pid)
{
	int	i;

	for (i = 0; i < TOKENS_MAX; i++)
		if (tokens[i].port != MACH_PORT_NULL && tokens[i].pipe == pi &&
		    tokens[i].pid == pid)
			return 1;
	return 0;
}

/*
 * A lock word holding pid, which has no token on the pipe left, is freed: its
 * holder died inside a read or a write and will never let it go.  The holder
 * published all of a descriptor or none (one store), so what is left to undo
 * is the lock.  A sleeper on it is woken, as pipe_unlock() would.
 */
static void
free_lock(volatile uint32_t *w, uint32_t pid, const char *which, int pi)
{
	uint32_t	v = __atomic_load_n(w, __ATOMIC_ACQUIRE);

	while (v != 0 && (v & ~PL_WAITERS) == pid) {
		if (__atomic_compare_exchange_n(w, &v, 0, 0, __ATOMIC_RELEASE,
						__ATOMIC_ACQUIRE)) {
			if (v & PL_WAITERS)
				(void)urmach_futex((unsigned int *)w,
						   URMACH_FUTEX_WAKE, 1, 0,
						   (unsigned int *)0);
			printf("pipe_server: freed the %s lock of pipe %d, held "
			       "by pid %u, which has gone\n", which, pi,
			       (unsigned)pid);
			return;
		}
	}
}

/* The last token of an end has gone: say so, and wake the other side. */
static void
end_gone(struct pipe_rec *p, int end)
{
	struct uros_pipe_shared	*sh = p->sh;
	volatile uint32_t	*ev;

	if (end == PIPE_END_WRITE) {
		__atomic_store_n(&sh->writers_gone, 1, __ATOMIC_RELEASE);
		ev = &sh->ev_data;
	} else {
		__atomic_store_n(&sh->readers_gone, 1, __ATOMIC_RELEASE);
		ev = &sh->ev_room;
	}
	__atomic_add_fetch(ev, 1, __ATOMIC_SEQ_CST);
	(void)urmach_futex((unsigned int *)ev, URMACH_FUTEX_WAKE, 0x7fffffffu,
			   0, (unsigned int *)0);
}

static void
token_gone(struct token_rec *t)
{
	int		pi = t->pipe, end = t->end;
	uint32_t	pid = t->pid;
	struct pipe_rec	*p = &pipes[pi];

	drop_receive(t->port);
	memset(t, 0, sizeof *t);
	if (p->port == MACH_PORT_NULL || p->live[end] == 0)
		return;
	p->live[end]--;
	if (!pid_holds(pi, pid)) {
		free_lock(&p->sh->wlock, pid, end_name[PIPE_END_WRITE], pi);
		free_lock(&p->sh->rlock, pid, end_name[PIPE_END_READ], pi);
	}
	if (p->live[end] == 0)
		end_gone(p, end);
	if (p->live[PIPE_END_READ] == 0 && p->live[PIPE_END_WRITE] == 0)
		pipe_free(p);
}

/* ------------------------------------------------------------------ */
/*  pipe.defs                                                          */
/* ------------------------------------------------------------------ */

kern_return_t
pipe_S_register(mach_port_t server, mach_port_t entry, natural_t size,
		mach_port_t *pipe)
{
	struct pipe_rec	*p = NULL;
	vm_address_t	base = 0;
	mach_port_t	port = MACH_PORT_NULL;
	kern_return_t	kr;
	int		i;

	/* On a refusal the request is destroyed, the entry with it. */
	if (server != pipe_service || size != PIPE_CHANNEL_SIZE)
		return KERN_INVALID_ARGUMENT;
	for (i = 0; i < PIPES_MAX && p == NULL; i++)
		if (pipes[i].port == MACH_PORT_NULL)
			p = &pipes[i];
	if (p == NULL)
		return KERN_RESOURCE_SHORTAGE;
	kr = vm_map(mach_task_self(), &base, PIPE_CHANNEL_SIZE, 0, TRUE, entry,
		    0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
		    VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
	if (kr != KERN_SUCCESS)
		return kr;
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&port);
	if (kr == KERN_SUCCESS)
		kr = watch(port);
	if (kr == KERN_SUCCESS)
		kr = mach_port_move_member(mach_task_self(), port, port_set);
	if (kr != KERN_SUCCESS) {
		if (port != MACH_PORT_NULL)
			drop_receive(port);
		(void)vm_deallocate(mach_task_self(), base, PIPE_CHANNEL_SIZE);
		return kr;
	}
	memset(p, 0, sizeof *p);
	p->port = port;
	p->entry = entry;
	p->base = base;
	p->sh = (struct uros_pipe_shared *)(base + PIPE_DATA_OFFSET);
	*pipe = port;
	return KERN_SUCCESS;
}

kern_return_t
pipe_S_token(mach_port_t pipe, int end, int pid, mach_port_t *token)
{
	struct pipe_rec		*p = pipe_find(pipe);
	struct token_rec	*t = NULL;
	mach_port_t		port = MACH_PORT_NULL;
	kern_return_t		kr;
	int			i;

	if (p == NULL || (end != PIPE_END_READ && end != PIPE_END_WRITE))
		return KERN_INVALID_ARGUMENT;
	/* An end whose holders have all gone has none to come: only a holder
	 * can hand one on, by fork(), and it would still hold its own. */
	if (p->given[end] != 0 && p->live[end] == 0)
		return KERN_INVALID_ARGUMENT;
	for (i = 0; i < TOKENS_MAX && t == NULL; i++)
		if (tokens[i].port == MACH_PORT_NULL)
			t = &tokens[i];
	if (t == NULL)
		return KERN_RESOURCE_SHORTAGE;
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&port);
	if (kr == KERN_SUCCESS)
		kr = watch(port);
	if (kr == KERN_SUCCESS)
		kr = mach_port_move_member(mach_task_self(), port, port_set);
	if (kr != KERN_SUCCESS) {
		if (port != MACH_PORT_NULL)
			drop_receive(port);
		return kr;
	}
	t->port = port;
	t->pipe = (int)(p - pipes);
	t->end = end;
	t->pid = (uint32_t)pid;
	p->live[end]++;
	p->given[end]++;
	*token = port;
	return KERN_SUCCESS;
}

kern_return_t
pipe_S_entry(mach_port_t pipe, mach_port_t *entry)
{
	struct pipe_rec	*p = pipe_find(pipe);

	if (p == NULL)
		return KERN_INVALID_ARGUMENT;
	*entry = p->entry;
	return KERN_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  notify.defs                                                        */
/* ------------------------------------------------------------------ */

/*
 * A token with no senders left: its task let the end go or died.  A pipe's
 * port with none: nobody can ask for the pipe any more, and if no token was
 * ever given -- its maker died between pipe_register() and pipe_token() --
 * nothing else would ever free it.
 */
kern_return_t
do_mach_notify_no_senders(mach_port_t port, mach_port_mscount_t mscount)
{
	struct token_rec	*t = token_find(port);
	struct pipe_rec		*p;

	(void)mscount;
	if (t != NULL) {
		token_gone(t);
		return KERN_SUCCESS;
	}
	p = pipe_find(port);
	if (p != NULL && p->live[PIPE_END_READ] == 0 &&
	    p->live[PIPE_END_WRITE] == 0)
		pipe_free(p);
	return KERN_SUCCESS;
}

kern_return_t
do_mach_notify_port_deleted(mach_port_t notify, mach_port_t name)
{
	(void)notify;
	(void)name;
	return KERN_SUCCESS;
}

kern_return_t
do_mach_notify_port_destroyed(mach_port_t notify, mach_port_t rights)
{
	(void)notify;
	(void)rights;
	return KERN_SUCCESS;
}

kern_return_t
do_mach_notify_send_once(mach_port_t notify)
{
	(void)notify;
	return KERN_SUCCESS;
}

kern_return_t
do_mach_notify_dead_name(mach_port_t notify, mach_port_t name)
{
	(void)notify;
	(void)name;
	return KERN_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Bring-up                                                           */
/* ------------------------------------------------------------------ */

static boolean_t
pipe_demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
	if (pipe_server(in, out))
		return TRUE;
	return notify_server(in, out);
}

int
main(int argc, char **argv)
{
	mach_port_t	sig = MACH_PORT_NULL;
	kern_return_t	kr;

	(void)argc;
	(void)argv;
	kr = bootstrap_ports(bootstrap_port, &host_port, &device_port,
			     &root_ledger_wired, &root_ledger_paged,
			     &security_port);
	if (kr != KERN_SUCCESS)
		return 1;
	printf_init(device_port);
	/* Bus authority, wanted only to open a console (#511). */
	(void)mach_port_deallocate(mach_task_self(), device_port);
	panic_init(host_port);
	printf("\n=== pipe_server ===\n");

	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				&pipe_service);
	if (kr == KERN_SUCCESS)
		kr = mach_port_allocate(mach_task_self(),
					MACH_PORT_RIGHT_PORT_SET, &port_set);
	if (kr == KERN_SUCCESS)
		kr = mach_port_move_member(mach_task_self(), pipe_service,
					   port_set);
	if (kr == KERN_SUCCESS)
		kr = mach_port_insert_right(mach_task_self(), pipe_service,
					    pipe_service,
					    MACH_MSG_TYPE_MAKE_SEND);
	/* #744: signed with a port only this task holds. */
	if (kr == KERN_SUCCESS)
		kr = urmach_netname_signature(&sig);
	if (kr == KERN_SUCCESS)
		kr = netname_check_in(name_server_port, (char *)"pipe_server",
				      sig, pipe_service);
	if (kr != KERN_SUCCESS) {
		printf("pipe_server: bring-up failed, kr=%d\n", kr);
		return 1;
	}

	bootstrap_completed(bootstrap_port, mach_task_self());
	printf("pipe_server: ready, entering message loop\n");
	mach_msg_server(pipe_demux, 1024, port_set, MACH_MSG_OPTION_NONE);
	printf("pipe_server: mach_msg_server exited unexpectedly\n");
	return 1;
}
