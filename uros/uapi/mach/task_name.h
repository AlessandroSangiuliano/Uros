/*
 * Copyright (c) 2026 Alessandro Sangiuliano <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * How long a task's name is (#963).  One definition, reachable from both
 * halves: mach.defs includes this file, where cpp gives MIG its size, and
 * <mach/mach_types.h> declares urmach_task_name_t from it, for the C the
 * stubs are compiled as.  Defines only, so that MIG can read it.
 *
 * 31 characters and their terminator.  A thread's name holds 15, and a
 * driver task is named after its device: "virtio_blk@00:04.0" is 18, and its
 * form with the PCI domain, "virtio_blk@0000:00:04.0", 23.  A name that does
 * not fit is refused, not cut (#963): two devices cut to the same characters
 * could not be told apart.
 */

#ifndef	_MACH_TASK_NAME_H_
#define	_MACH_TASK_NAME_H_

#define	URMACH_TASK_NAME_MAX	32	/* 31 characters and the terminator */

#endif	/* _MACH_TASK_NAME_H_ */
