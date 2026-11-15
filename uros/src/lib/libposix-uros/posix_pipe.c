/*
 * libposix-uros — pipes (#904).
 *
 * A pipe is one FLIPC v2 channel (flipc2.h), in memory that every holder
 * maps: the task that made it, and every task forked from a holder after,
 * since the region is inherited shared and not copied.  The bytes go into a
 * ring in the channel's data region, and each write is one descriptor, or
 * two where it wraps.  A read takes bytes from the descriptor at the head
 * and releases it once it has taken all of it.  No server is in the path: a
 * write that does not have to wait and a read that finds data cost no trip
 * to the kernel.
 *
 * Writers take the writers' lock and readers the readers' lock, both in the
 * shared page and both futexes keyed on it.  Under them FLIPC's single
 * producer, single consumer protocol is the one FLIPC has: the pipe layer is
 * around flipc2_*, not in it, and FLIPC's fast path is what it was.  The
 * writers' lock is what keeps a write of up to PIPE_BUF bytes whole when
 * several tasks write.  And a write is published by flipc2_produce_commit(),
 * one store, so a writer that dies holding the lock has published all of a
 * descriptor or none of it: what is left to undo is the lock.
 *
 * 🔴 NOT HERE YET, AND WAITING FOR #911 (memory entries).  End of file and
 * EPIPE: pipe_server will set writers_gone and readers_gone when the last
 * holder of an end lets it go or dies, as only it can learn that.  The
 * release of a lock whose holder died, which is pipe_server's too.  And a
 * pipe's ends across execve().  Until then a read on a pipe whose writers
 * have all gone waits for ever, and a pipe's descriptors do not cross exec.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <mach_init.h>          /* mach_task_self, name_server_port */
#include <mach/vm_inherit.h>
#include <servers/netname.h>    /* netname_look_up: pipe_server's port */
#include "mach.h"               /* MIG: vm_inherit */
#include "pipe.h"               /* MIG: pipe.defs, pipe_server (#904) */

/*
 * flipc2.h's flipc2_ring_count() never reads its `hdr', and this library is
 * built with -Wextra -Werror where FLIPC's users are not.  Off for that header
 * alone; flipc2.h is #911's to change now, and the parameter goes with the
 * part of #904 that is in libflipc2.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <flipc2.h>
#pragma GCC diagnostic pop

#include "posix_pipe.h"
#include "pipe_types.h"		/* the layout pipe_server shares (#904) */

extern unsigned int __uros_my_pid;	/* signals.c */

/*
 * The channel.  A flat one: every holder reads and writes all of it, as
 * every holder may hold both ends.
 *
 *   FLIPC header           256 bytes
 *   descriptor ring        PIPE_SLOTS descriptors, one per write in the pipe
 *   data region:
 *     struct uros_pipe_shared   PIPE_SHARED bytes, at data offset 0
 *     the byte ring             PIPE_RING bytes, the bytes a pipe holds
 *
 * The constants and the shared state are pipe_types.h's: pipe_server finds
 * the words it writes by them, in a mapping of its own.
 */
_Static_assert(PIPE_HEADER_BYTES == FLIPC2_HEADER_SIZE &&
	       PIPE_DESC_BYTES == FLIPC2_DESC_SIZE,
	       "pipe_types.h's layout and flipc2.h's disagree");

#define PIPE_OP_DATA	1u

/* A position is a byte count since the pipe was made, kept modulo 2^32: the
 * ring's size divides 2^32, so a position's offset in the ring survives the
 * wrap of the count. */
_Static_assert((PIPE_RING & (PIPE_RING - 1)) == 0,
	       "the byte ring's size must be a power of two");
_Static_assert(PIPE_BUF <= PIPE_RING,
	       "a write of PIPE_BUF bytes must fit the ring whole");

_Static_assert(sizeof(struct uros_pipe_shared) <= PIPE_SHARED,
	       "the pipe's shared state outgrew its page");

/* This task's pipes: as many as it can have descriptors. */
#define PIPES_MAX	64

static struct uros_pipe	pipes[PIPES_MAX];
static pthread_mutex_t	pipes_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/*  The two locks                                                      */
/* ------------------------------------------------------------------ */

