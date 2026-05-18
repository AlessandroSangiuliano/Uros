/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * kern/klog.c — kernel printf ring buffer (#200).  See klog.h.
 *
 * Design: single 64 KiB byte-circular buffer protected by
 * klog_lock.  `head` counts total bytes appended (mod 2^32);
 * `tail` is `head - KLOG_BUF_SIZE` once the buffer fills, otherwise
 * 0.  Both are natural_t — the natural type for the MIG cursor.
 *
 * Locking: simple_lock is enough.  klog_putc is called from inside
 * printf() which already holds printf_lock, but klog_putc must also
 * be safe from contexts that bypass printf() (panic prefix, kdb
 * single-step prints).  Holding a separate lock costs nothing on UP
 * and is correct on MP.
 */

#include <kern/lock.h>
#include <kern/klog.h>
#include <kern/host.h>
#include <string.h>

static char		klog_buf[KLOG_BUF_SIZE];
static natural_t		klog_head;
static natural_t		klog_tail;
static int		klog_ready;
decl_simple_lock_data(static, klog_lock)

void
klog_init(void)
{
	simple_lock_init(&klog_lock, ETAP_MISC_PRINTF);
	klog_ready = 1;
}

void
klog_putc(char c)
{
	if (!klog_ready) {
		/* Pre-init printf: drop on the floor.  Same path the
		 * old kernel had — these chars went only to serial. */
		return;
	}
	simple_lock(&klog_lock);
	klog_buf[klog_head % KLOG_BUF_SIZE] = c;
	klog_head++;
	if ((natural_t)(klog_head - klog_tail) > KLOG_BUF_SIZE)
		klog_tail = klog_head - KLOG_BUF_SIZE;
	simple_unlock(&klog_lock);
}

kern_return_t
klog_read(natural_t start,
	  char *buf,
	  mach_msg_type_number_t *count,
	  natural_t *next)
{
	natural_t avail;
	natural_t want;
	natural_t i;
	natural_t copy_start;
	natural_t cap = *count;

	if (!klog_ready) {
		*count = 0;
		*next = 0;
		return KERN_SUCCESS;
	}

	simple_lock(&klog_lock);

	/* If reader's cursor is outside [tail, head] (more than
	 * KLOG_BUF_SIZE bytes behind, or ahead of head), snap it to
	 * tail.  We computed in unsigned 32-bit modular space so an
	 * "ahead" cursor (>head) wraps into a huge `behind`. */
	avail = klog_head - start;
	if (avail > KLOG_BUF_SIZE) {
		start = klog_tail;
		avail = klog_head - start;
	}

	want = avail < cap ? avail : cap;
	copy_start = start;
	for (i = 0; i < want; i++)
		buf[i] = klog_buf[(copy_start + i) % KLOG_BUF_SIZE];

	*count = want;
	*next = start + want;

	simple_unlock(&klog_lock);
	return KERN_SUCCESS;
}

/*
 * MIG entry point (mach_klog_server.c): drop the host arg, defer to
 * klog_read.  Any task with a host_t name can drain — printf already
 * goes out on the serial console where anyone with the right device
 * could see it, so we don't gate on host_priv.
 */
kern_return_t
host_get_log(host_t host,
	     natural_t start,
	     char *buf,
	     mach_msg_type_number_t *count,
	     natural_t *next)
{
	if (host == HOST_NULL)
		return KERN_INVALID_HOST;
	return klog_read(start, buf, count, next);
}
