/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * pipe_types.h -- what pipe_server and libposix-uros share of a pipe (#904).
 *
 * A pipe is one flat FLIPC v2 channel (libposix-uros/posix_pipe.c): the
 * channel's header, a ring of PIPE_SLOTS descriptors, and the data region,
 * whose first PIPE_SHARED bytes are struct uros_pipe_shared and the rest the
 * byte ring.  pipe_server maps the same memory through the memory entry the
 * pipe's maker registers (#911), and finds the words it writes and the words
 * it wakes by the constants here.  Every holder may write any byte of the
 * channel, its header included, so nothing read from it says where anything
 * is.  posix_pipe.c and pipe_server.c each check PIPE_HEADER_BYTES and
 * PIPE_DESC_BYTES against <flipc2.h>.
 */

#ifndef UROS_PIPE_TYPES_H
#define UROS_PIPE_TYPES_H

#include <stdint.h>

#define PIPE_SLOTS		256u	/* descriptors: one per write in the pipe */
#define PIPE_SHARED		4096u	/* struct uros_pipe_shared's room */
#define PIPE_RING		65536u	/* the bytes a pipe holds */

/* A flat channel's layout (flipc2_channel_create_ex): the header, then the
 * descriptor ring, then the data region. */
#define PIPE_HEADER_BYTES	256u	/* FLIPC2_HEADER_SIZE */
#define PIPE_DESC_BYTES		64u	/* FLIPC2_DESC_SIZE */
#define PIPE_DATA_OFFSET	(PIPE_HEADER_BYTES + PIPE_SLOTS * PIPE_DESC_BYTES)
#define PIPE_CHANNEL_SIZE						\
	((PIPE_DATA_OFFSET + PIPE_SHARED + PIPE_RING + 4095u) & ~4095u)

/*
 * The pipe's own state, at data offset 0 of the channel.  Every holder may
 * write any of it, so nothing here is trusted to stay in bounds.
 *
 * ev_data and ev_room are event counts.  A reader that finds no data, or a
 * writer that finds no room, reads its count, looks again, and sleeps on the
 * count with the value it read; whatever would end its wait -- a write, a
 * read that frees room, the other end gone -- adds one to the count before
 * it wakes it.  So the wait cannot miss its wake: a change after the look
 * changes the word the kernel compares (#904).  The end-gone flags are set
 * by pipe_server alone, which learns of an end gone from its tokens.
 */
struct uros_pipe_shared {
	uint32_t		magic;
	uint32_t		ring_bytes;
	uint32_t		_pad0[14];

	/* The writers' line. */
	volatile uint32_t	wlock;		/* 0 free; else holder | PL_WAITERS */
	uint32_t		_pad1[15];

	/* The readers' line. */
	volatile uint32_t	rlock;
	volatile uint32_t	partial;	/* bytes of the head descriptor read */
	uint32_t		_pad2[14];

	/* Set by pipe_server when the last token of an end has gone. */
	volatile uint32_t	writers_gone;	/* end of file */
	volatile uint32_t	readers_gone;	/* EPIPE */
	uint32_t		_pad3[14];

	/* The event counts the two sides sleep on. */
	volatile uint32_t	ev_data;	/* a write, or the writers gone */
	volatile uint32_t	reader_sleeps;	/* nonzero: a reader may sleep on ev_data */
	uint32_t		_pad4[14];
	volatile uint32_t	ev_room;	/* a read, or the readers gone */
	volatile uint32_t	writer_sleeps;	/* nonzero: a writer may sleep on ev_room */
	uint32_t		_pad5[14];
};

#define UROS_PIPE_MAGIC		0x50495031u	/* "PIP1" */

/*
 * A lock word holds its holder's pid, with the top bit for "others sleep on
 * it", so that pipe_server can free the lock of a holder that died: it knows
 * a pipe's holders by the pid each token was asked for.  A task with no pid
 * holds it as PL_NO_PID.
 */
#define PL_WAITERS		0x80000000u
#define PL_NO_PID		0x7fffffffu

/* The two ends, as pipe_token() names them. */
#define PIPE_END_READ		0
#define PIPE_END_WRITE		1

#endif /* UROS_PIPE_TYPES_H */
