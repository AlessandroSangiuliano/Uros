/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 * 
 */
/*
 * MkLinux
 */

/*
 * POSIX Threads - IEEE 1003.1c
 */

#ifndef _POSIX_PTHREAD_H
#define _POSIX_PTHREAD_H

#ifndef __POSIX_LIB__
#include <machine/pthread_impl.h>
#endif
#include <errno.h>

/*
 * These symbols indicate which [optional] features are available
 * They can be tested at compile time via '#ifdef XXX'
 */

#define _POSIX_THREADS
#undef  _POSIX_THREAD_ATTR_STACKADDR
#define _POSIX_THREAD_ATTR_STACKSIZE
#define _POSIX_THREAD_PRIORITY_SCHEDULING
#define _POSIX_THREAD_PRIO_INHERIT
#define _POSIX_THREAD_PRIO_PROTECT
#undef  _POSIX_THREAD_PROCESS_SHARED
#undef  _POSIX_THREAD_SAFE_FUNCTIONS

/*
 * Note: These data structures are meant to be opaque.  Only enough
 * structure is exposed to support initializers.
 */

/*
 * Scheduling paramters
 */
#ifndef __POSIX_LIB__
struct sched_param { int sched_priority;  char opaque[__SCHED_PARAM_SIZE__]; };
#endif

/*
 * Threads
 */
#ifndef __POSIX_LIB__
typedef struct { long sig; char opaque[__PTHREAD_SIZE__];} *pthread_t;
#endif

struct _pthread_handler_rec
{
	void           *(*routine)(void *);  /* Routine to call */
	void           *arg;                 /* Argument to pass */
	struct _pthread_handler_rec *next;
};

/*
 * Cancel cleanup handler management.  Note, since these are implemented as macros,
 * they *MUST* occur in matched pairs!
 */

#define pthread_cleanup_push(func, val) \
   { \
	     struct _pthread_handler_rec __handler; \
	     pthread_t __self = pthread_self(); \
	     __handler.routine = (func); \
	     __handler.arg = (val); \
	     __handler.next = __self->cleanup_stack; \
	     __self->cleanup_stack = &__handler;

#define pthread_cleanup_pop(execute) \
	     /* Note: '__handler' must be in this same lexical context! */ \
	     __self->cleanup_stack = __handler.next; \
	     if (execute) (__handler.routine)(__handler.arg); \
   }
	
/*
 * Thread attributes
 */
#ifndef __POSIX_LIB__
typedef struct { long sig; char opaque[__PTHREAD_ATTR_SIZE__]; } pthread_attr_t;
#endif

#define PTHREAD_CREATE_JOINABLE      1
#define PTHREAD_CREATE_DETACHED      2

#define PTHREAD_INHERIT_SCHED        1
#define PTHREAD_EXPLICIT_SCHED       2

#define PTHREAD_CANCEL_ENABLE        0x01  /* Cancel takes place at next cancellation point */
#define PTHREAD_CANCEL_DISABLE       0x00  /* Cancel postponed */
#define PTHREAD_CANCEL_DEFERRED      0x02  /* Cancel waits until cancellation point */
#define PTHREAD_CANCEL_ASYNCHRONOUS  0x00  /* Cancel occurs immediately */

/*
 * Mutex attributes
 */
#ifndef __POSIX_LIB__
typedef struct { long sig; char opaque[__PTHREAD_MUTEXATTR_SIZE__]; } pthread_mutexattr_t;
#endif

#define PTHREAD_PRIO_NONE            0
#define PTHREAD_PRIO_INHERIT         1
#define PTHREAD_PRIO_PROTECT         2

/*
 * Mutex variables
 */
#ifndef __POSIX_LIB__
typedef struct { long sig; char opaque[__PTHREAD_MUTEX_SIZE__]; } pthread_mutex_t;
#endif

#define PTHREAD_MUTEX_INITIALIZER {_PTHREAD_MUTEX_SIG_init}

/*
 * Mutex types (POSIX.1-2001)
 */
#define PTHREAD_MUTEX_NORMAL		0
#define PTHREAD_MUTEX_ERRORCHECK	1
#define PTHREAD_MUTEX_RECURSIVE		2
#define PTHREAD_MUTEX_DEFAULT		PTHREAD_MUTEX_NORMAL

/*
 * Mutex robustness (POSIX.1-2008)
 */
#define PTHREAD_MUTEX_STALLED		0
#define PTHREAD_MUTEX_ROBUST		1

/*
 * Process-shared attribute (POSIX.1-2001)
 */
#define PTHREAD_PROCESS_PRIVATE		0
#define PTHREAD_PROCESS_SHARED		1

/*
 * Condition variable attributes
 */
