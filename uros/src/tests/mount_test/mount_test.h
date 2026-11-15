/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What mount_test.c and mount_liar.c share (#716): the files the liar serves,
 * how it answers each, and what it counts.  Nothing here names a vfs_ function,
 * so both sides can include it.
 */

#ifndef MOUNT_TEST_H
#define MOUNT_TEST_H

#include <mach.h>
#include <stdint.h>

/*
 * The files under /mount_test, by the handle the liar's fs_open gives each.
 * Every one is LIAR_SIZE bytes long; how it is read is what differs.
 */
enum {
	LIAR_HONEST = 1,	/* 'M', on the Mach path */
	LIAR_MORE,		/* a Mach read answered LIAR_MORE_BY bytes longer */
	LIAR_WRITE_MORE,	/* a Mach write answered as one byte more */
	LIAR_FLIPC_HONEST,	/* 'F' through FLIPC, 'M' on the Mach path */
	LIAR_FLIPC_MORE,	/* a FLIPC read answered LIAR_FLIPC_MORE_BY longer */
	LIAR_FLIPC_OUTSIDE,	/* a FLIPC read answered from before the region */
	LIAR_FILES
};

#define LIAR_SIZE		16
#define LIAR_MORE_BY		4096
#define LIAR_FLIPC_MORE_BY	8

/* Requests served per file, on each path; written by the liar's threads. */
extern volatile uint32_t liar_mach_reads[LIAR_FILES];
extern volatile uint32_t liar_flipc_reads[LIAR_FILES];

/*
 * #911: what the liar's fs_open was handed to learn its client's death by --
 * how many opens it answered, and how many of them came with a task port.
 */
extern volatile uint32_t liar_opens;
extern volatile uint32_t liar_opens_with_a_task;

/*
 * #911: whether `name', in this task's space, is a task port, asked of the
 * kernel by a trap that sends nothing: vm_deallocate() of nothing answers
 * KERN_SUCCESS for a task and refuses every other port.  A message would be
 * no question at all -- sent to a port that is not the kernel's, it would sit
 * there unanswered.
 */
int mt_is_task(mach_port_t name);

/* Serve `port' on a thread of its own.  NULL, or what failed. */
const char *liar_start(mach_port_t port);

/* Create the FLIPC endpoint and accept on it.  NULL, or what failed. */
const char *liar_flipc_start(void);

/*
 * #574: a port-destroyed notification, sent by the kernel to this task and
 * handed to the stub MIG generates for it (mount_notify.c).  NULL when the
 * stub took it and the receive right came back; else what went wrong.  The
 * id and the size the message said it had, as they arrived.
 */
const char *mt_port_destroyed(mach_msg_id_t *id, mach_msg_size_t *size);

#endif /* MOUNT_TEST_H */
