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
 * POSIX Pthread Library
 * -- Mutex variable support
 */

#include "pthread_internals.h"
#include <sys/timers.h>		/* For struct timespec and getclock(). */

/*
 * Destroy a mutex variable.
 */
int       
pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	kern_return_t kern_res;
	if (mutex->sig != _PTHREAD_MUTEX_SIG)
		return (EINVAL);
	if ((mutex->owner != (pthread_t)NULL) || 
	    (mutex->busy != (pthread_cond_t *)NULL))
		return (EBUSY);
	mutex->sig = _PTHREAD_NO_SIG;
	MACH_CALL(semaphore_destroy(mach_task_self(),
				    mutex->sem), kern_res);
	return (ESUCCESS);
}

/*
 * Initialize a mutex variable, possibly with additional attributes.
 */
int       
pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
	kern_return_t kern_res;
	LOCK_INIT(mutex->lock);
	mutex->sig = _PTHREAD_MUTEX_SIG;
	if (attr)
	{
		if (attr->sig != _PTHREAD_MUTEX_ATTR_SIG)
			return (EINVAL);
		mutex->prioceiling = attr->prioceiling;
		mutex->protocol = attr->protocol;
		mutex->type = attr->type;
		mutex->robust = attr->robust;
	} else
	{
		mutex->prioceiling = _PTHREAD_DEFAULT_PRIOCEILING;
		mutex->protocol = _PTHREAD_DEFAULT_PROTOCOL;
		mutex->type = PTHREAD_MUTEX_DEFAULT;
		mutex->robust = PTHREAD_MUTEX_STALLED;
	}
	mutex->lock_count = 0;
	mutex->owner_dead = 0;
	mutex->unrecoverable = 0;
	mutex->owner = (pthread_t)NULL;
	mutex->next = (pthread_mutex_t *)NULL;
	mutex->prev = (pthread_mutex_t *)NULL;
	mutex->busy = (pthread_cond_t *)NULL;
	mutex->waiters = 0;
	mutex->state = 0;
	MACH_CALL(semaphore_create(mach_task_self(),
				   &mutex->sem,
				   SYNC_POLICY_FIFO,
				   0), kern_res);
	if (kern_res != KERN_SUCCESS)
	{
		mutex->sig = _PTHREAD_NO_SIG;  /* Not a valid mutex variable */
		return (ENOMEM);
	} else
	{
		return (ESUCCESS);
	}
}

/*
 * Manage a list of mutex variables owned by a thread
 */

static void
_pthread_mutex_add(pthread_mutex_t *mutex)
{
	pthread_mutex_t *m;
	pthread_t self = pthread_self();
	mutex->owner = self;
	if ((m = self->mutexes) != (pthread_mutex_t *)NULL)
	{ /* Add to list */
		m->prev = mutex;
	}
	mutex->next = m;
	mutex->prev = (pthread_mutex_t *)NULL;
	self->mutexes = mutex;
}

static void
_pthread_mutex_remove(pthread_mutex_t *mutex)
{
	pthread_mutex_t *n, *p;
	pthread_t self = pthread_self();
	if ((n = mutex->next) != (pthread_mutex_t *)NULL)
	{
		n->prev = mutex->prev;
	}
	if ((p = mutex->prev) != (pthread_mutex_t *)NULL)
	{
		p->next = mutex->next;
	} else
	{ /* This is the first in the list */
		self->mutexes = n;
	}
}

/*
 * Mutex state machine (futex-like):
 *   0 = unlocked
 *   1 = locked, no waiters
 *   2 = locked, one or more waiters (contended)
 *
 * Uncontended lock:  CAS(0→1), no syscall
 * Contended lock:    CAS(→2), then semaphore_wait
 * Uncontended unlock: XCHG(→0), if old was 1 → no syscall
 * Contended unlock:   XCHG(→0), if old was 2 → semaphore_signal
 */

/*
 * Check if a robust mutex's owner thread has died.
 * Must be called with mutex->lock held.
 * Returns EOWNERDEAD if owner died (takes ownership), ENOTRECOVERABLE
 * if mutex was not recovered after prior death, ESUCCESS otherwise.
 */
