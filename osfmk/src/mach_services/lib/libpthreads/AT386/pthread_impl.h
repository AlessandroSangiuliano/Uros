/*
 * Copyright (c) 2026 Alessandro Sangiuliano <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * POSIX Threads - Machine implementation details (i386)
 *
 * Generated from mk_pthread_impl.c struct sizes.
 * These must match the internal structures in pthread_internals.h.
 */

#define __PTHREAD_SIZE__           132
#define __PTHREAD_ATTR_SIZE__      32
#define __PTHREAD_MUTEXATTR_SIZE__ 12
#define __PTHREAD_MUTEX_SIZE__     52
#define __PTHREAD_CONDATTR_SIZE__  4
#define __PTHREAD_COND_SIZE__      24
#define __PTHREAD_ONCE_SIZE__      4
#define __PTHREAD_RWLOCKATTR_SIZE__ 4
#define __PTHREAD_RWLOCK_SIZE__    24
#define __PTHREAD_BARRIERATTR_SIZE__ 4
#define __PTHREAD_BARRIER_SIZE__   20
#define __PTHREAD_SPIN_SIZE__      4
/*
 * [Internal] data structure signatures
 */
#define _PTHREAD_MUTEX_SIG_init		0x32AAABA7
#define _PTHREAD_COND_SIG_init		0x3CB0B1BB
#define _PTHREAD_ONCE_SIG_init		0x30B1BCBA
#define _PTHREAD_RWLOCK_SIG		0x52574C4B
/*
 * POSIX scheduling policies
 */

#define SCHED_OTHER      1
#define SCHED_FIFO       4
#define SCHED_RR         2
#define __SCHED_PARAM_SIZE__       4