#ifndef __POSIX_LIB__
typedef struct { long sig; char opaque[__PTHREAD_CONDATTR_SIZE__]; } pthread_condattr_t;
#endif

/*
 * Condition variables
 */
#ifndef __POSIX_LIB__
typedef struct { long sig;  char opaque[__PTHREAD_COND_SIZE__]; } pthread_cond_t;
#endif

#define PTHREAD_COND_INITIALIZER {_PTHREAD_COND_SIG_init}

/*
 * Initialization control (once) variables
 */
#ifndef __POSIX_LIB__
typedef struct { long sig; char opaque[__PTHREAD_ONCE_SIZE__]; } pthread_once_t;
#endif

#define PTHREAD_ONCE_INIT {_PTHREAD_ONCE_SIG_init}

/*
 * Read-write lock attributes
 */
#ifndef __POSIX_LIB__
typedef struct { long sig; char opaque[__PTHREAD_RWLOCKATTR_SIZE__]; } pthread_rwlockattr_t;
#endif

/*
 * Read-write lock variables
 */
#ifndef __POSIX_LIB__
typedef struct { long sig; char opaque[__PTHREAD_RWLOCK_SIZE__]; } pthread_rwlock_t;
#endif

#define PTHREAD_RWLOCK_INITIALIZER {_PTHREAD_RWLOCK_SIG}

/*
 * Barrier attributes
 */
#ifndef __POSIX_LIB__
typedef struct { long sig; char opaque[__PTHREAD_BARRIERATTR_SIZE__]; } pthread_barrierattr_t;
#endif

/*
 * Barrier variables
 */
#ifndef __POSIX_LIB__
typedef struct { long sig; char opaque[__PTHREAD_BARRIER_SIZE__]; } pthread_barrier_t;
#endif

#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

/*
 * Spinlock variables
 */
#ifndef __POSIX_LIB__
typedef struct { long sig; char opaque[__PTHREAD_SPIN_SIZE__]; } pthread_spinlock_t;
#endif

/*
 * Thread Specific Data - keys
 */
typedef unsigned long pthread_key_t;    /* Opaque 'pointer' */

#include <stddef.h>
#include <sys/timers.h>

/*
 * Prototypes for all PTHREAD interfaces
 */
int       pthread_attr_destroy(pthread_attr_t *attr);
int       pthread_attr_getdetachstate(const pthread_attr_t *attr,
				      int *detachstate);
int       pthread_attr_getinheritsched(const pthread_attr_t *attr, 
				       int *inheritsched);
int       pthread_attr_getschedparam(const pthread_attr_t *attr, 
                                     struct sched_param *param);
int       pthread_attr_getschedpolicy(const pthread_attr_t *attr, 
				      int *policy);
int       pthread_attr_init(pthread_attr_t *attr);
int       pthread_attr_setdetachstate(pthread_attr_t *attr, 
				      int detachstate);
int       pthread_attr_setinheritsched(pthread_attr_t *attr, 
				       int inheritsched);
int       pthread_attr_setschedparam(pthread_attr_t *attr, 
                                     const struct sched_param *param);
int       pthread_attr_setschedpolicy(pthread_attr_t *attr,
				      int policy);
int       pthread_attr_setstacksize(pthread_attr_t *attr,
				     size_t stacksize);
int       pthread_attr_getstacksize(const pthread_attr_t *attr,
				     size_t *stacksize);
int       pthread_cancel(pthread_t thread);
int       pthread_setcancelstate(int state, int *oldstate);
int       pthread_setcanceltype(int type, int *oldtype);
void      pthread_testcancel(void);
int       pthread_cond_broadcast(pthread_cond_t *cond);
int       pthread_cond_destroy(pthread_cond_t *cond);
int       pthread_cond_init(pthread_cond_t *cond,
                            const pthread_condattr_t *attr);
int       pthread_cond_signal(pthread_cond_t *cond);
int       pthread_cond_wait(pthread_cond_t *cond, 
			    pthread_mutex_t *mutex);
int       pthread_cond_timedwait(pthread_cond_t *cond, 
				 pthread_mutex_t *mutex,
				 const struct timespec *abstime);
int       pthread_create(pthread_t *thread, 
                         const pthread_attr_t *attr,
                         void *(*start_routine)(void *), 
                         void *arg);
int       pthread_detach(pthread_t thread);
int       pthread_equal(pthread_t t1, 
			pthread_t t2);
void      pthread_exit(void *value_ptr);
int       pthread_getschedparam(pthread_t thread, 
				int *policy,
                                struct sched_param *param);
int       pthread_join(pthread_t thread, 
		       void **value_ptr);
int       pthread_mutex_destroy(pthread_mutex_t *mutex);
int       pthread_mutex_getprioceiling(const pthread_mutex_t *mutex, 
                                       int *prioceiling);