/*
 * A lock word holds its holder's pid, with the top bit for "others sleep on
 * it" (PL_WAITERS, PL_NO_PID: pipe_types.h), so that pipe_server can free the
 * lock of a holder that died: it knows the holders of a pipe by the pid each
 * token was asked for.  A forked child has a pid of its own, and the word is
 * in the shared page, so a fork never copies a lock as held.
 */
static uint32_t
pipe_owner(void)
{
	uint32_t	id = __uros_my_pid;

	return (id != 0 && id < PL_NO_PID) ? id : PL_NO_PID;
}

static void
pipe_lock(volatile uint32_t *w)
{
	uint32_t	me = pipe_owner(), v = 0;

	if (__atomic_compare_exchange_n(w, &v, me, 0, __ATOMIC_ACQUIRE,
					__ATOMIC_RELAXED))
		return;
	for (;;) {
		v = __atomic_load_n(w, __ATOMIC_RELAXED);
		if (v == 0) {
			/* Free, and others may sleep on it: keep them marked. */
			if (__atomic_compare_exchange_n(w, &v, me | PL_WAITERS,
							0, __ATOMIC_ACQUIRE,
							__ATOMIC_RELAXED))
				return;
			continue;
		}
		if ((v & PL_WAITERS) == 0) {
			if (!__atomic_compare_exchange_n(w, &v, v | PL_WAITERS,
							 0, __ATOMIC_RELAXED,
							 __ATOMIC_RELAXED))
				continue;
			v |= PL_WAITERS;
		}
		/* Shared, not URMACH_FUTEX_PRIVATE: the holders map the page
		 * at addresses of their own, and the kernel keys on the page. */
		(void)urmach_futex((unsigned int *)w, URMACH_FUTEX_WAIT, v, 0,
				   (unsigned int *)0);
	}
}

static void
pipe_unlock(volatile uint32_t *w)
{
	if (__atomic_exchange_n(w, 0, __ATOMIC_RELEASE) & PL_WAITERS)
		(void)urmach_futex((unsigned int *)w, URMACH_FUTEX_WAKE, 1, 0,
				   (unsigned int *)0);
}

/* ------------------------------------------------------------------ */
/*  Where the bytes are                                                */
/* ------------------------------------------------------------------ */

/* Where the next write goes: just past the last descriptor written.  The
 * writers' lock keeps prod_tail still.  A ring never written reads its last
 * slot zeroed, which is position 0. */
static uint32_t
tail_pos(flipc2_channel_t ch)
{
	const struct flipc2_desc *d =
		&ch->ring[(*ch->prod_tail - 1) & ch->ring_mask];

	return (uint32_t)d->param[0] + (uint32_t)d->data_length;
}

/* The oldest byte not yet released: where the head descriptor starts, or
 * with none, `tail'.  A reader releasing meanwhile only makes it older than
 * it is, which leaves less room than there is, never more. */
static uint32_t
head_pos(flipc2_channel_t ch, uint32_t tail)
{
	uint32_t	head = *ch->cons_head;

	if (head == *ch->prod_tail)
		return tail;
	return (uint32_t)ch->ring[head & ch->ring_mask].param[0];
}

/*
 * The waits sleep on the event counts of pipe_types.h, not on FLIPC's
 * cons_head and prod_tail, for one reason: a wait must also end when the
 * other end goes, and that is pipe_server's flag, in another word.  Slept on
 * cons_head, a writer that looked at readers_gone just before pipe_server set
 * it would sleep for good, since the kernel compares only the word it sleeps
 * on, and nothing would move cons_head again.
 *
 * The count is read at the top of the caller's loop, before it looks at the
 * ring, and passed here.  Whatever ends the wait -- a release, a commit, the
 * other end gone -- adds one to the count, then looks whether anybody sleeps.
 * So either the sleeper's count is no longer the kernel's, and it does not
 * sleep, or the waker sees the sleeper's flag, and wakes it: the store of
 * the flag and the add are each ordered before the other side's load.  One
 * writer and one reader at most sleep here, each holding its side's lock.
 */
