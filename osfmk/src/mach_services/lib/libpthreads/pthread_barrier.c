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
 *   Barrier support (POSIX.1-2001)
 *
 * A barrier blocks until 'count' threads have called wait().
 * One thread receives PTHREAD_BARRIER_SERIAL_THREAD, the rest get 0.
 * Uses a phase toggle to allow safe reuse after release.
 */

#include "pthread_internals.h"

int
pthread_barrierattr_init(pthread_barrierattr_t *attr)
{
	attr->sig = _PTHREAD_BARRIER_ATTR_SIG;
	attr->pshared = 0;
	return (ESUCCESS);
}

int
pthread_barrierattr_destroy(pthread_barrierattr_t *attr)
{
	attr->sig = _PTHREAD_NO_SIG;
	return (ESUCCESS);
}

int
pthread_barrierattr_getpshared(const pthread_barrierattr_t *attr, int *pshared)
{
	if (attr->sig != _PTHREAD_BARRIER_ATTR_SIG)
		return (EINVAL);
	if (pshared != (int *)NULL)
		*pshared = attr->pshared;
	return (ESUCCESS);
}

int
pthread_barrierattr_setpshared(pthread_barrierattr_t *attr, int pshared)
{
	if (attr->sig != _PTHREAD_BARRIER_ATTR_SIG)
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
pthread_barrier_init(pthread_barrier_t *barrier,
		     const pthread_barrierattr_t *attr,
		     unsigned count)
{
	kern_return_t kr;

	if (count == 0)
		return (EINVAL);

	LOCK_INIT(barrier->lock);
	barrier->sig = _PTHREAD_BARRIER_SIG;
	barrier->count = (int)count;
	barrier->waiting = 0;
	barrier->phase = 0;

	MACH_CALL(semaphore_create(mach_task_self(),
				   &barrier->sem,
				   SYNC_POLICY_FIFO, 0), kr);
	if (kr != KERN_SUCCESS)
		return (ENOMEM);

	return (ESUCCESS);
}

int
pthread_barrier_destroy(pthread_barrier_t *barrier)
{
	kern_return_t kr;

	if (barrier->sig != _PTHREAD_BARRIER_SIG)
		return (EINVAL);

	LOCK(barrier->lock);
	if (barrier->waiting != 0) {
		UNLOCK(barrier->lock);
		return (EBUSY);
	}
	barrier->sig = _PTHREAD_NO_SIG;
	UNLOCK(barrier->lock);

	MACH_CALL(semaphore_destroy(mach_task_self(),
				    barrier->sem), kr);
	return (ESUCCESS);
}

int
pthread_barrier_wait(pthread_barrier_t *barrier)
{
	kern_return_t kr;
	int my_phase;

	if (barrier->sig != _PTHREAD_BARRIER_SIG)
		return (EINVAL);

	LOCK(barrier->lock);
	my_phase = barrier->phase;
	barrier->waiting++;

	if (barrier->waiting == barrier->count) {
		/* Last thread: release everyone */
		barrier->waiting = 0;
		__atomic_store_n(&barrier->phase, !my_phase,
				 __ATOMIC_RELEASE);
		UNLOCK(barrier->lock);
		/* Wake all (count - 1) blocked threads */
		MACH_CALL(semaphore_signal_all(barrier->sem), kr);
		return (PTHREAD_BARRIER_SERIAL_THREAD);
	}

	/* Not last: block until phase changes (spurious-wakeup safe) */
	UNLOCK(barrier->lock);
	do {
		MACH_CALL(semaphore_wait(barrier->sem), kr);
	} while (__atomic_load_n(&barrier->phase, __ATOMIC_ACQUIRE)
		 == my_phase);
	return (ESUCCESS);
}