int       pthread_mutex_init(pthread_mutex_t *mutex, 
			     const pthread_mutexattr_t *attr);
int       pthread_mutex_lock(pthread_mutex_t *mutex);
int       pthread_mutex_setprioceiling(pthread_mutex_t *mutex, 
                                       int prioceiling, 
                                       int *old_prioceiling);
int       pthread_mutex_trylock(pthread_mutex_t *mutex);
int       pthread_mutex_timedlock(pthread_mutex_t *mutex,
                                  const struct timespec *abstime);
int       pthread_mutex_unlock(pthread_mutex_t *mutex);
int       pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int       pthread_mutexattr_getprioceiling(const pthread_mutexattr_t *attr, 
                                           int *prioceiling);
int       pthread_mutexattr_getprotocol(const pthread_mutexattr_t *attr, 
                                        int *protocol);
int       pthread_mutexattr_init(pthread_mutexattr_t *attr);
int       pthread_mutexattr_setprioceiling(pthread_mutexattr_t *attr, 
                                           int prioceiling);
int       pthread_mutexattr_setprotocol(pthread_mutexattr_t *attr,
                                        int protocol);
int       pthread_mutexattr_gettype(const pthread_mutexattr_t *attr,
                                    int *type);
int       pthread_mutexattr_settype(pthread_mutexattr_t *attr,
                                    int type);
int       pthread_mutexattr_getrobust(const pthread_mutexattr_t *attr,
                                      int *robust);
int       pthread_mutexattr_setrobust(pthread_mutexattr_t *attr,
                                      int robust);
int       pthread_mutex_consistent(pthread_mutex_t *mutex);
int       pthread_mutexattr_getpshared(const pthread_mutexattr_t *attr,
                                       int *pshared);
int       pthread_mutexattr_setpshared(pthread_mutexattr_t *attr,
                                       int pshared);
int       pthread_condattr_getpshared(const pthread_condattr_t *attr,
                                      int *pshared);
int       pthread_condattr_setpshared(pthread_condattr_t *attr,
                                      int pshared);
int       pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *attr,
                                        int *pshared);
int       pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attr,
                                        int pshared);
int       pthread_barrierattr_getpshared(const pthread_barrierattr_t *attr,
                                         int *pshared);
int       pthread_barrierattr_setpshared(pthread_barrierattr_t *attr,
                                         int pshared);
int       pthread_once(pthread_once_t *once_control, 
		       void (*init_routine)(void));
pthread_t pthread_self(void);
int       pthread_setname_np(pthread_t thread, const char *name);
int       pthread_getname_np(pthread_t thread, char *buf, int len);
int       pthread_setschedparam(pthread_t thread, 
				int policy,
                                const struct sched_param *param);
int       pthread_key_create(pthread_key_t *key,
			     void (*destructor)(void *));
int       pthread_key_delete(pthread_key_t key);
int       pthread_setspecific(pthread_key_t key,
			      const void *value);
void     *pthread_getspecific(pthread_key_t key);

/* Read-write locks */
int       pthread_rwlock_init(pthread_rwlock_t *rwlock,
			       const pthread_rwlockattr_t *attr);
int       pthread_rwlock_destroy(pthread_rwlock_t *rwlock);
int       pthread_rwlock_rdlock(pthread_rwlock_t *rwlock);
int       pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock);
int       pthread_rwlock_timedrdlock(pthread_rwlock_t *rwlock,
                                     const struct timespec *abstime);
int       pthread_rwlock_wrlock(pthread_rwlock_t *rwlock);
int       pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock);
int       pthread_rwlock_timedwrlock(pthread_rwlock_t *rwlock,
                                     const struct timespec *abstime);
int       pthread_rwlock_unlock(pthread_rwlock_t *rwlock);
int       pthread_rwlockattr_init(pthread_rwlockattr_t *attr);
int       pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr);

/* Barriers */
int       pthread_barrier_init(pthread_barrier_t *barrier,
				const pthread_barrierattr_t *attr,
				unsigned count);
int       pthread_barrier_destroy(pthread_barrier_t *barrier);
int       pthread_barrier_wait(pthread_barrier_t *barrier);
int       pthread_barrierattr_init(pthread_barrierattr_t *attr);
int       pthread_barrierattr_destroy(pthread_barrierattr_t *attr);

/* Spinlocks */
int       pthread_spin_init(pthread_spinlock_t *lock, int pshared);
int       pthread_spin_destroy(pthread_spinlock_t *lock);
int       pthread_spin_lock(pthread_spinlock_t *lock);
int       pthread_spin_trylock(pthread_spinlock_t *lock);
int       pthread_spin_unlock(pthread_spinlock_t *lock);


#endif /* _POSIX_PTHREAD_H */