static void
wait_for_room(struct uros_pipe_shared *sh, uint32_t ev)
{
	__atomic_store_n(&sh->writer_sleeps, 1, __ATOMIC_SEQ_CST);
	if (!__atomic_load_n(&sh->readers_gone, __ATOMIC_SEQ_CST))
		(void)urmach_futex((unsigned int *)&sh->ev_room,
				   URMACH_FUTEX_WAIT, ev, 0, (unsigned int *)0);
	__atomic_store_n(&sh->writer_sleeps, 0, __ATOMIC_RELAXED);
}

static void
wait_for_data(struct uros_pipe_shared *sh, uint32_t ev)
{
	__atomic_store_n(&sh->reader_sleeps, 1, __ATOMIC_SEQ_CST);
	if (!__atomic_load_n(&sh->writers_gone, __ATOMIC_SEQ_CST))
		(void)urmach_futex((unsigned int *)&sh->ev_data,
				   URMACH_FUTEX_WAIT, ev, 0, (unsigned int *)0);
	__atomic_store_n(&sh->reader_sleeps, 0, __ATOMIC_RELAXED);
}

/* A reader released a descriptor: room for a writer that waits for it. */
static void
room_made(struct uros_pipe_shared *sh)
{
	__atomic_add_fetch(&sh->ev_room, 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&sh->writer_sleeps, __ATOMIC_SEQ_CST))
		(void)urmach_futex((unsigned int *)&sh->ev_room,
				   URMACH_FUTEX_WAKE, 1, 0, (unsigned int *)0);
}

/* A writer committed a descriptor: data for a reader that waits for it. */
static void
data_made(struct uros_pipe_shared *sh)
{
	__atomic_add_fetch(&sh->ev_data, 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&sh->reader_sleeps, __ATOMIC_SEQ_CST))
		(void)urmach_futex((unsigned int *)&sh->ev_data,
				   URMACH_FUTEX_WAKE, 1, 0, (unsigned int *)0);
}

/* n bytes from the caller's pieces, from piece *k at *koff on. */
static void
copy_in(uint8_t *dst, size_t n, const struct uros_pipe_iov *iov, int iovcnt,
	int *k, size_t *koff)
{
	while (n > 0 && *k < iovcnt) {
		size_t	avail = iov[*k].len - *koff;
		size_t	take = avail < n ? avail : n;

		memcpy(dst, (const uint8_t *)iov[*k].base + *koff, take);
		dst += take;
		n -= take;
		*koff += take;
		if (*koff == iov[*k].len) {
			(*k)++;
			*koff = 0;
		}
	}
}

/* n bytes into the caller's pieces, from piece *k at *koff on. */
static void
copy_out(const struct uros_pipe_iov *iov, int iovcnt, int *k, size_t *koff,
	 const uint8_t *src, size_t n)
{
	while (n > 0 && *k < iovcnt) {
		size_t	room = iov[*k].len - *koff;
		size_t	take = room < n ? room : n;

		memcpy((uint8_t *)iov[*k].base + *koff, src, take);
		src += take;
		n -= take;
		*koff += take;
		if (*koff == iov[*k].len) {
			(*k)++;
			*koff = 0;
		}
	}
}

/* The bytes the pieces hold, or -1 past what a read or write may return. */
static long
iov_total(const struct uros_pipe_iov *iov, int iovcnt, size_t *out)
{
	size_t	total = 0;
	int	i;

	if (iovcnt < 0)
		return -1;
	for (i = 0; i < iovcnt; i++) {
		if (iov[i].len > (size_t)SSIZE_MAX - total)
			return -1;
		total += iov[i].len;
	}
	*out = total;
	return 0;
}

/* ------------------------------------------------------------------ */
/*  The two ends                                                       */
/* ------------------------------------------------------------------ */

