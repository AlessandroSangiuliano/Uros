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
 */
/*
 * MkLinux
 */

/*
 * POSIX Pthread Library
 */

#include "pthread_internals.h"
#include <sys/timers.h>              /* For struct timespec and getclock(). */

/*
 * #324: the condvar's kernel wakeup is now urmach_futex on a generation
 * (sequence) counter instead of a Mach semaphore.  We repurpose the
 * 32-bit cond->sem slot (no longer a port) as that counter — COND_SEQ().
 * A waiter snapshots the seq under cond->lock before blocking; signal /
 * broadcast bump the seq (under cond->lock) then wake, so a signal racing
 * a not-yet-blocked waiter is never lost (FUTEX_WAIT on a changed seq
 * returns at once).  Spurious wakeups are allowed by the condvar contract.
 */
#define COND_SEQ(c)	((volatile int *)&(c)->sem)

/*
 * Thread-safe lazy initialization for PTHREAD_COND_INITIALIZER.
 * Uses CAS to ensure exactly one thread initializes the seq counter;
 * losers spin until the winner publishes _PTHREAD_COND_SIG.
 */
static int
_pthread_cond_lazy_init(pthread_cond_t *cond)
{
	/*
	 * 🔴 `long', because `sig' is a long -- and this was an `int' until
	 * #425, which is the whole of that defect.
	 *
	 * __atomic_compare_exchange_n takes the expected value BY ADDRESS and
	 * operates at the width of the object, so on a 64-bit target it reads
	 * eight bytes from a four-byte stack slot and, when the compare fails,
	 * writes eight bytes back into it.  The compare therefore fails for
	 * every thread -- the upper four bytes are whatever the stack held --
	 * every thread takes the losing branch, and every one of them spins
	 * for a magic word nobody is left to publish.  pthread_test stopped
	 * there, on the second of twenty-three arms.
	 *
	 * ⚠️ i386 never saw it: `long' is four bytes there, so the two widths
	 * agree by accident of the target rather than by anything written.
	 *
	 * 🔑 gcc names it exactly -- "__atomic_compare_exchange_8 writing 8
	 * bytes into a region of size 4 overflows the destination" -- and this
	 * library is compiled -w, so it was thrown away.  pthread_once() has
	 * the same lazy-init written correctly in another file, which is what
	 * made the intent unambiguous.
	 */
	long expected = _PTHREAD_COND_SIG_init;
	if (__atomic_compare_exchange_n(&cond->sig, &expected,
					_PTHREAD_NO_SIG, 0,
					__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
	{
		LOCK_INIT(cond->lock);
		cond->next = (pthread_cond_t *)NULL;
		cond->prev = (pthread_cond_t *)NULL;
		cond->busy = (pthread_mutex_t *)NULL;
		cond->waiters = 0;
		cond->clock = CLOCK_REALTIME;
		*COND_SEQ(cond) = 0;		/* #324: futex seq, not a port */
		__atomic_store_n(&cond->sig, _PTHREAD_COND_SIG,
				 __ATOMIC_RELEASE);
	} else
	{
		while (__atomic_load_n(&cond->sig, __ATOMIC_ACQUIRE)
		       != _PTHREAD_COND_SIG)
			;
	}
	return (ESUCCESS);
}

/*
 * Destroy a condition variable.
 */
int
pthread_cond_destroy(pthread_cond_t *cond)
{
	if (cond->sig == _PTHREAD_COND_SIG)
	{
		LOCK(cond->lock);
		if (cond->busy != (pthread_mutex_t *)NULL)
		{
			UNLOCK(cond->lock);
			return (EBUSY);
		} else
		{
			cond->sig = _PTHREAD_NO_SIG;
			/* #324: no kernel object — the seq lives in user mem. */
			UNLOCK(cond->lock);
			return (ESUCCESS);
		}
	} else
		return (EINVAL); /* Not an initialized condition variable structure */
}

/*
 * Initialize a condition variable.  Note: 'attr' is ignored.
 */
int
pthread_cond_init(pthread_cond_t *cond,
		  const pthread_condattr_t *attr)
{
	LOCK_INIT(cond->lock);
	cond->sig = _PTHREAD_COND_SIG;
	cond->next = (pthread_cond_t *)NULL;
	cond->prev = (pthread_cond_t *)NULL;
	cond->busy = (pthread_mutex_t *)NULL;
	cond->waiters = 0;
	cond->clock = (attr && attr->sig == _PTHREAD_COND_ATTR_SIG)
		      ? attr->clock : CLOCK_REALTIME;
	*COND_SEQ(cond) = 0;		/* #324: futex seq counter, not a port */
	return (ESUCCESS);
}

/*
 * Signal a condition variable, waking up all threads waiting for it.
 */
int
pthread_cond_broadcast(pthread_cond_t *cond)
{
	if (cond->sig == _PTHREAD_COND_SIG_init)
	{
		int res;
		if (res = _pthread_cond_lazy_init(cond))
			return (res);
	}
	if (cond->sig == _PTHREAD_COND_SIG)
	{
		LOCK(cond->lock);
		if (cond->waiters == 0)
		{ /* Avoid kernel call since there are no waiters... */
			UNLOCK(cond->lock);
			return (ESUCCESS);
		}
		(*COND_SEQ(cond))++;	/* new generation; wakes every waiter */
		UNLOCK(cond->lock);
		_pthread_futex_wake_all(COND_SEQ(cond));
		return (ESUCCESS);
	} else
		return (EINVAL); /* Not a condition variable */
}

/*
 * Signal a condition variable, waking only one thread.
 */
int
pthread_cond_signal(pthread_cond_t *cond)
{
	if (cond->sig == _PTHREAD_COND_SIG_init)
	{
		int res;
		if (res = _pthread_cond_lazy_init(cond))
			return (res);
	}
	if (cond->sig == _PTHREAD_COND_SIG)
	{
		LOCK(cond->lock);
		if (cond->waiters == 0)
		{ /* Avoid kernel call since there are no waiters... */
			UNLOCK(cond->lock);
			return (ESUCCESS);
		}
		(*COND_SEQ(cond))++;	/* new generation; wakes one waiter */
		UNLOCK(cond->lock);
		_pthread_futex_wake_one(COND_SEQ(cond));
		return (ESUCCESS);
	} else
		return (EINVAL); /* Not a condition variable */
}
/*
 * Manage a list of condition variables associated with a mutex
 */

static void
_pthread_cond_add(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	pthread_cond_t *c;
	LOCK(mutex->lock);
	if ((c = mutex->busy) != (pthread_cond_t *)NULL)
	{
		c->prev = cond;
	} 
	cond->next = c;
	cond->prev = (pthread_cond_t *)NULL;
	mutex->busy = cond;
	UNLOCK(mutex->lock);
}

static void
_pthread_cond_remove(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	pthread_cond_t *n, *p;
	LOCK(mutex->lock);
	if ((n = cond->next) != (pthread_cond_t *)NULL)
	{
		n->prev = cond->prev;
	}
	if ((p = cond->prev) != (pthread_cond_t *)NULL)
	{
		p->next = cond->next;
	} else
	{ /* This is the first in the list */
		mutex->busy = n;
	}
	UNLOCK(mutex->lock);
}

/*
 * Suspend waiting for a condition variable.
 * Note: we have to keep a list of condition variables which are using
 * this same mutex variable so we can detect invalid 'destroy' sequences.
 */
static int       
_pthread_cond_wait(pthread_cond_t *cond, 
		   pthread_mutex_t *mutex,
		   const struct timespec *abstime)
{
	int res;
	kern_return_t kern_res;
	pthread_mutex_t *busy;
	tvalspec_t then;
	int seq;
	if (cond->sig == _PTHREAD_COND_SIG_init)
	{
		if (res = _pthread_cond_lazy_init(cond))
			return (res);
	}
	if (cond->sig != _PTHREAD_COND_SIG)
		return (EINVAL); /* Not a condition variable */
	LOCK(cond->lock);
	busy = cond->busy;
	if ((busy != (pthread_mutex_t *)NULL) && (busy != mutex))
	{ /* Must always specify the same mutex! */
		UNLOCK(cond->lock);
		return (EINVAL);
	}
	cond->waiters++;
	/* Snapshot the generation under cond->lock: a signal/broadcast that
	 * runs after this (also under cond->lock) bumps it, so our futex wait
	 * below will not block on a stale value -> no lost wakeup. */
	seq = *COND_SEQ(cond);
	if (cond->waiters == 1)
	{
		_pthread_cond_add(cond, mutex);
		cond->busy = mutex;
	}
	if ((res = pthread_mutex_unlock(mutex)) != ESUCCESS)
	{
		cond->waiters--;
		if (cond->waiters == 0)
		{
			_pthread_cond_remove(cond, mutex);
			cond->busy = (pthread_mutex_t *)NULL;
		}
		UNLOCK(cond->lock);
		return (res);
	}	
	UNLOCK(cond->lock);
	if (abstime)
	{
		struct timespec now;
		getclock(TIMEOFDAY, &now);
		/* Compute relative time to sleep */
		then.tv_nsec = abstime->tv_nsec - now.tv_nsec;
	        then.tv_sec = abstime->tv_sec - now.tv_sec;
		if (then.tv_nsec < 0)
		{
			then.tv_nsec += 1000000000;  /* nsec/sec */
			then.tv_sec--;
		}
		if (((int)then.tv_sec < 0) ||
		    ((then.tv_sec == 0) && (then.tv_nsec == 0)))
		{
			kern_res = KERN_OPERATION_TIMED_OUT;
		} else
		{
			/* Relative ms; 0 means "forever" to urmach_futex, so
			 * round a sub-ms remainder up to 1. */
			unsigned int tmo_ms = (unsigned int)then.tv_sec * 1000u
					    + (unsigned int)(then.tv_nsec / 1000000);
			if (tmo_ms == 0)
				tmo_ms = 1;
			kern_res = _pthread_futex_wait(COND_SEQ(cond), seq, tmo_ms);
		}
	} else
	{
		kern_res = _pthread_futex_wait(COND_SEQ(cond), seq, 0);
	}
	LOCK(cond->lock);
	cond->waiters--;
	if (cond->waiters == 0)
	{
		_pthread_cond_remove(cond, mutex);
		cond->busy = (pthread_mutex_t *)NULL;
	}
	UNLOCK(cond->lock);
	if ((res = pthread_mutex_lock(mutex)) != ESUCCESS)
	{
		return (res);
	}
	/* Only a real timeout is an error; a normal wake, a generation that
	 * already advanced (KERN_NOT_WAITING), or any spurious return all map
	 * to ESUCCESS — pthread_cond_wait callers re-test the predicate. */
	if (kern_res == KERN_OPERATION_TIMED_OUT)
		return (ETIMEDOUT);
	return (ESUCCESS);
}

int       
pthread_cond_wait(pthread_cond_t *cond, 
		  pthread_mutex_t *mutex)
{
	return (_pthread_cond_wait(cond, mutex, (struct timespec *)NULL));
}

int       
pthread_cond_timedwait(pthread_cond_t *cond, 
		       pthread_mutex_t *mutex,
		       const struct timespec *abstime)
{
	return (_pthread_cond_wait(cond, mutex, abstime));
}

/*
 * Condition variable attribute functions.
 */

int
pthread_condattr_init(pthread_condattr_t *attr)
{
	attr->sig = _PTHREAD_COND_ATTR_SIG;
	attr->pshared = PTHREAD_PROCESS_PRIVATE;
	attr->clock = CLOCK_REALTIME;
	return (ESUCCESS);
}

int
pthread_condattr_destroy(pthread_condattr_t *attr)
{
	attr->sig = _PTHREAD_NO_SIG;
	return (ESUCCESS);
}

int
pthread_condattr_getpshared(const pthread_condattr_t *attr, int *pshared)
{
	if (attr->sig != _PTHREAD_COND_ATTR_SIG)
		return (EINVAL);
	if (pshared != (int *)NULL)
		*pshared = attr->pshared;
	return (ESUCCESS);
}

int
pthread_condattr_setpshared(pthread_condattr_t *attr, int pshared)
{
	if (attr->sig != _PTHREAD_COND_ATTR_SIG)
		return (EINVAL);
	if (pshared == PTHREAD_PROCESS_PRIVATE)
	{
		attr->pshared = pshared;
		return (ESUCCESS);
	}
	if (pshared == PTHREAD_PROCESS_SHARED)
		return (ENOTSUP);
	return (EINVAL);
}

int
pthread_condattr_getclock(const pthread_condattr_t *attr, int *clock_id)
{
	if (attr->sig != _PTHREAD_COND_ATTR_SIG)
		return (EINVAL);
	if (clock_id != (int *)NULL)
		*clock_id = attr->clock;
	return (ESUCCESS);
}

int
pthread_condattr_setclock(pthread_condattr_t *attr, int clock_id)
{
	if (attr->sig != _PTHREAD_COND_ATTR_SIG)
		return (EINVAL);
	if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC)
		return (EINVAL);
	attr->clock = clock_id;
	return (ESUCCESS);
}