static int
_pthread_mutex_robust_check(pthread_mutex_t *mutex)
{
	if (mutex->unrecoverable)
		return (ENOTRECOVERABLE);
	if (mutex->owner != (pthread_t)NULL &&
	    mutex->owner->sig != _PTHREAD_SIG)
	{
		/* Owner thread is dead — take ownership */
		mutex->owner_dead = 1;
		_pthread_mutex_remove(mutex);
		mutex->owner = (pthread_t)NULL;
		return (EOWNERDEAD);
	}
	return (ESUCCESS);
}

/*
 * Lock a mutex.
 */
int
pthread_mutex_lock(pthread_mutex_t *mutex)
{
	kern_return_t kern_res;
	int old;
	if (mutex->sig == _PTHREAD_MUTEX_SIG_init)
	{
		int res;
		if (res = pthread_mutex_init(mutex, NULL))
			return (res);
	}
	if (mutex->sig != _PTHREAD_MUTEX_SIG)
		return (EINVAL);

	/* Robust: fail immediately if unrecoverable */
	if (mutex->robust == PTHREAD_MUTEX_ROBUST && mutex->unrecoverable)
		return (ENOTRECOVERABLE);

	/* ERRORCHECK / RECURSIVE: check if already owned by this thread */
	if (mutex->type != PTHREAD_MUTEX_NORMAL)
	{
		LOCK(mutex->lock);
		if (mutex->owner == pthread_self())
		{
			if (mutex->type == PTHREAD_MUTEX_RECURSIVE)
			{
				mutex->lock_count++;
				UNLOCK(mutex->lock);
				return (ESUCCESS);
			}
			UNLOCK(mutex->lock);
			return (EDEADLK);  /* ERRORCHECK */
		}
		UNLOCK(mutex->lock);
	}

	/* Fast path: uncontended — CAS 0→1, zero syscalls */
	old = 0;
	if (__atomic_compare_exchange_n(&mutex->state, &old, 1,
					0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
	{
		LOCK(mutex->lock);
		_pthread_mutex_add(mutex);
		UNLOCK(mutex->lock);
		return (ESUCCESS);
	}

	/* Robust: check if owner died before blocking */
	if (mutex->robust == PTHREAD_MUTEX_ROBUST)
	{
		LOCK(mutex->lock);
		int rc = _pthread_mutex_robust_check(mutex);
		if (rc == EOWNERDEAD)
		{
			/* Force acquire — owner is dead, state is stale */
			__atomic_store_n(&mutex->state, 1, __ATOMIC_ACQUIRE);
			_pthread_mutex_add(mutex);
			UNLOCK(mutex->lock);
			return (EOWNERDEAD);
		}
		if (rc == ENOTRECOVERABLE)
		{
			UNLOCK(mutex->lock);
			return (ENOTRECOVERABLE);
		}
		UNLOCK(mutex->lock);
	}

	/* Slow path: contended — mark state=2 and block */
	if (old != 2)
		old = __atomic_exchange_n(&mutex->state, 2, __ATOMIC_ACQUIRE);
	while (old != 0)
	{
		MACH_CALL(semaphore_wait(mutex->sem), kern_res);
		old = __atomic_exchange_n(&mutex->state, 2, __ATOMIC_ACQUIRE);
	}
	LOCK(mutex->lock);
	_pthread_mutex_add(mutex);
	UNLOCK(mutex->lock);
	return (ESUCCESS);
}

/*
 * Attempt to lock a mutex, but don't block if this isn't possible.
 */
int
pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	int old;
	if (mutex->sig == _PTHREAD_MUTEX_SIG_init)
	{
		int res;
		if (res = pthread_mutex_init(mutex, NULL))
			return (res);
	}
	if (mutex->sig != _PTHREAD_MUTEX_SIG)
		return (EINVAL);

	/* RECURSIVE: if already owned, just bump count */
	if (mutex->type == PTHREAD_MUTEX_RECURSIVE)
	{
		LOCK(mutex->lock);
		if (mutex->owner == pthread_self())
		{
			mutex->lock_count++;
			UNLOCK(mutex->lock);
			return (ESUCCESS);
		}
		UNLOCK(mutex->lock);
	}

	old = 0;
	if (__atomic_compare_exchange_n(&mutex->state, &old, 1,
					0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
	{
		LOCK(mutex->lock);
		_pthread_mutex_add(mutex);
		UNLOCK(mutex->lock);
		return (ESUCCESS);
	}
	return (EBUSY);
}

/*
 * Lock a mutex with a timeout (POSIX.1-2008).
 * abstime is an absolute CLOCK_REALTIME deadline.
 */
int
pthread_mutex_timedlock(pthread_mutex_t *mutex,
			const struct timespec *abstime)
{
	kern_return_t kern_res;
	int old;
	if (mutex->sig == _PTHREAD_MUTEX_SIG_init)
	{
		int res;
		if (res = pthread_mutex_init(mutex, NULL))
			return (res);
	}
	if (mutex->sig != _PTHREAD_MUTEX_SIG)
		return (EINVAL);

	/* Robust: fail immediately if unrecoverable */
	if (mutex->robust == PTHREAD_MUTEX_ROBUST && mutex->unrecoverable)
		return (ENOTRECOVERABLE);

	/* ERRORCHECK / RECURSIVE */
	if (mutex->type != PTHREAD_MUTEX_NORMAL)
	{
		LOCK(mutex->lock);
		if (mutex->owner == pthread_self())
		{
			if (mutex->type == PTHREAD_MUTEX_RECURSIVE)
			{
				mutex->lock_count++;
				UNLOCK(mutex->lock);
				return (ESUCCESS);
			}
			UNLOCK(mutex->lock);
			return (EDEADLK);
		}
		UNLOCK(mutex->lock);
	}

	/* Fast path: uncontended */
	old = 0;
	if (__atomic_compare_exchange_n(&mutex->state, &old, 1,
					0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
	{
		LOCK(mutex->lock);
		_pthread_mutex_add(mutex);
		UNLOCK(mutex->lock);
		return (ESUCCESS);
	}

	/* Robust: check if owner died before blocking */
	if (mutex->robust == PTHREAD_MUTEX_ROBUST)
	{
		LOCK(mutex->lock);
		int rc = _pthread_mutex_robust_check(mutex);
		if (rc == EOWNERDEAD)
		{
			__atomic_store_n(&mutex->state, 1, __ATOMIC_ACQUIRE);
			_pthread_mutex_add(mutex);
			UNLOCK(mutex->lock);
			return (EOWNERDEAD);
		}
		if (rc == ENOTRECOVERABLE)
		{
			UNLOCK(mutex->lock);
			return (ENOTRECOVERABLE);
		}
		UNLOCK(mutex->lock);
	}

	/* Slow path with timeout */
	if (old != 2)
		old = __atomic_exchange_n(&mutex->state, 2, __ATOMIC_ACQUIRE);
	while (old != 0)
	{
		struct timespec now;
		tvalspec_t then;
		getclock(TIMEOFDAY, &now);
		then.tv_nsec = abstime->tv_nsec - now.tv_nsec;
		then.tv_sec = abstime->tv_sec - now.tv_sec;
		if (then.tv_nsec < 0)
		{
			then.tv_nsec += 1000000000;
			then.tv_sec--;
		}
		if (((int)then.tv_sec < 0) ||
		    ((then.tv_sec == 0) && (then.tv_nsec == 0)))
		{
			return (ETIMEDOUT);
		}
		MACH_CALL(semaphore_timedwait(mutex->sem, then), kern_res);
		if (kern_res == KERN_OPERATION_TIMED_OUT)
			return (ETIMEDOUT);
		old = __atomic_exchange_n(&mutex->state, 2, __ATOMIC_ACQUIRE);
	}
	LOCK(mutex->lock);
	_pthread_mutex_add(mutex);
	UNLOCK(mutex->lock);
	return (ESUCCESS);
}

/*
 * Unlock a mutex.
 */
int
pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	kern_return_t kern_res;
	int old;
	if (mutex->sig == _PTHREAD_MUTEX_SIG_init)
	{
		int res;
		if (res = pthread_mutex_init(mutex, NULL))
			return (res);
	}
	if (mutex->sig != _PTHREAD_MUTEX_SIG)
		return (EINVAL);

	LOCK(mutex->lock);
	if (mutex->owner != pthread_self())
	{
		UNLOCK(mutex->lock);
		return (EPERM);
	}
	/* RECURSIVE: decrement count; only truly unlock at zero */
	if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->lock_count > 0)
	{
		mutex->lock_count--;
		UNLOCK(mutex->lock);
		return (ESUCCESS);
	}
	/* Robust: if owner_dead and not made consistent, mark unrecoverable */
	if (mutex->robust == PTHREAD_MUTEX_ROBUST && mutex->owner_dead)
	{
		mutex->unrecoverable = 1;
		mutex->owner_dead = 0;
	}
	_pthread_mutex_remove(mutex);
	mutex->owner = (pthread_t)NULL;
	mutex->lock_count = 0;
	UNLOCK(mutex->lock);

	/* Fast path: if state was 1 (no waiters), just store 0 — no syscall */
	old = __atomic_exchange_n(&mutex->state, 0, __ATOMIC_RELEASE);
	if (old == 2)
	{
		/* There were waiters — wake one */
		MACH_CALL(semaphore_signal(mutex->sem), kern_res);
	}
	return (ESUCCESS);
}

/*
 * Fetch the priority ceiling value from a mutex variable.
 * Note: written as a 'helper' function to hide implementation details.
 */
int       
pthread_mutex_getprioceiling(const pthread_mutex_t *mutex, 
                             int *prioceiling)
{
	if (mutex->sig == _PTHREAD_MUTEX_SIG)
	{
		if (prioceiling != (int *)NULL)
			*prioceiling = mutex->prioceiling;
		return (ESUCCESS);
	} else
	{
		return (EINVAL); /* Not an initialized 'attribute' structure */
	}
}

/*
 * Set the priority ceiling for a mutex.
 * Note: written as a 'helper' function to hide implementation details.
 */
int       
pthread_mutex_setprioceiling(pthread_mutex_t *mutex, 
			     int prioceiling, 
			     int *old_prioceiling)
{
	if (mutex->sig == _PTHREAD_MUTEX_SIG)
	{
		if ((prioceiling >= -999) &&
		    (prioceiling <= 999))
		{
			if (old_prioceiling != (int *)NULL)
				*old_prioceiling = mutex->prioceiling;
			mutex->prioceiling = prioceiling;
			return (ESUCCESS);
		} else 
		{
			return (EINVAL); /* Invalid parameter */
		}
	} else
	{
		return (EINVAL); /* Not an initialized 'attribute' structure */
	}
}

/*
 * Destroy a mutex attribute structure.
 */
int       
pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
	attr->sig = _PTHREAD_NO_SIG;  /* Uninitialized */
	return (ESUCCESS);
}

/*
 * Get the priority ceiling value from a mutex attribute structure.
 * Note: written as a 'helper' function to hide implementation details.
 */
int       
pthread_mutexattr_getprioceiling(const pthread_mutexattr_t *attr, 
				 int *prioceiling)
{
	if (attr->sig == _PTHREAD_MUTEX_ATTR_SIG)
	{
		if (prioceiling != (int *)NULL)
			*prioceiling = attr->prioceiling;
		return (ESUCCESS);
	} else
	{
		return (EINVAL); /* Not an initialized 'attribute' structure */
	}
}

/*
 * Get the mutex 'protocol' value from a mutex attribute structure.
 * Note: written as a 'helper' function to hide implementation details.
 */
int       
pthread_mutexattr_getprotocol(const pthread_mutexattr_t *attr, 
			      int *protocol)
{
	if (attr->sig == _PTHREAD_MUTEX_ATTR_SIG)
	{
		if (protocol != (int *)NULL)
			*protocol = attr->protocol;
		return (ESUCCESS);
	} else
	{
		return (EINVAL); /* Not an initialized 'attribute' structure */
	}
}

/*
 * Initialize a mutex attribute structure to system defaults.
 */
int       
pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
	attr->prioceiling = _PTHREAD_DEFAULT_PRIOCEILING;
	attr->protocol = _PTHREAD_DEFAULT_PROTOCOL;
	attr->type = PTHREAD_MUTEX_DEFAULT;
	attr->robust = PTHREAD_MUTEX_STALLED;
	attr->sig = _PTHREAD_MUTEX_ATTR_SIG;
	return (ESUCCESS);
}

/*
 * Set the priority ceiling value in a mutex attribute structure.
 * Note: written as a 'helper' function to hide implementation details.
 */
int       
pthread_mutexattr_setprioceiling(pthread_mutexattr_t *attr, 
				 int prioceiling)
{
	if (attr->sig == _PTHREAD_MUTEX_ATTR_SIG)
	{
		if ((prioceiling >= -999) &&
		    (prioceiling <= 999))
		{
			attr->prioceiling = prioceiling;
			return (ESUCCESS);
		} else 
		{
			return (EINVAL); /* Invalid parameter */
		}
	} else
	{
		return (EINVAL); /* Not an initialized 'attribute' structure */
	}
}

/*
 * Set the mutex 'protocol' value in a mutex attribute structure.
 * Note: written as a 'helper' function to hide implementation details.
 */
int       
pthread_mutexattr_setprotocol(pthread_mutexattr_t *attr, 
			      int protocol)
{
	if (attr->sig == _PTHREAD_MUTEX_ATTR_SIG)
	{
		if ((protocol == PTHREAD_PRIO_NONE) || 
		    (protocol == PTHREAD_PRIO_INHERIT) ||
		    (protocol == PTHREAD_PRIO_PROTECT))
		{
			attr->protocol = protocol;
			return (ESUCCESS);
		} else
		{
			return (EINVAL); /* Invalid parameter */
		}
	} else
	{
		return (EINVAL); /* Not an initialized 'attribute' structure */
	}
}

/*
 * Get the mutex 'type' value from a mutex attribute structure.
 */
int
pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type)
{
	if (attr->sig == _PTHREAD_MUTEX_ATTR_SIG)
	{
		if (type != (int *)NULL)
			*type = attr->type;
		return (ESUCCESS);
	} else
	{
		return (EINVAL);
	}
}