long
__uros_pipe_write(struct uros_pipe *p, const struct uros_pipe_iov *iov,
		  int iovcnt, int nonblock)
{
	flipc2_channel_t	ch = p->ch;
	struct uros_pipe_shared	*sh = p->sh;
	uint8_t			*ring = (uint8_t *)ch->data + PIPE_SHARED;
	size_t			total, done = 0, koff = 0;
	int			k = 0;
	long			stopped = 0;	/* why it stopped short */

	if (iov_total(iov, iovcnt, &total) != 0)
		return -EINVAL;
	if (total == 0)
		return 0;

	pipe_lock(&sh->wlock);
	/* Another task's writer moved cons_head on since this handle looked. */
	ch->cached_cons_head = *ch->cons_head;
	while (done < total) {
		/* Before the look at the ring: see wait_for_room(). */
		uint32_t		ev = __atomic_load_n(&sh->ev_room,
							     __ATOMIC_ACQUIRE);
		uint32_t		tail = *ch->prod_tail;
		uint32_t		tpos = tail_pos(ch);
		uint32_t		used = tpos - head_pos(ch, tpos);
		uint32_t		slots = ch->ring_entries -
					(tail - *ch->cons_head);
		uint32_t		off = tpos & (PIPE_RING - 1);
		size_t			left = total - done, chunk;
		size_t			need_bytes = 1;
		uint32_t		need_slots = 1;
		struct flipc2_desc	*d;

		if (sh->readers_gone) {
			stopped = -EPIPE;
			break;
		}
		if (used > PIPE_RING || slots > ch->ring_entries) {
			/* not a state a pipe leaves: its memory was written */
			stopped = -EIO;
			break;
		}
		/* Up to PIPE_BUF bytes go in whole or wait, in two descriptors
		 * where they wrap; a longer write goes in as it fits. */
		if (total <= PIPE_BUF && done == 0) {
			need_bytes = total;
			need_slots = off + total > PIPE_RING ? 2 : 1;
		}
		if (PIPE_RING - used < need_bytes || slots < need_slots) {
			if (nonblock) {
				stopped = -EAGAIN;
				break;
			}
			wait_for_room(sh, ev);
			continue;
		}
		chunk = left;
		if (chunk > PIPE_RING - used)
			chunk = PIPE_RING - used;
		if (chunk > PIPE_RING - off)
			chunk = PIPE_RING - off;
		d = flipc2_produce_reserve(ch);
		if (d == NULL) {
			stopped = -EIO;
			break;
		}
		copy_in(ring + off, chunk, iov, iovcnt, &k, &koff);
		d->opcode = PIPE_OP_DATA;
		d->flags = FLIPC2_FLAG_DATA_INLINE;
		d->cookie = 0;
		d->data_offset = PIPE_SHARED + off;
		d->data_length = chunk;
		d->param[0] = tpos;
		d->param[1] = 0;
		d->param[2] = 0;
		d->status = 0;
		flipc2_produce_commit(ch);
		data_made(sh);
		done += chunk;
	}
	pipe_unlock(&sh->wlock);
	/* What went in, if anything did: a write is short, not failed. */
	return done ? (long)done : stopped;
}

long
__uros_pipe_read(struct uros_pipe *p, const struct uros_pipe_iov *iov,
		 int iovcnt, int nonblock)
{
	flipc2_channel_t	ch = p->ch;
	struct uros_pipe_shared	*sh = p->sh;
	size_t			total, got = 0, koff = 0;
	int			k = 0;
	long			stopped = 0;	/* 0: end of file */

	if (iov_total(iov, iovcnt, &total) != 0)
		return -EINVAL;
	if (total == 0)
		return 0;

	pipe_lock(&sh->rlock);
	/* Another task's reader moved prod_tail's view on since this one. */
	ch->cached_prod_tail = *ch->prod_tail;
	for (;;) {
		/* Before the look at the ring: see wait_for_room(). */
		uint32_t		ev = __atomic_load_n(&sh->ev_data,
							     __ATOMIC_ACQUIRE);
		struct flipc2_desc	*d = flipc2_consume_peek(ch);
		uint64_t		off, len;
		uint32_t		part;
		size_t			take;

		if (d == NULL) {
			if (got > 0 || sh->writers_gone)
				break;		/* what there was, or end of file */
			if (nonblock) {
				stopped = -EAGAIN;
				break;
			}
			wait_for_data(sh, ev);
			continue;
		}
		/*
		 * A descriptor is written by whoever holds the write end, and
		 * the pipe's state by any holder: each is read once, and taken
		 * from within the ring or not at all.  The check is against
		 * this file's constants, not flipc2_data_ptr_safe(), which
		 * bounds by the header's data_size, in the same memory.
		 */
		off = d->data_offset;
		len = d->data_length;
		part = sh->partial;
		if (len > PIPE_RING || off < PIPE_SHARED ||
		    off - PIPE_SHARED > PIPE_RING - len || part > len) {
			stopped = -EIO;
			break;
		}
		take = (size_t)(len - part);
		if (take > total - got)
			take = total - got;
		copy_out(iov, iovcnt, &k, &koff,
			 (const uint8_t *)ch->data + off + part, take);
		got += take;
		part += (uint32_t)take;
		if (part == len) {
			sh->partial = 0;
			flipc2_consume_release(ch);
			room_made(sh);
		} else {
			sh->partial = part;
		}
		if (got == total)
			break;
	}
	pipe_unlock(&sh->rlock);
	/* What came out, if anything did: a read is short, not failed. */
	return got ? (long)got : stopped;
}

