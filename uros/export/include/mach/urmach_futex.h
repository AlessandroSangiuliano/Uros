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
 * urmach_futex (#324) — modern futex-style fast synchronization.
 *
 * A lightweight wait/wake on a 32-bit word in user memory.  The
 * uncontended fast path stays entirely in userspace (atomic compare on
 * the word); this trap is taken only when a thread must actually block
 * or wake a blocked peer — the futex model (Linux futex / FreeBSD
 * _umtx_op), replacing the heavy Mach-semaphore kernel-trap block path.
 *
 * The wait key is the physical address of the word, so the same shared
 * page mapped at different virtual addresses in two tasks (e.g. a FLIPC
 * channel header) refers to the same futex.  The word must be in
 * resident memory (FLIPC channels are wired shared memory).
 */

#ifndef _MACH_URMACH_FUTEX_H_
#define _MACH_URMACH_FUTEX_H_

#include <mach/kern_return.h>

/* op codes — MUST match the kernel (kern/sync_sema.c) */
#define URMACH_FUTEX_WAIT	0	/* if *uaddr==val, block (timeout_ms; 0=forever) */
#define URMACH_FUTEX_WAKE	1	/* wake up to `val` waiters (val==1 -> exactly one) */
#define URMACH_FUTEX_WAKE_WAIT	2	/* wake one on wake_uaddr + block on uaddr==val,
					 * with a direct hand-off (RPC/ping-pong) */

/*
 * urmach_futex(uaddr, op, val, timeout_ms, wake_uaddr)
 *   WAIT:      block if *uaddr==val.  wake_uaddr unused.
 *   WAKE:      wake `val` waiters on uaddr.  wake_uaddr unused.
 *   WAKE_WAIT: wake one waiter on wake_uaddr, then block on *uaddr==val,
 *              switching straight to the woken thread (no run-queue trip).
 * Returns: KERN_SUCCESS woken, KERN_OPERATION_TIMED_OUT, KERN_NOT_WAITING
 * (value already changed — retry), KERN_INVALID_ADDRESS.
 */
extern kern_return_t urmach_futex(unsigned int *uaddr, int op,
				  unsigned int val, unsigned int timeout_ms,
				  unsigned int *wake_uaddr);

#endif	/* _MACH_URMACH_FUTEX_H_ */
