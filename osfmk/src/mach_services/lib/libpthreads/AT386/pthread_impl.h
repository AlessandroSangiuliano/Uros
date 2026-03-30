/*
 * POSIX Threads - Machine implementation details (i386)
 *
 * Generated from mk_pthread_impl.c struct sizes.
 * These must match the internal structures in pthread_internals.h.
 */

#define __PTHREAD_SIZE__           136
#define __PTHREAD_ATTR_SIZE__      28
#define __PTHREAD_MUTEXATTR_SIZE__ 8
#define __PTHREAD_MUTEX_SIZE__     40
#define __PTHREAD_CONDATTR_SIZE__  4
#define __PTHREAD_COND_SIZE__      24
#define __PTHREAD_ONCE_SIZE__      4
/*
 * [Internal] data structure signatures
 */
#define _PTHREAD_MUTEX_SIG_init		0x32AAABA7
#define _PTHREAD_COND_SIG_init		0x3CB0B1BB
#define _PTHREAD_ONCE_SIG_init		0x30B1BCBA
/*
 * POSIX scheduling policies
 */

#define SCHED_OTHER      1
#define SCHED_FIFO       4
#define SCHED_RR         2
#define __SCHED_PARAM_SIZE__       4