/* ------------------------------------------------------------------ */
/*  pipe_server                                                        */
/* ------------------------------------------------------------------ */

/*
 * pipe_server's service port, asked of the name server once per task.  One
 * try, not proc_server's patient ones: where there is no pipe_server -- i386,
 * or a bundle without it -- a pipe works as it did before it, never saying
 * that an end has gone, and pipe() must not wait for a server that will not
 * come.  MACH_PORT_DEAD keeps "there is none".  A forked child holds the same
 * right under the same name: posix_fork.c gives it, with the pipes.
 */
static mach_port_t	pipe_service_port = MACH_PORT_NULL;

static mach_port_t
pipe_service(void)
{
	char		host[80] = "";
	char		serv[80] = "pipe_server";
	mach_port_t	port = MACH_PORT_NULL;

	pthread_mutex_lock(&pipes_lock);
	if (pipe_service_port == MACH_PORT_NULL) {
		if (name_server_port == MACH_PORT_NULL ||
		    netname_look_up(name_server_port, host, serv, &port)
		    != NETNAME_SUCCESS || port == MACH_PORT_NULL)
			port = MACH_PORT_DEAD;
		pipe_service_port = port;
	}
	port = pipe_service_port;
	pthread_mutex_unlock(&pipes_lock);
	return port == MACH_PORT_DEAD ? MACH_PORT_NULL : port;
}

/* A send that failed before it left: MIG gives back an error of message.h's
 * MACH_SEND_* range, and the rights the message was to move are the
 * caller's still.  Any later error means they went with the message. */
#define PIPE_SEND_FAILED(kr)	(((unsigned int)(kr) & ~0x3fffu) == 0x10000000u)

/*
 * The pipe registered with pipe_server, and a token for each end: 0, or a
 * negative errno with nothing kept.  No pipe_server: 0, and none of it.
 */
static int
pipe_witness(struct uros_pipe *p)
{
	mach_port_t	service = pipe_service();
	mach_port_t	entry = MACH_PORT_NULL, port = MACH_PORT_NULL;
	mach_port_t	tok[2] = { MACH_PORT_NULL, MACH_PORT_NULL };
	int		me = (int)pipe_owner();
	kern_return_t	kr;

	p->port = MACH_PORT_NULL;
	p->token[PIPE_END_READ] = p->token[PIPE_END_WRITE] = MACH_PORT_NULL;
	p->ends[PIPE_END_READ] = p->ends[PIPE_END_WRITE] = 0;
	if (service == MACH_PORT_NULL)
		return 0;
	if (flipc2_channel_make_entry(p->ch, PIPE_CHANNEL_SIZE,
				      FLIPC2_ROLE_PRODUCER, &entry)
	    != FLIPC2_SUCCESS)
		return -ENFILE;
	kr = pipe_register(service, entry, PIPE_CHANNEL_SIZE, &port);
	if (kr != KERN_SUCCESS) {
		if (PIPE_SEND_FAILED(kr))
			(void)mach_port_deallocate(mach_task_self(), entry);
		return -ENFILE;
	}
	kr = pipe_token(port, PIPE_END_READ, me, &tok[PIPE_END_READ]);
	if (kr == KERN_SUCCESS)
		kr = pipe_token(port, PIPE_END_WRITE, me, &tok[PIPE_END_WRITE]);
	if (kr != KERN_SUCCESS) {
		/* pipe_server lets the pipe go with these, its tokens' and its
		 * port's last senders. */
		if (tok[PIPE_END_READ] != MACH_PORT_NULL)
			(void)mach_port_deallocate(mach_task_self(),
						   tok[PIPE_END_READ]);
		(void)mach_port_deallocate(mach_task_self(), port);
		return -ENFILE;
	}
	p->port = port;
	p->token[PIPE_END_READ] = tok[PIPE_END_READ];
	p->token[PIPE_END_WRITE] = tok[PIPE_END_WRITE];
	return 0;
}

