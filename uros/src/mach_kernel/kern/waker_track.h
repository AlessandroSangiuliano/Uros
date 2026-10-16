/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * WAKER_TRACK (#687) -- who last ended a thread's wait, and whether an IPC
 * wakeup still meets the wait it was meant for.
 *
 * #687 came back once in 11 669 campaign boots of trees with its two fixes: a
 * thread parked by mach_msg's hot path woke with THREAD_AWAKENED and nothing
 * delivered.  Every waker read in the tree keeps #700's rule, and the panic
 * could not say which one broke it.  With this, each way a wait ends writes
 * its caller into the thread, the panics print it, and thread_go() refuses a
 * wakeup for an IPC wait the thread has already left -- the late wakeup is
 * stopped where it is made, with the waker's own stack on the screen.
 *
 * x86-64 only: a few stores on every wakeup, and i386's .text stays as it is.
 * The fields are in <kern/thread.h>, the record in <kern/sched_prim.h>, and
 * the IPC park count in <ipc/ipc_thread.h>; all three read this switch.
 */

#ifndef	_KERN_WAKER_TRACK_H_
#define	_KERN_WAKER_TRACK_H_

#if	defined(__x86_64__)
#define	WAKER_TRACK	1
#else
#define	WAKER_TRACK	0
#endif

#endif	/* _KERN_WAKER_TRACK_H_ */