/*
 * Set the mutex 'type' value in a mutex attribute structure.
 */
int
pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{
	if (attr->sig == _PTHREAD_MUTEX_ATTR_SIG)
	{
		if ((type == PTHREAD_MUTEX_NORMAL) ||
		    (type == PTHREAD_MUTEX_ERRORCHECK) ||
		    (type == PTHREAD_MUTEX_RECURSIVE))
		{
			attr->type = type;
			return (ESUCCESS);
		} else
		{
			return (EINVAL);
		}
	} else
	{
		return (EINVAL);
	}
}

/*
 * Get the robustness value from a mutex attribute structure.
 */
int
pthread_mutexattr_getrobust(const pthread_mutexattr_t *attr, int *robust)
{
	if (attr->sig == _PTHREAD_MUTEX_ATTR_SIG)
	{
		if (robust != (int *)NULL)
			*robust = attr->robust;
		return (ESUCCESS);
	} else
	{
		return (EINVAL);
	}
}

/*
 * Set the robustness value in a mutex attribute structure.
 */
int
pthread_mutexattr_setrobust(pthread_mutexattr_t *attr, int robust)
{
	if (attr->sig == _PTHREAD_MUTEX_ATTR_SIG)
	{
		if ((robust == PTHREAD_MUTEX_STALLED) ||
		    (robust == PTHREAD_MUTEX_ROBUST))
		{
			attr->robust = robust;
			return (ESUCCESS);
		} else
		{
			return (EINVAL);
		}
	} else
	{
		return (EINVAL);
	}
}

/*
 * Mark a robust mutex as consistent after EOWNERDEAD recovery.
 * Must be called by the thread that received EOWNERDEAD before unlocking.
 */
int
pthread_mutex_consistent(pthread_mutex_t *mutex)
{
	if (mutex->sig != _PTHREAD_MUTEX_SIG)
		return (EINVAL);
	if (mutex->robust != PTHREAD_MUTEX_ROBUST)
		return (EINVAL);

	LOCK(mutex->lock);
	if (mutex->owner != pthread_self() || !mutex->owner_dead)
	{
		UNLOCK(mutex->lock);
		return (EINVAL);
	}
	mutex->owner_dead = 0;
	UNLOCK(mutex->lock);
	return (ESUCCESS);
}