void
__uros_pipe_end_get(struct uros_pipe *p, int end)
{
	p->ends[end]++;
}

unsigned int
__uros_pipe_end_put(struct uros_pipe *p, int end)
{
	unsigned int	t = MACH_PORT_NULL;

	if (p->ends[end] > 0 && --p->ends[end] == 0) {
		t = p->token[end];
		p->token[end] = MACH_PORT_NULL;
	}
	return t;
}

/*
 * This task leaves an end: said to pipe_server before close() returns, so
 * that a read with O_NONBLOCK right after the last writer's close() answers
 * end of file and not EAGAIN -- the token's no-senders notification says the
 * same, but later.  The name is then a dead one, or still the right if
 * pipe_server could not be told, and goes either way.
 */
void
__uros_pipe_token_put(unsigned int token)
{
	if (token == MACH_PORT_NULL)
		return;
	(void)pipe_leave((mach_port_t)token);
	(void)mach_port_deallocate(mach_task_self(), (mach_port_t)token);
}

/* A copy of this task's send right `name' into `child', under that name. */
static void
copy_send(mach_port_t child, mach_port_t name)
{
	mach_port_t		right = MACH_PORT_NULL;
	mach_msg_type_name_t	got = 0;

	if (mach_port_extract_right(mach_task_self(), name,
				    MACH_MSG_TYPE_COPY_SEND, &right, &got)
	    != KERN_SUCCESS)
		return;
	if (mach_port_insert_right(child, name, right, got) != KERN_SUCCESS)
		(void)mach_port_deallocate(mach_task_self(), right);
}

/*
 * fork(): the child's hold on this task's pipes, given by posix_fork.c before
 * the child's first instruction.  Its memory is a copy of this task's, the
 * pipe table and every name in it included, and its IPC space is empty.  So
 * it is given pipe_server's service port and each pipe's port under the names
 * it knows them by, and for each end this task holds a token of its own,
 * asked for its own pid, under the name of this task's.  Its locks are then
 * its pid's, and its going is its own tokens' no-senders.  A token refused
 * leaves the child without that end's, its copy of the name cleared, so that
 * its close() gives back nothing: pipe_server sees the end go with this task,
 * as if the child had closed it.
 */
void
__uros_pipe_fork(mach_port_t child, unsigned int child_pid)
{
	unsigned int	none = MACH_PORT_NULL;
	int		i, e;

	pthread_mutex_lock(&pipes_lock);
	if (pipe_service_port != MACH_PORT_NULL &&
	    pipe_service_port != MACH_PORT_DEAD)
		copy_send(child, pipe_service_port);
	for (i = 0; i < PIPES_MAX; i++) {
		struct uros_pipe	*p = &pipes[i];

		if (!p->in_use || p->port == MACH_PORT_NULL)
			continue;
		copy_send(child, p->port);
		for (e = PIPE_END_READ; e <= PIPE_END_WRITE; e++) {
			mach_port_t	t = MACH_PORT_NULL;

			if (p->token[e] == MACH_PORT_NULL)
				continue;
			if (pipe_token(p->port, e, (int)child_pid, &t)
			    == KERN_SUCCESS &&
			    mach_port_insert_right(child, p->token[e], t,
						   MACH_MSG_TYPE_MOVE_SEND)
			    == KERN_SUCCESS)
				continue;
			if (t != MACH_PORT_NULL)
				(void)mach_port_deallocate(mach_task_self(), t);
			(void)vm_write(child, (vm_address_t)&p->token[e],
				       (vm_offset_t)&none, sizeof none);
		}
	}
	pthread_mutex_unlock(&pipes_lock);
}

