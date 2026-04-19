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
 * POSIX Pthread Library
 *   Read-write lock support (POSIX.1-2001)
 *
 * Implements multiple-reader / exclusive-writer semantics using
 * Mach kernel semaphores for blocking.  Writer-preference: when a
 * writer is waiting, new readers block to prevent writer starvation.
 */

#include "pthread_internals.h"
#include <sys/timers.h>		/* For struct timespec and getclock(). */

int
pthread_rwlockattr_init(pthread_rwlockattr_t *attr)
{
	attr->sig = _PTHREAD_RWLOCK_ATTR_SIG;
	attr->pshared = 0;
	return (ESUCCESS);
}

int
pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr)
{
	attr->sig = _PTHREAD_NO_SIG;
	return (ESUCCESS);
}

int
pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *attr, int *pshared)
{
	if (attr->sig != _PTHREAD_RWLOCK_ATTR_SIG)
		return (EINVAL);
	if (pshared != (int *)NULL)
		*pshared = attr->pshared;
	return (ESUCCESS);
}

int
pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attr, int pshared)
{
	if (attr->sig != _PTHREAD_RWLOCK_ATTR_SIG)
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
pthread_rwlock_init(pthread_rwlock_t *rwlock,
		    const pthread_rwlockattr_t *attr)
{
	kern_return_t kr;

	LOCK_INIT(rwlock->lock);
	rwlock->sig = _PTHREAD_RWLOCK_SIG;
	rwlock->readers = 0;
	rwlock->writer = 0;
	rwlock->blocked_writers = 0;

	MACH_CALL(semaphore_create(mach_task_self(),
				   &rwlock->reader_sem,
				   SYNC_POLICY_FIFO, 0), kr);
	if (kr != KERN_SUCCESS)
		return (ENOMEM);

	MACH_CALL(semaphore_create(mach_task_self(),
				   &rwlock->writer_sem,
				   SYNC_POLICY_FIFO, 0), kr);
	if (kr != KERN_SUCCESS) {
		semaphore_destroy(mach_task_self(), rwlock->reader_sem);
		return (ENOMEM);
	}

	return (ESUCCESS);
}

int
pthread_rwlock_destroy(pthread_rwlock_t *rwlock)
{
	kern_return_t kr;

	if (rwlock->sig != _PTHREAD_RWLOCK_SIG)
		return (EINVAL);

	LOCK(rwlock->lock);
	if (rwlock->readers != 0 || rwlock->writer != 0) {
		UNLOCK(rwlock->lock);
		return (EBUSY);
	}
	rwlock->sig = _PTHREAD_NO_SIG;
	UNLOCK(rwlock->lock);

	MACH_CALL(semaphore_destroy(mach_task_self(),
				    rwlock->reader_sem), kr);
	MACH_CALL(semaphore_destroy(mach_task_self(),
				    rwlock->writer_sem), kr);
	return (ESUCCESS);
}

int
pthread_rwlock_rdlock(pthread_rwlock_t *rwlock)
{
	kern_return_t kr;

	if (rwlock->sig != _PTHREAD_RWLOCK_SIG)
		return (EINVAL);

	LOCK(rwlock->lock);
	/*
	 * Block while a writer holds the lock or writers are waiting
	 * (writer-preference to prevent starvation).
	 */
	while (rwlock->writer || rwlock->blocked_writers > 0) {
		UNLOCK(rwlock->lock);
		MACH_CALL(semaphore_wait(rwlock->reader_sem), kr);
		LOCK(rwlock->lock);
	}
	rwlock->readers++;
	UNLOCK(rwlock->lock);
	return (ESUCCESS);
}

int
pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock)
{
	if (rwlock->sig != _PTHREAD_RWLOCK_SIG)
		return (EINVAL);

	LOCK(rwlock->lock);
	if (rwlock->writer || rwlock->blocked_writers > 0) {
		UNLOCK(rwlock->lock);
		return (EBUSY);
	}
	rwlock->readers++;
	UNLOCK(rwlock->lock);
	return (ESUCCESS);
}

/*
 * Compute relative deadline from absolute timespec.
 * Returns 0 if deadline already passed, 1 if 'then' is filled.
 */
static int
_pthread_rwlock_deadline(const struct timespec *abstime, tvalspec_t *then)
{
	struct timespec now;
	getclock(TIMEOFDAY, &now);
	then->tv_nsec = abstime->tv_nsec - now.tv_nsec;
	then->tv_sec = abstime->tv_sec - now.tv_sec;
	if (then->tv_nsec < 0)
	{
		then->tv_nsec += 1000000000;
		then->tv_sec--;
	}
	if (((int)then->tv_sec < 0) ||
	    ((then->tv_sec == 0) && (then->tv_nsec == 0)))
		return (0);
	return (1);
}

