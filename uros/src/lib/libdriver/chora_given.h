/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * chora_given.h -- the module a chora is given as it starts (#964).
 *
 * Whoever starts a chora gives it its image, its port and its capability, and
 * its module too: the bytes of the module it read and checked for the device,
 * and not a name the chora would fetch again -- from a pool whose file could
 * have changed since, or from a disk the chora has no business reading.
 *
 * They come in one message on a port of the chora's own.  The starter makes
 * the port in the chora's task, a receive right, before the task's thread
 * runs; takes a send right from it, sends this message, and lets the send
 * right go.  The chora holds the only right left, and the one message on its
 * port is the starter's.  The port's name in the chora's space is the chora's
 * fifth argument, "0x" and hexadecimal digits.
 *
 * The message is queued before the chora runs, so the chora receives it
 * without waiting: a port with nothing on it is a starter that gave nothing.
 */

#ifndef _CHORA_GIVEN_H_
#define _CHORA_GIVEN_H_

#include <mach.h>
#include <mach/message.h>
#include <string.h>

#define CHORA_GIVEN_MODULE_ID	9640		/* the message's id */
#define CHORA_GIVEN_MODULE_MAX	(16u << 20)	/* the largest module given */

typedef struct {
	mach_msg_header_t		head;
	mach_msg_body_t			body;
	mach_msg_ool_descriptor_t	module;		/* the module's bytes */
} chora_given_module_t;

/* As received: the message and the trailer the kernel adds. */
typedef struct {
	chora_given_module_t		m;
	mach_msg_trailer_t		trailer;
} chora_given_module_rcv_t;

/*
 * Give the chora in `task' -- made, its thread not yet run -- the module in
 * `data': the port made in its task, the message sent on it, and the send
 * right let go.  The port's name in the chora's task, or MACH_PORT_NULL with
 * *kr saying why.  The bytes stay the caller's: the message carries a copy.
 * The HAL gives one so, and chora_test the second chora it starts the HAL's
 * way: one function, so that the two give the same.
 */
static inline mach_port_t
chora_give_module(mach_port_t task, vm_offset_t data,
		  mach_msg_type_number_t len, kern_return_t *kr)
{
	chora_given_module_t	m;
	mach_port_t		box = MACH_PORT_NULL, send = MACH_PORT_NULL;
	mach_msg_type_name_t	type;

	*kr = mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &box);
	if (*kr == KERN_SUCCESS)
		*kr = mach_port_extract_right(task, box, MACH_MSG_TYPE_MAKE_SEND,
					      &send, &type);
	if (*kr == KERN_SUCCESS) {
		memset(&m, 0, sizeof m);
		m.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) |
				   MACH_MSGH_BITS_COMPLEX;
		m.head.msgh_size = sizeof m;
		m.head.msgh_remote_port = send;
		m.head.msgh_local_port = MACH_PORT_NULL;
		m.head.msgh_id = CHORA_GIVEN_MODULE_ID;
		m.body.msgh_descriptor_count = 1;
		m.module.address = (void *)data;
		m.module.size = len;
		m.module.deallocate = FALSE;
		m.module.copy = MACH_MSG_VIRTUAL_COPY;
		m.module.type = MACH_MSG_OOL_DESCRIPTOR;
		/* A port made now has room for it: the send never waits. */
		*kr = mach_msg(&m.head, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
			       sizeof m, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
	}
	if (send != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), send);
	return *kr == KERN_SUCCESS ? box : MACH_PORT_NULL;
}

#endif /* _CHORA_GIVEN_H_ */