/* ------------------------------------------------------------------ */
/*  Making and letting go                                              */
/* ------------------------------------------------------------------ */

int
__uros_pipe_new(struct uros_pipe **out)
{
	flipc2_channel_t	ch = NULL;
	mach_port_t		sem = MACH_PORT_NULL;
	struct uros_pipe	*p = NULL;
	struct uros_pipe_shared	*sh;
	int			i, rc;

	if (flipc2_channel_create_ex(PIPE_CHANNEL_SIZE, PIPE_SLOTS,
				     FLIPC2_CREATE_FLAT, &ch, &sem)
	    != FLIPC2_SUCCESS)
		return -ENOMEM;
	/*
	 * This task's handle owns its mapping, so that flipc2_channel_detach()
	 * unmaps it and leaves the channel whole for the other holders:
	 * flipc2_channel_destroy() would mark it dead for all of them.
	 */
	ch->mapped_size = PIPE_CHANNEL_SIZE;
	/* What fork() shares instead of copying (task_create's inherit). */
	if (vm_inherit(mach_task_self(), (vm_address_t)ch->hdr,
		       PIPE_CHANNEL_SIZE, VM_INHERIT_SHARE) != KERN_SUCCESS ||
	    ch->hdr->data_size < PIPE_SHARED + PIPE_RING) {
		(void)flipc2_channel_detach(ch);
		return -ENOMEM;
	}
	sh = (struct uros_pipe_shared *)flipc2_data_ptr(ch, 0);
	/* Where pipe_server will look for it, by pipe_types.h's constants. */
	if ((uint8_t *)sh != (uint8_t *)ch->hdr + PIPE_DATA_OFFSET) {
		(void)flipc2_channel_detach(ch);
		return -EIO;
	}
	memset(sh, 0, sizeof *sh);
	sh->magic = UROS_PIPE_MAGIC;
	sh->ring_bytes = PIPE_RING;

	pthread_mutex_lock(&pipes_lock);
	for (i = 0; i < PIPES_MAX; i++)
		if (!pipes[i].in_use) {
			p = &pipes[i];
			p->in_use = 1;
			break;
		}
	pthread_mutex_unlock(&pipes_lock);
	if (p == NULL) {
		(void)flipc2_channel_detach(ch);
		return -ENFILE;
	}
	p->refs = 0;
	p->ch = ch;
	p->sh = sh;
	rc = pipe_witness(p);
	if (rc != 0) {
		(void)flipc2_channel_detach(ch);
		pthread_mutex_lock(&pipes_lock);
		p->ch = NULL;
		p->sh = NULL;
		p->in_use = 0;
		pthread_mutex_unlock(&pipes_lock);
		return rc;
	}
	*out = p;
	return 0;
}

void
__uros_pipe_free(struct uros_pipe *p)
{
	/* A token left is a description never made, on a pipe2() that failed;
	 * the last holder's going is pipe_server's to see, by these rights. */
	__uros_pipe_token_put(p->token[PIPE_END_READ]);
	__uros_pipe_token_put(p->token[PIPE_END_WRITE]);
	if (p->port != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(),
					   (mach_port_t)p->port);
	(void)flipc2_channel_detach(p->ch);
	pthread_mutex_lock(&pipes_lock);
	p->ch = NULL;
	p->sh = NULL;
	p->port = MACH_PORT_NULL;
	p->token[PIPE_END_READ] = p->token[PIPE_END_WRITE] = MACH_PORT_NULL;
	p->ends[PIPE_END_READ] = p->ends[PIPE_END_WRITE] = 0;
	p->refs = 0;
	p->in_use = 0;
	pthread_mutex_unlock(&pipes_lock);
}