int
pthread_rwlock_timedrdlock(pthread_rwlock_t *rwlock,
			   const struct timespec *abstime)
{
	kern_return_t kr;
	tvalspec_t then;

	if (rwlock->sig != _PTHREAD_RWLOCK_SIG)
		return (EINVAL);

	LOCK(rwlock->lock);
	while (rwlock->writer || rwlock->blocked_writers > 0) {
		UNLOCK(rwlock->lock);
		if (!_pthread_rwlock_deadline(abstime, &then))
			return (ETIMEDOUT);
		MACH_CALL(semaphore_timedwait(rwlock->reader_sem, then), kr);
		if (kr == KERN_OPERATION_TIMED_OUT)
			return (ETIMEDOUT);
		LOCK(rwlock->lock);
	}
	rwlock->readers++;
	UNLOCK(rwlock->lock);
	return (ESUCCESS);
}

int
pthread_rwlock_timedwrlock(pthread_rwlock_t *rwlock,
			   const struct timespec *abstime)
{
	kern_return_t kr;
	tvalspec_t then;

	if (rwlock->sig != _PTHREAD_RWLOCK_SIG)
		return (EINVAL);

	LOCK(rwlock->lock);
	rwlock->blocked_writers++;
	while (rwlock->writer || rwlock->readers > 0) {
		UNLOCK(rwlock->lock);
		if (!_pthread_rwlock_deadline(abstime, &then))
		{
			LOCK(rwlock->lock);
			rwlock->blocked_writers--;
			UNLOCK(rwlock->lock);
			return (ETIMEDOUT);
		}
		MACH_CALL(semaphore_timedwait(rwlock->writer_sem, then), kr);
		if (kr == KERN_OPERATION_TIMED_OUT)
		{
			LOCK(rwlock->lock);
			rwlock->blocked_writers--;
			UNLOCK(rwlock->lock);
			return (ETIMEDOUT);
		}
		LOCK(rwlock->lock);
	}
	rwlock->blocked_writers--;
	rwlock->writer = 1;
	UNLOCK(rwlock->lock);
	return (ESUCCESS);
}

int
pthread_rwlock_wrlock(pthread_rwlock_t *rwlock)
{
	kern_return_t kr;

	if (rwlock->sig != _PTHREAD_RWLOCK_SIG)
		return (EINVAL);

	LOCK(rwlock->lock);
	rwlock->blocked_writers++;
	while (rwlock->writer || rwlock->readers > 0) {
		UNLOCK(rwlock->lock);
		MACH_CALL(semaphore_wait(rwlock->writer_sem), kr);
		LOCK(rwlock->lock);
	}
	rwlock->blocked_writers--;
	rwlock->writer = 1;
	UNLOCK(rwlock->lock);
	return (ESUCCESS);
}

int
pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock)
{
	if (rwlock->sig != _PTHREAD_RWLOCK_SIG)
		return (EINVAL);

	LOCK(rwlock->lock);
	if (rwlock->writer || rwlock->readers > 0) {
		UNLOCK(rwlock->lock);
		return (EBUSY);
	}
	rwlock->writer = 1;
	UNLOCK(rwlock->lock);
	return (ESUCCESS);
}

int
pthread_rwlock_unlock(pthread_rwlock_t *rwlock)
{
	kern_return_t kr;

	if (rwlock->sig != _PTHREAD_RWLOCK_SIG)
		return (EINVAL);

	LOCK(rwlock->lock);
	if (rwlock->writer) {
		/* Writer releasing — prefer waiting writers */
		rwlock->writer = 0;
		if (rwlock->blocked_writers > 0) {
			UNLOCK(rwlock->lock);
			MACH_CALL(semaphore_signal(rwlock->writer_sem), kr);
		} else {
			UNLOCK(rwlock->lock);
			/* Wake all blocked readers */
			MACH_CALL(semaphore_signal_all(rwlock->reader_sem), kr);
		}
	} else if (rwlock->readers > 0) {
		rwlock->readers--;
		if (rwlock->readers == 0 && rwlock->blocked_writers > 0) {
			UNLOCK(rwlock->lock);
			MACH_CALL(semaphore_signal(rwlock->writer_sem), kr);
		} else {
			UNLOCK(rwlock->lock);
		}
	} else {
		UNLOCK(rwlock->lock);
		return (EINVAL);
	}
	return (ESUCCESS);
}
